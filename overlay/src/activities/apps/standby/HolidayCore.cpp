#include "HolidayCore.h"

#include <ArduinoJson.h>

#include <cstdlib>
#include <cstring>

namespace holiday_core {

using calendar_core::Lang;

// ---- Хранилище лет -----------------------------------------------------------

const YearData* Store::find(int year) const {
  for (uint8_t i = 0; i < count; ++i) {
    if (years[i].year == year) return &years[i];
  }
  return nullptr;
}

YearData* Store::put(int year, int keepNear) {
  for (uint8_t i = 0; i < count; ++i) {
    if (years[i].year == year) return &years[i];
  }
  if (count < kMaxYears) {
    years[count] = YearData{};
    years[count].year = static_cast<int16_t>(year);
    return &years[count++];
  }
  uint8_t far = 0;
  int farDist = -1;
  for (uint8_t i = 0; i < count; ++i) {
    const int dist = std::abs(years[i].year - keepNear);
    if (dist > farDist) {
      farDist = dist;
      far = i;
    }
  }
  years[far] = YearData{};
  years[far].year = static_cast<int16_t>(year);
  return &years[far];
}

// ---- Разбор ответа -----------------------------------------------------------

ParseResult parseYear(const char* body, size_t len, int year, uint32_t nowEpoch, YearData& out) {
  while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r' || body[len - 1] == ' ')) --len;
  const unsigned days = calendar_core::daysInYear(year);
  if (!body || len != days) return ParseResult::Bad;

  YearData yd;
  yd.year = static_cast<int16_t>(year);
  yd.fetchedEpoch = nowEpoch;
  unsigned off = 0;
  for (unsigned i = 0; i < days; ++i) {
    Code c;
    switch (body[i]) {
      case '0':
      case '4':  // «рабочий» для шестидневки — для нас тоже рабочий
        c = Code::Work;
        break;
      case '1':
        c = Code::Off;
        ++off;
        break;
      case '2':
        c = Code::Short;
        break;
      default:
        return ParseResult::Bad;
    }
    yd.set(i, c);
  }
  yd.published = off >= kMinOffDays;
  if (!yd.published) {
    std::memset(yd.bits, 0, sizeof(yd.bits));  // «ничего не знаем»: не держим нули за данные
  }
  out = yd;
  return yd.published ? ParseResult::Ok : ParseResult::Unpublished;
}

bool needsFetch(const Store& s, int year, int currentYear, uint32_t nowEpoch, unsigned refreshDays) {
  const YearData* y = s.find(year);
  if (!y) return true;
  if (y->published && year < currentYear) return false;  // опубликованное прошлое не меняется
  if (nowEpoch < y->fetchedEpoch) return true;            // часы «ушли назад»
  return nowEpoch - y->fetchedEpoch >= refreshDays * 86400u;
}

// ---- Фиксированные праздники (Россия) ----------------------------------------

namespace {

struct Fixed {
  uint8_t month, day;
  const char* en;
  const char* ru;
  const char* de;
};

const Fixed kFixedRu[] = {
    {1, 1, "New Year's Day", "Новый год", "Neujahr"},
    {1, 2, "New Year holidays", "Новогодние каникулы", "Neujahrsferien"},
    {1, 7, "Orthodox Christmas", "Рождество Христово", "Orthodoxes Weihnachten"},
    {2, 23, "Defender of the Fatherland Day", "День защитника Отечества", "Tag des Vaterlandsverteidigers"},
    {3, 8, "International Women's Day", "Международный женский день", "Internationaler Frauentag"},
    {5, 1, "Spring and Labour Day", "Праздник Весны и Труда", "Tag des Frühlings und der Arbeit"},
    {5, 9, "Victory Day", "День Победы", "Tag des Sieges"},
    {6, 12, "Russia Day", "День России", "Tag Russlands"},
    {11, 4, "Unity Day", "День народного единства", "Tag der Volkseinheit"},
};

bool isRu(const char* cc) { return cc && (cc[0] == 'r' || cc[0] == 'R') && (cc[1] == 'u' || cc[1] == 'U') && cc[2] == '\0'; }

int fixedIndex(const char* cc, unsigned m, unsigned d) {
  if (!isRu(cc)) return -1;
  if (m == 1 && d >= 1 && d <= 8) return d == 1 ? 0 : d == 7 ? 2 : 1;  // 1–8 января: каникулы, 1-е и 7-е — по имени
  for (size_t i = 3; i < sizeof(kFixedRu) / sizeof(kFixedRu[0]); ++i) {
    if (kFixedRu[i].month == m && kFixedRu[i].day == d) return static_cast<int>(i);
  }
  return -1;
}

struct Txt {
  const char* en;
  const char* ru;
  const char* de;
};
const Txt kTransferOff = {"Day off (transferred)", "Перенос выходного дня", "Verlegter freier Tag"};
const Txt kWorkWeekend = {"Working day (transferred)", "Рабочий день (перенос)", "Verlegter Arbeitstag"};
const Txt kShortDay = {"Shortened working day", "Сокращённый рабочий день", "Verkürzter Arbeitstag"};

const char* pick(Lang l, const Txt& t) { return l == Lang::Ru ? t.ru : l == Lang::De ? t.de : t.en; }

}  // namespace

DayInfo classify(const Store& s, const char* cc, int year, unsigned month, unsigned day) {
  DayInfo r;
  const unsigned wd = calendar_core::weekday(year, month, day);  // 0 = Пн
  const bool weekend = wd >= 5;
  const int fx = fixedIndex(cc, month, day);
  r.fixedIdx = static_cast<int8_t>(fx);
  r.holiday = fx >= 0;

  const YearData* yd = s.find(year);
  if (!yd || !yd->published) {
    r.known = false;
    r.off = weekend || r.holiday;
    return r;
  }
  r.known = true;
  const Code c = yd->at(calendar_core::dayOfYear(year, month, day) - 1);
  r.off = (c == Code::Off);
  r.shortDay = (c == Code::Short);
  r.workWeekend = (c != Code::Off) && weekend;
  r.transferOff = r.off && !weekend && !r.holiday;
  return r;
}

const char* label(Lang lang, const DayInfo& d) {
  if (d.fixedIdx >= 0 && d.fixedIdx < static_cast<int8_t>(sizeof(kFixedRu) / sizeof(kFixedRu[0]))) {
    const Fixed& f = kFixedRu[d.fixedIdx];
    return lang == Lang::Ru ? f.ru : lang == Lang::De ? f.de : f.en;
  }
  if (d.transferOff) return pick(lang, kTransferOff);
  if (d.workWeekend) return pick(lang, kWorkWeekend);
  if (d.shortDay) return pick(lang, kShortDay);
  return "";
}

// ---- Кэш ---------------------------------------------------------------------

std::string serialize(const Store& s, const char* cc) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["cc"] = cc;
  JsonArray arr = doc["y"].to<JsonArray>();
  for (uint8_t i = 0; i < s.count; ++i) {
    const YearData& y = s.years[i];
    JsonObject o = arr.add<JsonObject>();
    o["y"] = y.year;
    o["p"] = y.published ? 1 : 0;
    o["at"] = y.fetchedEpoch;
    if (y.published) {
      std::string d;
      const unsigned n = calendar_core::daysInYear(y.year);
      d.reserve(n);
      for (unsigned k = 0; k < n; ++k) d.push_back(static_cast<char>('0' + static_cast<unsigned>(y.at(k))));
      o["d"] = d;
    }
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parse(const char* json, size_t len, const char* cc, Store& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;
  if (std::strcmp(doc["cc"] | "", cc) != 0) return false;
  Store st;
  for (JsonObjectConst o : doc["y"].as<JsonArrayConst>()) {
    const int year = o["y"] | 0;
    if (year < calendar_core::kMinYear || year > calendar_core::kMaxYear) continue;
    YearData yd;
    yd.year = static_cast<int16_t>(year);
    yd.fetchedEpoch = o["at"] | 0u;
    if ((o["p"] | 0) != 0) {
      const char* d = o["d"] | "";
      const unsigned n = calendar_core::daysInYear(year);
      if (std::strlen(d) != n) continue;
      bool ok = true;
      for (unsigned k = 0; k < n && ok; ++k) {
        if (d[k] < '0' || d[k] > '2') ok = false;
        else yd.set(k, static_cast<Code>(d[k] - '0'));
      }
      if (!ok) continue;
      yd.published = true;
    }
    YearData* slot = st.put(year, year);
    if (slot) *slot = yd;
  }
  out = st;
  return true;
}

}  // namespace holiday_core
