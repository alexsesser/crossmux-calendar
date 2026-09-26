#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Чистая часть HTTP-клиента календаря (без Arduino/SDK): разбор URL и заголовков ответа, выделение тела
// (Content-Length / chunked / до закрытия соединения), распаковка gzip. Сетевая часть — CalendarHttp.
// Host-тест — tests/test_http.cpp.
namespace http_core {

struct Url {
  bool https = true;
  std::string host;
  uint16_t port = 443;
  std::string path;  // вместе со строкой запроса: "/v1/forecast?latitude=…"
};
bool parseUrl(const char* url, Url& out);

struct Head {
  int status = 0;
  bool chunked = false;      // Transfer-Encoding: chunked
  bool gzip = false;         // Content-Encoding: gzip
  long contentLength = -1;   // -1 — длина не указана
  size_t bodyStart = 0;      // где в сыром ответе начинается тело
};
// false — заголовки пришли ещё не целиком (или это вообще не HTTP-ответ).
bool parseHead(const std::string& raw, Head& out);

enum class Body : uint8_t { Incomplete, Complete, Bad };
// Тело из сырого ответа (после parseHead). closed — сервер уже закрыл соединение: для ответа без длины это и есть
// конец тела, для остальных — обрыв.
Body body(const std::string& raw, const Head& h, bool closed, std::string& out);

// gzip → out. false — не gzip, повреждён (CRC) или распакованный размер больше maxOut.
bool gunzip(const std::string& in, std::string& out, size_t maxOut);

}  // namespace http_core
