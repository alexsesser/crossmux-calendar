// Тесты HolidayCore: настоящий ответ isdayoff.ru за 2026, «нулевой» ответ неопубликованного года, мусор, классификация, кэш.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "HolidayCore.h"
using namespace holiday_core;
using calendar_core::Lang;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
static std::string slurp(const std::string& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "data";
  const std::string b26 = slurp(dir + "/isdayoff_2026.txt"), b28 = slurp(dir + "/isdayoff_2028_unpublished.txt");

  Store st;
  { YearData y; CHECK(parseYear(b26.data(), b26.size(), 2026, 1000, y) == ParseResult::Ok); CHECK(y.published && y.year == 2026 && y.fetchedEpoch == 1000);
    *st.put(2026, 2026) = y; }
  { YearData y; CHECK(parseYear(b28.data(), b28.size(), 2028, 1000, y) == ParseResult::Unpublished); CHECK(!y.published);
    *st.put(2028, 2026) = y; }

  // --- классификация по опубликованному 2026 ---
  auto C = [&](int y, unsigned m, unsigned d) { return classify(st, "ru", y, m, d); };
  { auto d = C(2026, 1, 1); CHECK(d.known && d.off && d.holiday && std::string(label(Lang::Ru, d)) == "Новый год"); }
  { auto d = C(2026, 1, 7); CHECK(d.off && d.holiday && std::string(label(Lang::En, d)) == "Orthodox Christmas"); }
  { auto d = C(2026, 1, 5); CHECK(d.off && d.holiday && std::string(label(Lang::Ru, d)) == "Новогодние каникулы"); }
  { auto d = C(2026, 1, 9); CHECK(d.off && !d.holiday && d.transferOff && std::string(label(Lang::Ru, d)) == "Перенос выходного дня"); }  // пт
  { auto d = C(2026, 3, 8); CHECK(d.off && d.holiday && std::string(label(Lang::Ru, d)) == "Международный женский день"); }             // вс
  { auto d = C(2026, 3, 9); CHECK(d.off && d.transferOff); }                                                                             // пн — перенос
  { auto d = C(2026, 5, 9); CHECK(d.off && d.holiday && std::string(label(Lang::De, d)) == "Tag des Sieges"); }                          // сб
  { auto d = C(2026, 9, 21); CHECK(d.known && !d.off && !d.holiday && std::string(label(Lang::Ru, d)).empty()); }                        // обычный пн
  { auto d = C(2026, 9, 19); CHECK(d.off && !d.holiday && !d.transferOff && !d.workWeekend); }                                           // обычная сб
  // все 118 нерабочих дней 2026 из ответа — ровно те, что классифицируются как off
  { unsigned off = 0; for (unsigned m = 1; m <= 12; ++m) for (unsigned d = 1; d <= calendar_core::daysInMonth(2026, m); ++d) off += C(2026, m, d).off ? 1u : 0u; CHECK(off == 118); }
  // рабочая суббота/воскресенье и сокращённые дни — если есть в календаре: их число должно совпадать с ответом
  { unsigned ww = 0, sh = 0; for (unsigned m = 1; m <= 12; ++m) for (unsigned d = 1; d <= calendar_core::daysInMonth(2026, m); ++d) { auto x = C(2026, m, d); ww += x.workWeekend; sh += x.shortDay; if (x.workWeekend) CHECK(!x.off); }
    unsigned twos = 0; for (char ch : b26) twos += ch == '2'; CHECK(sh == twos); std::printf("info: рабочих выходных %u, сокращённых %u\n", ww, sh); }

  // --- запасной вариант: 2028 не опубликован, 2030 нет в кэше ---
  { auto d = C(2028, 5, 9); CHECK(!d.known && d.off && d.holiday); }                       // вт, фиксированный
  { auto d = C(2028, 1, 3); CHECK(!d.known && d.off && d.holiday); }                       // каникулы
  { auto d = C(2028, 9, 16); CHECK(!d.known && d.off && !d.holiday); }                     // сб
  { auto d = C(2028, 9, 18); CHECK(!d.known && !d.off); }                                   // пн
  { auto d = classify(st, "ru", 2030, 2, 23); CHECK(!d.known && d.off && d.holiday); }
  { auto d = classify(st, "by", 2028, 5, 9); CHECK(!d.known && !d.holiday && !d.off); }    // другая страна: фиксированных нет (вт)
  { auto d = classify(st, "by", 2028, 5, 13); CHECK(!d.known && d.off); }                   // но выходные — да

  // --- разбор: мусор ---
  { YearData keep; keep.year = 7; keep.published = true;
    for (const char* bad : {"", "100", "101", "199", "abc", "0101"}) { YearData y = keep; CHECK(parseYear(bad, std::strlen(bad), 2026, 1, y) == ParseResult::Bad); CHECK(y.year == 7); }
    std::string wrongLen = b26.substr(0, 364); YearData y = keep; CHECK(parseYear(wrongLen.data(), wrongLen.size(), 2026, 1, y) == ParseResult::Bad);
    std::string badChar = b26; badChar[10] = '9'; y = keep; CHECK(parseYear(badChar.data(), badChar.size(), 2026, 1, y) == ParseResult::Bad);
    std::string leap(366, '0'); y = keep; CHECK(parseYear(leap.data(), leap.size(), 2028, 1, y) == ParseResult::Unpublished);
    std::string wrong365 = std::string(365, '1'); y = keep; CHECK(parseYear(wrong365.data(), wrong365.size(), 2028, 1, y) == ParseResult::Bad);   // 2028 — високосный
    std::string nl = b26 + "\n"; y = keep; CHECK(parseYear(nl.data(), nl.size(), 2026, 1, y) == ParseResult::Ok); }

  // --- когда запрашивать ---
  { Store s2; CHECK(needsFetch(s2, 2026, 2026, 100, 30)); *s2.put(2026, 2026) = *st.find(2026);
    CHECK(!needsFetch(s2, 2026, 2026, 1000 + 29 * 86400u, 30)); CHECK(needsFetch(s2, 2026, 2026, 1000 + 31 * 86400u, 30));
    CHECK(!needsFetch(s2, 2026, 2027, 1000 + 400 * 86400u, 30));             // опубликованное прошлое — не трогаем
    *s2.put(2028, 2026) = *st.find(2028); CHECK(!needsFetch(s2, 2028, 2026, 1000 + 10 * 86400u, 30)); CHECK(needsFetch(s2, 2028, 2026, 1000 + 40 * 86400u, 30)); }

  // --- кэш туда-обратно ---
  { const std::string j = serialize(st, "ru"); Store r; CHECK(parse(j.data(), j.size(), "ru", r));
    CHECK(r.count == 2 && r.find(2026) && r.find(2026)->published && r.find(2028) && !r.find(2028)->published);
    CHECK(std::memcmp(r.find(2026)->bits, st.find(2026)->bits, sizeof(r.find(2026)->bits)) == 0 && r.find(2026)->fetchedEpoch == 1000);
    Store other; CHECK(!parse(j.data(), j.size(), "by", other) && other.count == 0);        // чужая страна — отбрасываем
    for (const char* bad : {"", "{", "[]", "{\"cc\":\"ru\",\"y\":[{\"y\":2026,\"p\":1,\"d\":\"01\"}]}"}) { Store t; bool ok = parse(bad, std::strlen(bad), "ru", t); CHECK(!ok || t.count == 0); } }

  // --- вытеснение: при переполнении уходит самый далёкий от текущего ---
  { Store s3; for (int y = 2000; y < 2000 + kMaxYears; ++y) s3.put(y, 2026)->published = true; CHECK(s3.count == kMaxYears);
    s3.put(2026, 2026); CHECK(s3.find(2000) == nullptr && s3.find(2026) && s3.count == kMaxYears); }

  // --- размер ---
  CHECK(sizeof(YearData) <= 104); std::printf("holidays: ошибок %d, sizeof(Store)=%zu\n", fails, sizeof(Store));
  return fails;
}
