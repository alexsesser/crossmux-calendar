#pragma once

#include <GfxRenderer.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "CalendarCore.h"
#include "CalendarFontMetrics.h"
#include "WeatherCore.h"
#include "components/themes/BaseTheme.h"  // Rect
#include "fontIds.h"

// Общие функции отрисовки грани «Календарь»: шрифты, крупные цифры, иконки погоды, строки-«чипы», статус-строка.
// Используются главным экраном (CalendarFace.cpp) и вложенными (CalendarDetail.cpp).
namespace cal_draw {

using calendar_core::Lang;

// ВАЖНО: NOTOSANS_12..18 в этой прошивке — один и тот же notosans_cjk_12 (ASCII + CJK, без кириллицы и без
// жирного; см. main.cpp: insertFont(NOTOSANS_*, offlineReaderFontFamily)). Кириллицу и bold дают только UI_10/UI_12.
constexpr int kFontText = UI_12_FONT_ID;   // основной текст, заголовки (жирный — kBold)
constexpr int kFontSmall = UI_10_FONT_ID;  // чипсы, шапка сетки, соседние месяцы, детали погоды

constexpr auto kBold = EpdFontFamily::BOLD;
constexpr auto kRegular = EpdFontFamily::REGULAR;

// «Нет значения» в интерфейсе. Тире из общей пунктуации есть не во всех подмножествах шрифта — берём ASCII.
constexpr const char* kDash = "--";

// id сгенерированных шрифтов крупных цифр (см. tools/gen_digit_fonts.sh).
constexpr int kFontTimeXl = 0x43414C58;  // время, портрет
constexpr int kFontTimeL = 0x43414C4C;   // время, ландшафт
constexpr int kFontTemp = 0x43414C54;    // температура
constexpr int kFontDay = 0x43414C44;     // числа в сетке месяца
using calendar_fonts::kDayDigitH;
using calendar_fonts::kDayTopOffset;
using calendar_fonts::kTempDigitH;
using calendar_fonts::kTempTopOffset;
using calendar_fonts::kTimeLDigitH;
using calendar_fonts::kTimeLTopOffset;
using calendar_fonts::kTimeXlDigitH;
using calendar_fonts::kTimeXlTopOffset;

// Разбор «сейчас» — один раз за кадр (из снимка часов грани).
struct Today {
  int year;
  unsigned month, day, hour, minute, weekday, dayOfYear, isoWeek, daysInYear;
  int utcOffsetMin;
  uint32_t epoch;
};

Lang currentLang();
int textW(const GfxRenderer& r, int font, const char* s, EpdFontFamily::Style st = kRegular);
// Текст по центру горизонтального отрезка [x, x+w); y — верх строки.
void drawCentered(const GfxRenderer& r, int font, int x, int w, int y, const char* s, bool black = true,
                  EpdFontFamily::Style st = kRegular);

// Текст, обрезанный «…» под ширину. Пока текст помещается (почти всегда) — это просто указатель на исходную строку,
// без выделения памяти; копия создаётся только когда не влезло.
class Fit {
 public:
  Fit(const GfxRenderer& r, int font, const char* text, int maxW, EpdFontFamily::Style st = kRegular) : p_(text) {
    if (r.getTextWidth(font, text, st) > maxW) {
      tmp_ = r.truncatedText(font, text, maxW, st);
      p_ = tmp_.c_str();
    }
  }
  Fit(const Fit&) = delete;
  Fit& operator=(const Fit&) = delete;
  const char* c_str() const { return p_; }

 private:
  std::string tmp_;
  const char* p_;
};

// Первые n символов UTF-8 строки в буфер («сентября» → «сен»).
void abbrTo(char* out, size_t outSize, const char* s, int n);

// Шрифты крупных цифр регистрируются в рендерере один раз; вызывать в начале каждого render().
void ensureDigitFonts(GfxRenderer& r);
// Время «ЧЧ:ММ» по центру [x, x+w); y — верх цифр.
void drawBigTime(GfxRenderer& r, int font, int topOffset, int x, int y, int w, unsigned hh, unsigned mm);
// Температура «18°» / «-3°» / «--»; y — верх цифр. Возвращает ширину.
int drawTemperature(GfxRenderer& r, int x, int y, float t);

// Верхняя строка: слева часовой пояс, справа заряд с процентами.
void drawStatusRow(GfxRenderer& r, const Rect& vp, int utcOffsetMin);

// Иконки погоды примитивами; s — сторона квадрата иконки.
void drawWeatherIcon(const GfxRenderer& r, weather_core::Icon icon, int x, int y, int s);
// Луна по фракции цикла 0..1 (0 — новолуние, 0.5 — полнолуние); rad — радиус диска.
void drawMoonPhase(const GfxRenderer& r, double fraction, int cx, int cy, int rad);
// Залитый круг.
void disc(const GfxRenderer& r, int cx, int cy, int rad, Color c);

// Максимум элементов в строке значков (RichItems).
constexpr int kMaxItems = 6;

// ---- Значки вместо слов (мин / макс / ветер / осадки / восход / закат): читаются издалека и не отнимают ширину ----
enum class Glyph : uint8_t { None, TempMin, TempMax, Wind, Drop, Sunrise, Sunset };
// Значок в квадрате s×s с левым верхним углом (x, y).
void drawGlyph(const GfxRenderer& r, Glyph g, int x, int y, int s);
// Сторона значка для шрифта: чуть выше строки текста.
int glyphSize(const GfxRenderer& r, int font);
// Значок + текст справа от него; ширина такого элемента и его отрисовка (y — верх строки значка).
int glyphTextW(const GfxRenderer& r, int font, Glyph g, const char* text, EpdFontFamily::Style st = kRegular);
void drawGlyphText(const GfxRenderer& r, int font, Glyph g, int x, int y, const char* text, EpdFontFamily::Style st = kRegular);

// Строка из элементов «значок + текст» (значок необязателен), переносится по ширине. Возвращает высоту.
struct RichItems {
  struct It {
    Glyph g;
    char s[36];
  } it[kMaxItems];
  int n = 0;
  void add(Glyph g, const char* fmt, const char* a = "", const char* b = "", const char* c = "") {
    if (n < kMaxItems) {
      it[n].g = g;
      std::snprintf(it[n].s, sizeof(it[0].s), fmt, a, b, c);
      ++n;
    }
  }
};
// center — по центру [x, x+w); иначе прижато к x.
int drawRichRows(const GfxRenderer& r, int font, int x, int w, int y, const RichItems& items,
                 EpdFontFamily::Style st = kRegular, bool center = true);

// «17°» / «--»; «0,4» (ru/de) или «0.4» (en) миллиметры.
void fmtDeg(char* out, size_t n, float v);
void fmtMm(char* out, size_t n, float v, Lang lang);

}  // namespace cal_draw
