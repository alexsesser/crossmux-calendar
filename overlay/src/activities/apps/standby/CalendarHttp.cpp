#include "CalendarHttp.h"

#include <Arduino.h>

#include <cstdio>

#include "CalendarConfig.h"
#include "HttpCore.h"

#if CROSSPOINT_EMULATED == 0
#include <SecureClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#else
#include "network/HttpDownloader.h"
#endif

namespace cal_http {

namespace {

constexpr size_t kHeadRoom = 16384;  // заголовки и обёртка chunked сверх тела (не настройка)

bool isGzip(const std::string& s) {
  return s.size() >= 2 && static_cast<uint8_t>(s[0]) == 0x1F && static_cast<uint8_t>(s[1]) == 0x8B;
}

}  // namespace

#if CROSSPOINT_EMULATED == 0

bool get(const Request& rq, std::string& out, Result& r) {
  r = Result{};
  out.clear();
  const uint32_t t0 = millis();
  auto finish = [&](Stage s) {
    r.stage = s;
    r.totalMs = millis() - t0;
    return s == Stage::Ok;
  };
  auto aborted = [&] { return rq.abort && rq.abort(); };

  http_core::Url u;
  if (!http_core::parseUrl(rq.url, u)) return finish(Stage::BadUrl);
  r.port = u.port;

  IPAddress ip;
  uint32_t t = millis();
  const bool resolved = WiFi.hostByName(u.host.c_str(), ip) == 1;
  r.dnsMs = millis() - t;
  if (!resolved) return finish(Stage::Dns);
  std::snprintf(r.ip, sizeof(r.ip), "%s", ip.toString().c_str());
  if (aborted()) return finish(Stage::Aborted);

  // TCP — отдельным шагом даже для HTTPS: так видно, недоступен ли сервер из этой сети вообще (TCP) или рвётся/виснет
  // шифрование (TLS). Лишнее соединение стоит одного обмена пакетами.
  WiFiClient plain;
  t = millis();
  const bool tcpOk = plain.connect(ip, u.port, static_cast<int32_t>(rq.stageTimeoutMs)) != 0;
  r.tcpMs = millis() - t;
  if (!tcpOk) return finish(Stage::Tcp);

  freeink::SecureClient tls;
  Client* c = &plain;
  if (u.https) {
    plain.stop();
    if (aborted()) return finish(Stage::Aborted);
    tls.setInsecure();
    tls.setTimeout(rq.stageTimeoutMs);  // на TCP и на рукопожатие; не вышло — клиент прошивки ещё раз пробует TLS 1.2
    t = millis();
    const bool ok = tls.connect(u.host.c_str(), u.port) != 0;
    r.tlsMs = millis() - t;
    if (!ok) return finish(Stage::Tls);
    c = &tls;
  }

  std::string req;
  req.reserve(u.path.size() + u.host.size() + 200);
  req += "GET ";
  req += u.path;
  req += " HTTP/1.1\r\nHost: ";
  req += u.host;
  req += "\r\nUser-Agent: ";
  req += calendar_config::kHttpUserAgent;
  req += "\r\nAccept: application/json\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n";
  if (c->write(reinterpret_cast<const uint8_t*>(req.data()), req.size()) != req.size()) {
    c->stop();
    return finish(Stage::Send);
  }

  std::string raw;
  raw.reserve(2048);
  const size_t wireMax = rq.maxBytes + kHeadRoom;
  uint8_t buf[1024];
  http_core::Head h;
  bool haveHead = false;
  bool complete = false;
  std::string body;
  const uint32_t sent = millis();
  uint32_t lastData = sent;
  for (;;) {
    if (aborted()) {
      c->stop();
      return finish(Stage::Aborted);
    }
    const int n = c->read(buf, sizeof(buf));
    if (n > 0) {
      if (raw.empty()) r.answerMs = millis() - sent;
      raw.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
      lastData = millis();
      if (raw.size() > wireMax) {
        c->stop();
        r.rawBytes = raw.size();
        return finish(Stage::TooBig);
      }
      if (!haveHead) haveHead = http_core::parseHead(raw, h);
      if (haveHead && http_core::body(raw, h, false, body) == http_core::Body::Complete) {
        complete = true;
        break;
      }
      continue;
    }
    if (!c->connected() && c->available() == 0) {
      r.closed = true;
      break;
    }
    const uint32_t now = millis();
    // Тишина дольше этапа или ответ «по капле» дольше трёх этапов — хватит.
    if (now - lastData >= rq.stageTimeoutMs || now - sent >= 3 * rq.stageTimeoutMs) break;
    delay(5);
  }
  c->stop();  // память TLS — обратно до разбора
  r.rawBytes = raw.size();

  if (!haveHead) haveHead = http_core::parseHead(raw, h);
  if (!haveHead) return finish(raw.empty() ? Stage::NoAnswer : Stage::Body);
  r.status = h.status;
  if (!complete && http_core::body(raw, h, r.closed, body) != http_core::Body::Complete) return finish(Stage::Body);
  std::string().swap(raw);
  r.wireBytes = body.size();
  if (h.gzip || isGzip(body)) {
    r.gzip = true;
    if (!http_core::gunzip(body, out, rq.maxBytes)) return finish(Stage::Gzip);
  } else {
    out.swap(body);
  }
  r.bodyBytes = out.size();
  if (h.status != 200) return finish(Stage::Status);
  if (out.size() > rq.maxBytes) return finish(Stage::TooBig);
  return finish(Stage::Ok);
}

#else  // симулятор: HttpDownloader там подменён (файлы из CROSSPOINT_SIM_HTTP_MOCK_ROOT или curl)

bool get(const Request& rq, std::string& out, Result& r) {
  r = Result{};
  const uint32_t t0 = millis();
  http_core::Url u;
  if (!http_core::parseUrl(rq.url, u)) {
    r.stage = Stage::BadUrl;
    return false;
  }
  r.port = u.port;
  const bool ok = HttpDownloader::fetchUrl(rq.url, out);
  r.totalMs = millis() - t0;
  r.wireBytes = r.rawBytes = out.size();
  if (!ok) {
    r.stage = Stage::NoAnswer;
    return false;
  }
  if (isGzip(out)) {
    r.gzip = true;
    std::string plain;
    if (!http_core::gunzip(out, plain, rq.maxBytes)) {
      r.stage = Stage::Gzip;
      return false;
    }
    out.swap(plain);
  }
  r.bodyBytes = out.size();
  r.status = 200;
  r.stage = out.size() > rq.maxBytes ? Stage::TooBig : Stage::Ok;
  return r.stage == Stage::Ok;
}

#endif

void describe(const Result& r, char* buf, size_t size) {
  const unsigned long total = r.totalMs;
  switch (r.stage) {
    case Stage::Ok:
      if (!r.ip[0]) {  // симулятор: этапов не видно
        std::snprintf(buf, size, "ok, %u Б%s за %lu мс", static_cast<unsigned>(r.bodyBytes), r.gzip ? " (было сжато)" : "", total);
      } else if (r.gzip) {
        std::snprintf(buf, size, "ok, %u Б (сжато %u); DNS %lu мс, TCP %lu, TLS %lu, ответ через %lu, всего %lu мс [%s]",
                      static_cast<unsigned>(r.bodyBytes), static_cast<unsigned>(r.wireBytes),
                      static_cast<unsigned long>(r.dnsMs), static_cast<unsigned long>(r.tcpMs),
                      static_cast<unsigned long>(r.tlsMs), static_cast<unsigned long>(r.answerMs), total, r.ip);
      } else {
        std::snprintf(buf, size, "ok, %u Б; DNS %lu мс, TCP %lu, TLS %lu, ответ через %lu, всего %lu мс [%s]",
                      static_cast<unsigned>(r.wireBytes), static_cast<unsigned long>(r.dnsMs),
                      static_cast<unsigned long>(r.tcpMs), static_cast<unsigned long>(r.tlsMs),
                      static_cast<unsigned long>(r.answerMs), total, r.ip);
      }
      return;
    case Stage::BadUrl:
      std::snprintf(buf, size, "ОШИБКА: неверный адрес запроса");
      return;
    case Stage::Dns:
      std::snprintf(buf, size, "ОШИБКА DNS: адрес сервера не найден (%lu мс)", static_cast<unsigned long>(r.dnsMs));
      return;
    case Stage::Tcp:
      std::snprintf(buf, size, "ОШИБКА TCP: нет соединения с %s:%u за %lu мс — сервер недоступен из этой сети",
                    r.ip, static_cast<unsigned>(r.port), static_cast<unsigned long>(r.tcpMs));
      return;
    case Stage::Tls:
      std::snprintf(buf, size, "ОШИБКА TLS: TCP к %s:%u есть (%lu мс), а шифрованное соединение не установилось за %lu мс",
                    r.ip, static_cast<unsigned>(r.port), static_cast<unsigned long>(r.tcpMs),
                    static_cast<unsigned long>(r.tlsMs));
      return;
    case Stage::Send:
      std::snprintf(buf, size, "ОШИБКА: соединение с %s есть, запрос не отправился", r.ip);
      return;
    case Stage::NoAnswer:
      if (!r.ip[0]) {  // симулятор: этапов не видно
        std::snprintf(buf, size, "ОШИБКА: запрос не удался за %lu мс", total);
        return;
      }
      std::snprintf(buf, size, "ОШИБКА: соединение с %s есть (TCP %lu мс, TLS %lu), сервер %s; всего %lu мс", r.ip,
                    static_cast<unsigned long>(r.tcpMs), static_cast<unsigned long>(r.tlsMs),
                    r.closed ? "закрыл соединение без ответа" : "не ответил", total);
      return;
    case Stage::Body:
      std::snprintf(buf, size, "ОШИБКА: ответ оборван (%s), получено %u Б за %lu мс [%s]",
                    r.closed ? "сервер закрыл соединение" : "данные перестали приходить", static_cast<unsigned>(r.rawBytes),
                    total, r.ip);
      return;
    case Stage::Status:
      std::snprintf(buf, size, "ОШИБКА: сервер ответил HTTP %d, %u Б за %lu мс [%s]", r.status,
                    static_cast<unsigned>(r.wireBytes), total, r.ip);
      return;
    case Stage::TooBig:
      std::snprintf(buf, size, "ОШИБКА: ответ слишком большой (%u Б)", static_cast<unsigned>(r.rawBytes));
      return;
    case Stage::Gzip:
      std::snprintf(buf, size, "ОШИБКА: сжатый ответ (%u Б) не распаковался", static_cast<unsigned>(r.wireBytes));
      return;
    case Stage::Aborted:
      std::snprintf(buf, size, "прервано: календарь закрыт (%lu мс)", total);
      return;
  }
  std::snprintf(buf, size, "?");
}

}  // namespace cal_http
