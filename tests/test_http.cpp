// Тесты http_core: URL, заголовки ответа, тело (Content-Length / chunked / до закрытия), gzip на настоящем ответе
// MET Norway (tests/data/metno_moscow_complete.json.gz — как пришёл с сервера; .json — он же, распакованный zcat).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "HttpCore.h"
using namespace http_core;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "data";
  // --- URL ---
  {
    Url u;
    CHECK(parseUrl("https://api.open-meteo.com/v1/forecast?latitude=55.75&x=1", u));
    CHECK(u.https && u.host == "api.open-meteo.com" && u.port == 443 && u.path == "/v1/forecast?latitude=55.75&x=1");
    CHECK(parseUrl("http://api.open-meteo.com/v1/forecast?a=b", u) && !u.https && u.port == 80);
    CHECK(parseUrl("http://192.168.1.2:8080", u) && u.host == "192.168.1.2" && u.port == 8080 && u.path == "/");
    for (const char* bad : {"", "ftp://x/", "https:///path", "http://h:0/", "http://h:99999/", "api.met.no/x"}) CHECK(!parseUrl(bad, u));
  }
  // --- Заголовки ---
  {
    Head h;
    CHECK(!parseHead("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", h));  // ещё не целиком
    CHECK(!parseHead("<html>\r\n\r\n", h));
    const std::string r = "HTTP/1.1 200 OK\r\ncontent-length: 5\r\nContent-Encoding: gzip\r\n\r\nhello";
    CHECK(parseHead(r, h) && h.status == 200 && h.contentLength == 5 && h.gzip && !h.chunked && r.substr(h.bodyStart) == "hello");
    CHECK(parseHead("HTTP/1.1 503 Service Unavailable\r\nTransfer-Encoding: chunked\r\n\r\n", h) && h.status == 503 && h.chunked &&
          h.contentLength == -1 && !h.gzip);
  }
  // --- Тело ---
  {
    Head h;
    std::string out;
    std::string r = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello";
    CHECK(parseHead(r, h));
    CHECK(body(r, h, false, out) == Body::Incomplete);
    CHECK(body(r, h, true, out) == Body::Bad);  // оборвано
    r += "world+лишнее";
    CHECK(body(r, h, false, out) == Body::Complete && out == "helloworld");

    // Chunked, как у Open-Meteo: размер в hex, куски, «0».
    std::string c = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n7dd\r\n";
    const std::string big(0x7dd, 'x');
    CHECK(parseHead(c, h) && h.chunked);
    CHECK(body(c, h, false, out) == Body::Incomplete);
    c += big + "\r\n3;ext=1\r\nabc\r\n";
    CHECK(body(c, h, false, out) == Body::Incomplete);  // ещё нет последнего куска
    c += "0\r\n\r\n";
    CHECK(body(c, h, false, out) == Body::Complete && out == big + "abc");
    CHECK(body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", h, true, out) == Body::Bad);
    std::string badTail = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcXY0\r\n\r\n";
    CHECK(parseHead(badTail, h) && body(badTail, h, false, out) == Body::Bad);  // после куска нет CRLF

    // Без длины — до закрытия соединения.
    const std::string u = "HTTP/1.0 200 OK\r\nServer: x\r\n\r\npartial";
    CHECK(parseHead(u, h) && h.contentLength == -1 && !h.chunked);
    CHECK(body(u, h, false, out) == Body::Incomplete);
    CHECK(body(u, h, true, out) == Body::Complete && out == "partial");
  }
  // --- gzip: настоящий ответ MET Norway ---
  {
    const std::string gz = slurp(dir + "/metno_moscow_complete.json.gz");
    const std::string js = slurp(dir + "/metno_moscow_complete.json");
    CHECK(gz.size() > 1000 && js.size() > gz.size());
    std::string out;
    CHECK(gunzip(gz, out, 200000) && out == js);
    CHECK(!gunzip(gz, out, js.size() - 1));  // больше потолка — отказ
    std::string broken = gz;
    broken[broken.size() / 2] ^= 0x55;       // порча в середине — либо ошибка распаковки, либо CRC
    CHECK(!gunzip(broken, out, 200000));
    CHECK(!gunzip("not gzip at all, definitely", out, 1000));
    CHECK(!gunzip(gz.substr(0, 15), out, 200000));
  }
  std::printf("http: ошибок %d\n", fails);
  return fails;
}
