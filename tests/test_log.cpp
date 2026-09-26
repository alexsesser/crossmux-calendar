// cal_log::collect — копирование кольцевого буфера сообщений прошивки (16 ячеек × 256 байт) по ячейкам.
#include <cstdio>
#include <cstring>
#include <string>

#include "../overlay/src/activities/apps/standby/CalendarLog.h"

namespace {

// Как addToLogRingBuffer() в lib/Logging/Logging.cpp.
struct Ring {
  char cells[cal_log::kRingLines][cal_log::kRingEntry] = {};
  size_t head = 0;
  void add(const std::string& s) {
    std::strncpy(cells[head], s.c_str(), cal_log::kRingEntry - 1);
    cells[head][cal_log::kRingEntry - 1] = '\0';
    head = (head + 1) % cal_log::kRingLines;
  }
};

}  // namespace

int main() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    if (!ok) {
      std::printf("FAIL %s\n", what);
      ++fails;
    }
  };
  Ring ring;
  cal_log::RingCursor cur;
  bool lost = true;
  std::string out;

  // Первое чтение: всё, что уже есть, от старой строки к новой.
  ring.add("[100] [INF] [A] one\n");
  ring.add("[200] [ERR] [HTTP] two\n");
  cal_log::collect(ring.cells, ring.head, cur, out, lost);
  check(out == "[100] [INF] [A] one\n[200] [ERR] [HTTP] two\n" && !lost, "первое чтение");

  // Ничего нового — пусто.
  out.clear();
  cal_log::collect(ring.cells, ring.head, cur, out, lost);
  check(out.empty() && !lost, "без изменений пусто");

  // Длинная строка обрезана кольцом без перевода строки — отдельной строкой, и следующая не «склеивается» с ней.
  const std::string url(400, 'u');
  ring.add("[300] [DBG] [HTTP] Fetching: " + url + "\n");
  ring.add("[310] [DBG] [HTTP] wolfSSL GET: " + url + "\n");
  ring.add("[320] [INF] [CLK] synced\n");
  out.clear();
  cal_log::collect(ring.cells, ring.head, cur, out, lost);
  size_t lines = 0;
  for (char ch : out) lines += ch == '\n';
  check(lines == 3 && !lost, "обрезанные длинные строки — по одной");
  check(out.find("[320] [INF] [CLK] synced\n") != std::string::npos, "строка после обрезанной цела");
  out.clear();
  cal_log::collect(ring.cells, ring.head, cur, out, lost);
  check(out.empty() && !lost, "обрезанные строки не повторяются");

  // Одинаковые по тексту сообщения подряд (кроме времени) — все новые.
  ring.add("[400] [DBG] [HTTP] Fetching: X\n");
  ring.add("[401] [DBG] [HTTP] Fetching: X\n");
  out.clear();
  cal_log::collect(ring.cells, ring.head, cur, out, lost);
  check(out == "[400] [DBG] [HTTP] Fetching: X\n[401] [DBG] [HTTP] Fetching: X\n" && !lost, "повторы");

  // Ровно 15 новых строк — ничего не потеряно.
  for (int i = 0; i < 15; ++i) ring.add("[5" + std::to_string(i) + "] [I] [Z] fifteen\n");
  out.clear();
  cal_log::collect(ring.cells, ring.head, cur, out, lost);
  lines = 0;
  for (char ch : out) lines += ch == '\n';
  check(lines == 15 && !lost, "15 строк без потерь");

  // Больше, чем ячеек в кольце — пометка «могло пропасть» и всё кольцо.
  for (int i = 0; i < 20; ++i) ring.add("[6" + std::to_string(i) + "] [I] [Z] twenty\n");
  out.clear();
  cal_log::collect(ring.cells, ring.head, cur, out, lost);
  lines = 0;
  for (char ch : out) lines += ch == '\n';
  check(lines == 16 && lost, "оборот кольца");
  check(out.rfind("[619] [I] [Z] twenty\n") == out.size() - std::strlen("[619] [I] [Z] twenty\n"), "последняя строка — последней");

  // После оборота — снова только новое.
  ring.add("[700] [I] [Z] after\n");
  out.clear();
  cal_log::collect(ring.cells, ring.head, cur, out, lost);
  check(out == "[700] [I] [Z] after\n" && !lost, "после оборота");

  std::printf("log: ошибок %d\n", fails);
  return fails;
}
