#include "HttpCore.h"

#include <uzlib.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace http_core {

namespace {

bool startsWithNoCase(const std::string& s, size_t pos, const char* prefix) {
  const size_t n = std::strlen(prefix);
  if (s.size() - pos < n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(s[pos + i])) != prefix[i]) return false;
  }
  return true;
}

bool containsNoCase(const std::string& s, size_t from, size_t to, const char* needle) {
  const size_t n = std::strlen(needle);
  for (size_t i = from; i + n <= to; ++i) {
    if (startsWithNoCase(s, i, needle)) return true;
  }
  return false;
}

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

}  // namespace

bool parseUrl(const char* url, Url& out) {
  const std::string u(url ? url : "");
  Url r;
  size_t p;
  if (u.compare(0, 8, "https://") == 0) {
    r.https = true;
    r.port = 443;
    p = 8;
  } else if (u.compare(0, 7, "http://") == 0) {
    r.https = false;
    r.port = 80;
    p = 7;
  } else {
    return false;
  }
  const size_t slash = u.find('/', p);
  const std::string hostPort = u.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
  r.path = slash == std::string::npos ? "/" : u.substr(slash);
  const size_t colon = hostPort.rfind(':');
  if (colon != std::string::npos) {
    const long port = std::strtol(hostPort.c_str() + colon + 1, nullptr, 10);
    if (port <= 0 || port > 65535) return false;
    r.port = static_cast<uint16_t>(port);
    r.host = hostPort.substr(0, colon);
  } else {
    r.host = hostPort;
  }
  if (r.host.empty()) return false;
  out = r;
  return true;
}

bool parseHead(const std::string& raw, Head& out) {
  const size_t end = raw.find("\r\n\r\n");
  if (end == std::string::npos) return false;
  if (raw.compare(0, 5, "HTTP/") != 0) return false;
  const size_t sp = raw.find(' ');
  if (sp == std::string::npos || sp > end) return false;
  Head h;
  h.status = std::atoi(raw.c_str() + sp + 1);
  h.bodyStart = end + 4;
  size_t line = raw.find("\r\n");
  while (line != std::string::npos && line < end) {
    const size_t s = line + 2;
    const size_t e = raw.find("\r\n", s);
    if (startsWithNoCase(raw, s, "content-length:")) {
      h.contentLength = std::strtol(raw.c_str() + s + 15, nullptr, 10);
    } else if (startsWithNoCase(raw, s, "transfer-encoding:")) {
      h.chunked = containsNoCase(raw, s, e, "chunked");
    } else if (startsWithNoCase(raw, s, "content-encoding:")) {
      h.gzip = containsNoCase(raw, s, e, "gzip");
    }
    line = e;
  }
  if (h.status < 100 || h.status > 999) return false;
  out = h;
  return true;
}

Body body(const std::string& raw, const Head& h, bool closed, std::string& out) {
  if (raw.size() < h.bodyStart) return closed ? Body::Bad : Body::Incomplete;
  if (h.chunked) {
    std::string b;
    size_t p = h.bodyStart;
    for (;;) {
      const size_t eol = raw.find("\r\n", p);
      if (eol == std::string::npos) return closed ? Body::Bad : Body::Incomplete;
      char* endp = nullptr;
      const unsigned long n = std::strtoul(raw.c_str() + p, &endp, 16);
      if (endp == raw.c_str() + p) return Body::Bad;  // не число
      if (n == 0) break;                            // последний кусок; трейлеры не нужны
      const size_t data = eol + 2;
      if (raw.size() < data + n + 2) return closed ? Body::Bad : Body::Incomplete;
      if (raw.compare(data + n, 2, "\r\n") != 0) return Body::Bad;
      b.append(raw, data, n);
      p = data + n + 2;
    }
    out.swap(b);
    return Body::Complete;
  }
  const size_t have = raw.size() - h.bodyStart;
  if (h.contentLength >= 0) {
    if (have < static_cast<size_t>(h.contentLength)) return closed ? Body::Bad : Body::Incomplete;
    out.assign(raw, h.bodyStart, static_cast<size_t>(h.contentLength));
    return Body::Complete;
  }
  if (!closed) return Body::Incomplete;  // длины нет — тело кончается вместе с соединением
  out.assign(raw, h.bodyStart, std::string::npos);
  return Body::Complete;
}

bool gunzip(const std::string& in, std::string& out, size_t maxOut) {
  const auto* p = reinterpret_cast<const uint8_t*>(in.data());
  const size_t n = in.size();
  // Заголовок RFC 1952: 1F 8B 08, флаги, 6 байт; затем необязательные поля; в конце CRC32 и длина.
  if (n < 18 || p[0] != 0x1F || p[1] != 0x8B || p[2] != 8) return false;
  const uint8_t flags = p[3];
  size_t pos = 10;
  if (flags & 4) {  // FEXTRA
    if (pos + 2 > n) return false;
    pos += 2 + (static_cast<size_t>(p[pos]) | static_cast<size_t>(p[pos + 1]) << 8);
  }
  for (const uint8_t f : {uint8_t{8}, uint8_t{16}}) {  // FNAME, FCOMMENT — строки до нуля
    if (!(flags & f)) continue;
    while (pos < n && p[pos] != 0) ++pos;
    ++pos;
  }
  if (flags & 2) pos += 2;  // FHCRC
  if (pos + 8 > n) return false;
  const uint32_t crc = le32(p + n - 8);
  const uint32_t size = le32(p + n - 4);
  if (size > maxOut) return false;

  std::string o;
  o.resize(size);
  if (size > 0) {
    uzlib_uncomp d{};
    uzlib_init();
    uzlib_uncompress_init(&d, nullptr, 0);  // без кольца: словарь — сам выходной буфер
    d.source = p + pos;
    d.source_limit = p + n - 8;
    d.source_read_cb = nullptr;
    d.dest_start = d.dest = reinterpret_cast<unsigned char*>(&o[0]);
    d.dest_limit = d.dest_start + size;
    const int res = uzlib_uncompress(&d);
    if (res != TINF_DONE && res != TINF_OK) return false;
    if (d.dest != d.dest_limit) return false;
  }
  if (crc32(reinterpret_cast<const uint8_t*>(o.data()), o.size()) != crc) return false;
  out.swap(o);
  return true;
}

}  // namespace http_core
