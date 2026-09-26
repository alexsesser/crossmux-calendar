// cal_log::newSince — что в новом снимке кольцевого буфера сообщений прошивки появилось после прошлого снимка.
#include <cstdio>
#include <string>

#include "../overlay/src/activities/apps/standby/CalendarLog.h"

int main() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    if (!ok) {
      std::printf("FAIL %s\n", what);
      ++fails;
    }
  };
  std::string tail;
  bool lost = true;
  // Первый снимок: всё новое, ничего не потеряно.
  const std::string s1 = "[100] [INF] [A] one\n[200] [ERR] [HTTP] two\n";
  check(cal_log::newSince(s1, tail, lost) == s1 && !lost, "первый снимок целиком");
  // Ничего не изменилось — пусто.
  check(cal_log::newSince(s1, tail, lost).empty() && !lost, "без изменений пусто");
  // Добавились строки (и самые старые ушли из кольца) — только новые.
  const std::string s2 = "[200] [ERR] [HTTP] two\n[300] [DBG] [WX] three\n[400] [DBG] [WX] four\n";
  check(cal_log::newSince(s2, tail, lost) == "[300] [DBG] [WX] three\n[400] [DBG] [WX] four\n" && !lost, "только новые");
  // Кольцо обернулось целиком — всё как новое, с пометкой «могло пропасть».
  const std::string s3 = "[900] [INF] [X] nine\n[950] [INF] [X] ten\n";
  check(cal_log::newSince(s3, tail, lost) == s3 && lost, "полный оборот кольца");
  // Длинные строки (хвост 96 байт ищется внутри последней строки).
  std::string longLine = "[1000] [DBG] [HTTP] Fetching: https://api.open-meteo.com/v1/forecast?latitude=55.7558&longitude=37.6173&current=temperature_2m\n";
  const std::string s4 = s3 + longLine;
  check(cal_log::newSince(s4, tail, lost) == longLine && !lost, "длинная строка");
  const std::string s5 = longLine + "[1100] [INF] [Y] after\n";
  check(cal_log::newSince(s5, tail, lost) == "[1100] [INF] [Y] after\n" && !lost, "после длинной строки");
  // Одинаковые по тексту строки (тот же запрос раз в 5 минут) различаются временем — строки между ними не теряются.
  tail.clear();
  const std::string a = "[10] [DBG] [HTTP] Fetching: X\n";
  check(cal_log::newSince(a, tail, lost) == a, "повтор: первая");
  const std::string b = a + "[20] [INF] [Y] mid\n[30] [DBG] [HTTP] Fetching: X\n";
  check(cal_log::newSince(b, tail, lost) == "[20] [INF] [Y] mid\n[30] [DBG] [HTTP] Fetching: X\n" && !lost, "повтор: между");
  // Строка без перевода строки в конце (обрезана буфером прошивки) — тоже якорь.
  tail.clear();
  (void)cal_log::newSince("[1] [I] [Z] x\n[2] [I] [Z] cut", tail, lost);
  check(cal_log::newSince("[1] [I] [Z] x\n[2] [I] [Z] cut[3] [I] [Z] y\n", tail, lost) == "[3] [I] [Z] y\n" && !lost, "обрезанная строка");
  // Пустой снимок (буфер очищен) — ничего и без пометки.
  check(cal_log::newSince("", tail, lost).empty() && !lost, "пустой снимок");
  std::printf("log: ошибок %d\n", fails);
  return fails;
}
