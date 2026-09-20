#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "CalendarCore.h"

// Праздники и переносы выходных. Чистая логика (без Arduino/SDK); ArduinoJson — header-only, тестируется на хосте.
//
// Источник — производственный календарь isdayoff.ru (без ключа): по одному запросу на ГОД возвращает строку из
// цифр по дням: 0 — рабочий, 1 — нерабочий (выходной, праздник, перенос), 2 — сокращённый (с pre=1).
// ⚠ Для года, который ещё не опубликован (и для лет до ~2004), сервис отвечает всеми нулями БЕЗ ошибки —
// поэтому год считается опубликованным, только если в нём не меньше kMinOffDays нерабочих дней.
// Нет данных — работаем по запасному варианту: выходные + фиксированные праздники (сейчас — Россия).
namespace holiday_core {

constexpr int kMaxYears = 16;         // размер кэша лет (по 92 байта на год)
constexpr unsigned kMinOffDays = 90;  // меньше нерабочих дней в году — календарь не опубликован (одни выходные ≈ 104)

enum class Code : uint8_t { Work = 0, Off = 1, Short = 2, Unknown = 3 };

struct YearData {
  int16_t year = 0;
  bool published = false;     // false: данных нет (год не опубликован) — считаем по запасному варианту
  uint32_t fetchedEpoch = 0;  // когда запрашивали
  uint8_t bits[92] = {};      // 2 бита на день года (Code)

  Code at(unsigned dayIdx) const { return static_cast<Code>((bits[dayIdx >> 2] >> ((dayIdx & 3u) * 2u)) & 3u); }
  void set(unsigned dayIdx, Code c) {
    const unsigned sh = (dayIdx & 3u) * 2u;
    bits[dayIdx >> 2] = static_cast<uint8_t>((bits[dayIdx >> 2] & ~(3u << sh)) | (static_cast<unsigned>(c) << sh));
  }
};

struct Store {
  YearData years[kMaxYears];
  uint8_t count = 0;

  const YearData* find(int year) const;
  // Найти или занять запись под год; если места нет — вытеснить год, наиболее далёкий от keepNear.
  YearData* put(int year, int keepNear);
};

enum class ParseResult { Ok, Unpublished, Bad };
// body — ответ isdayoff.ru за год. Bad — не то (ошибка сервиса, чужой формат, неверная длина); out не меняется.
// Unpublished — ответ верный, но выходных слишком мало: out заполнен как «нет данных».
ParseResult parseYear(const char* body, size_t len, int year, uint32_t nowEpoch, YearData& out);

// Нужно ли (пере)запросить год: нет записи; либо запись «не опубликован»; либо запись старше refreshDays
// (кроме опубликованных прошлых лет — они не меняются).
bool needsFetch(const Store& s, int year, int currentYear, uint32_t nowEpoch, unsigned refreshDays);

struct DayInfo {
  bool known = false;        // есть опубликованный календарь на этот год
  bool off = false;          // нерабочий день — подсвечивать
  bool holiday = false;      // дата праздника (фиксированного)
  bool transferOff = false;  // выходной, перенесённый на будний день
  bool workWeekend = false;  // рабочая суббота/воскресенье (перенос)
  bool shortDay = false;     // сокращённый (предпраздничный) день
  int8_t fixedIdx = -1;      // индекс праздника в таблице фиксированных
};

// cc — страна («ru»). Запасной вариант (нет данных): off = выходные или фиксированный праздник.
DayInfo classify(const Store& s, const char* cc, int year, unsigned month, unsigned day);

// Подпись дня: название праздника / «Перенос выходного дня» / «Рабочий день (перенос)» / «Сокращённый рабочий день».
// Пустая строка — обычный день.
const char* label(calendar_core::Lang lang, const DayInfo& d);

// Кэш на SD: JSON; при чужой стране (cc) кэш отбрасывается.
std::string serialize(const Store& s, const char* cc);
bool parse(const char* json, size_t len, const char* cc, Store& out);  // при false out не меняется

}  // namespace holiday_core
