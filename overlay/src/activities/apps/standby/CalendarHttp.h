#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// HTTP(S) GET календаря с разбором по этапам — чтобы по журналу было видно, где именно застрял запрос: адрес сервера
// (DNS), TCP-соединение, шифрование (TLS), ответ сервера, тело. Штатный HttpDownloader такой разницы не показывает и
// ждёт до 60 с на каждом этапе. Здесь: короткие таймауты этапов (kHttpStageTimeoutSec), прерывание (грань закрыта),
// свой User-Agent (его требует MET Norway), ответ сжатый gzip (в разы меньше данных по воздуху).
// TLS — тот же wolfSSL-клиент прошивки (freeink::SecureClient), без проверки сертификата, как у HttpDownloader.
// В симуляторе — через HttpDownloader: он там подменён чтением файлов/curl.
namespace cal_http {

enum class Stage : uint8_t { Ok, BadUrl, Dns, Tcp, Tls, Send, NoAnswer, Body, Status, TooBig, Gzip, Aborted };

struct Result {
  Stage stage = Stage::BadUrl;
  int status = 0;        // HTTP-код; 0 — ответа не было
  char ip[40] = "";
  uint16_t port = 0;
  uint32_t dnsMs = 0, tcpMs = 0, tlsMs = 0, answerMs = 0, totalMs = 0;
  size_t wireBytes = 0;  // тело, как пришло по сети (до распаковки)
  size_t bodyBytes = 0;  // тело после распаковки
  size_t rawBytes = 0;   // всего получено, с заголовками
  bool gzip = false;     // ответ был сжат
  bool closed = false;   // сервер сам закрыл соединение
};

struct Request {
  const char* url = nullptr;
  size_t maxBytes = 4096;          // потолок тела после распаковки
  uint32_t stageTimeoutMs = 10000; // на ответ сервера и паузы в данных
  uint32_t connectTimeoutMs = 5000; // на TCP-соединение и на TLS-рукопожатие
  std::function<bool()> abort;     // true — бросить запрос (грань закрыта)
};

// true — ответ 200 и тело целиком в out. При ошибке HTTP (например, 503) тело тоже в out — для журнала.
bool get(const Request& rq, std::string& out, Result& res);

// Одна строка для журнала: где и за сколько.
void describe(const Result& r, char* buf, size_t size);

}  // namespace cal_http
