#include "CalendarFace.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "CalendarConfig.h"
#include "CalendarFonts.h"
#include "CalendarCore.h"
#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "SunTimes.h"
#include "WeatherCore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {

using calendar_core::CellKind;
using calendar_core::Lang;
using calendar_core::MonthGrid;
using weather_core::Icon;

// ВАЖНО: NOTOSANS_12..18 в этой прошивке — один и тот же notosans_cjk_12 (ASCII + CJK, без кириллицы и без
// жирного; см. main.cpp: insertFont(NOTOSANS_*, offlineReaderFontFamily)). Кириллицу и bold дают только UI_10/UI_12.
constexpr int kFontText = UI_12_FONT_ID;   // основной текст, заголовки (жирный — kBold)
constexpr int kFontSmall = UI_10_FONT_ID;  // чипсы, шапка сетки, соседние месяцы, детали погоды

constexpr auto kBold = EpdFontFamily::BOLD;
constexpr auto kRegular = EpdFontFamily::REGULAR;

// Раскладка — значения в CalendarConfig.h.
constexpr int kTopReserve = calendar_config::kTopReserve;        // под оверлей активности (заголовок, батарея)
constexpr int kBottomReserve = calendar_config::kBottomReserve;  // под точки-пейджер
constexpr int kSidePad = calendar_config::kSidePad;
constexpr int kCompactPad = calendar_config::kLandscapePad;      // поля левой колонки в ландшафте
constexpr int kLandscapeLeftPct = calendar_config::kLandscapeLeftPct;
constexpr int kMaxRowH = calendar_config::kMaxRowH;
constexpr int kMinRowH = calendar_config::kMinRowH;
constexpr int kMinGap = 6;  // правило upstream: зазор между соседними элементами (не настройка)

// «Нет значения» в интерфейсе. Тире из общей пунктуации есть не во всех подмножествах шрифта — берём ASCII.
constexpr const char* kDash = "--";

Lang currentLang() {
  switch (I18n::getInstance().getLanguage()) {
    case Language::RU:
      return Lang::Ru;
    case Language::DE:
      return Lang::De;
    default:
      return Lang::En;
  }
}

int textW(const GfxRenderer& r, int font, const char* s, EpdFontFamily::Style st = kRegular) {
  return r.getTextWidth(font, s, st);
}

// Текст по центру горизонтального отрезка [x, x+w); y — верх строки.
void drawCentered(const GfxRenderer& r, int font, int x, int w, int y, const char* s, bool black = true,
                  EpdFontFamily::Style st = kRegular) {
  r.drawText(font, x + (w - textW(r, font, s, st)) / 2, y, s, black, st);
}

// ---------------------------------------------------------------------------
// Крупные цифры — тем же шрифтом (Ubuntu Medium), что и остальной интерфейс. Во встроенных шрифтах нет ничего
// крупнее 12 pt, поэтому три размера сгенерированы tools/gen_digit_fonts.sh (CalendarFonts.h): только 0-9 : - °.
// Размеры ниже — из метрик сгенерированных шрифтов; при смене размеров в генераторе обновить и их.
// ---------------------------------------------------------------------------
constexpr int kFontTimeXl = 0x43414C58;  // время, портрет
constexpr int kFontTimeL = 0x43414C4C;   // время, ландшафт
constexpr int kFontTemp = 0x43414C54;    // температура

// Высота цифры и отступ от верха строки шрифта до верха цифры (px) генератор считает сам — namespace calendar_fonts
// в CalendarFonts.h. Если размеры в CalendarConfig.h поменяли, а шрифт не перегенерировали — сборка остановится здесь.
static_assert(calendar_fonts::kTimePortraitPt == calendar_config::kTimeFontPortraitPt &&
                  calendar_fonts::kTimeLandscapePt == calendar_config::kTimeFontLandscapePt &&
                  calendar_fonts::kTempPt == calendar_config::kTempFontPt,
              "Размеры шрифта цифр в CalendarConfig.h изменены — запустите ./tools/gen_digit_fonts.sh");
using calendar_fonts::kTempDigitH;
using calendar_fonts::kTempTopOffset;
using calendar_fonts::kTimeLDigitH;
using calendar_fonts::kTimeLTopOffset;
using calendar_fonts::kTimeXlDigitH;
using calendar_fonts::kTimeXlTopOffset;

// Шрифты регистрируются в рендерере один раз (он хранит их до перезагрузки); объекты должны жить всё это время.
void ensureDigitFonts(GfxRenderer& r) {
  static bool done = false;
  if (done) return;
  static EpdFont xl(&calendar_time_xl), l(&calendar_time_l), tmp(&calendar_temp);
  static EpdFontFamily fxl(&xl), fl(&l), ft(&tmp);
  r.insertFont(kFontTimeXl, fxl);
  r.insertFont(kFontTimeL, fl);
  r.insertFont(kFontTemp, ft);
  done = true;
}

// Время «ЧЧ:ММ» по центру [x, x+w); y — верх цифр.
void drawBigTime(GfxRenderer& r, int font, int topOffset, int x, int y, int w, unsigned hh, unsigned mm) {
  char t[8];
  std::snprintf(t, sizeof(t), "%02u:%02u", hh % 100, mm % 100);
  r.drawText(font, x + (w - r.getTextWidth(font, t)) / 2, y - topOffset, t, true);
}

// Температура «18°» / «-3°» / «--» (нет данных); y — верх цифр. Возвращает ширину.
int drawTemperature(GfxRenderer& r, int x, int y, float t) {
  char buf[16];
  if (std::isnan(t)) {
    std::snprintf(buf, sizeof(buf), "--");
  } else {
    std::snprintf(buf, sizeof(buf), "%ld\xC2\xB0", std::lround(t));
  }
  r.drawText(kFontTemp, x, y - kTempTopOffset, buf, true);
  return r.getTextWidth(kFontTemp, buf);
}

// Верхняя строка: слева часовой пояс, справа заряд с процентами. Рисуется всегда — и в Immersive, где
// активность своих значков не показывает. В Normal она рисует иконку батареи в том же месте — пиксели совпадают.
void drawStatusRow(GfxRenderer& r, const Rect& vp, int utcOffsetMin) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int y = std::max(metrics.topPadding, vp.y);
  char tz[16];
  calendar_core::formatUtcOffset(utcOffsetMin, tz, sizeof(tz));
  r.drawText(SMALL_FONT_ID, vp.x + metrics.contentSidePadding, y, tz, true);
  constexpr int kBatW = 16, kBatH = 12;  // как в StandbyActivity::render
  GUI.drawBatteryRight(r, Rect{vp.x + vp.width - kBatW - metrics.contentSidePadding, y, kBatW, kBatH},
                       /*showPercentage=*/true);
}

// ---------------------------------------------------------------------------
// Иконки погоды примитивами (шрифтов с эмодзи нет). Всё контурное — хорошо читается на e-ink.
// ---------------------------------------------------------------------------
void disc(const GfxRenderer& r, int cx, int cy, int rad, Color c) {
  if (rad > 0) r.fillRoundedRect(cx - rad, cy - rad, 2 * rad, 2 * rad, rad, c);
}

// Силуэт облака в рамке (x,y,w,h), сжатый на inset. Чёрный + белый с inset=3 даёт контур.
void cloudShape(const GfxRenderer& r, int x, int y, int w, int h, int inset, Color c) {
  const int baseY = y + h * 50 / 100;
  const int baseH = h * 50 / 100;
  r.fillRoundedRect(x + inset, baseY + inset, w - 2 * inset, baseH - 2 * inset, baseH / 2 - inset, c);
  disc(r, x + w * 33 / 100, y + h * 52 / 100, h * 27 / 100 - inset, c);
  disc(r, x + w * 62 / 100, y + h * 40 / 100, h * 36 / 100 - inset, c);
}

void drawCloud(const GfxRenderer& r, int x, int y, int w, int h) {
  cloudShape(r, x, y, w, h, 0, Color::Black);
  cloudShape(r, x, y, w, h, 3, Color::White);
}

void drawSun(const GfxRenderer& r, int cx, int cy, int rad) {
  disc(r, cx, cy, rad, Color::Black);
  disc(r, cx, cy, rad - 3, Color::White);
  static constexpr int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
  static constexpr int kDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
  for (int i = 0; i < 8; ++i) {
    const float k = (kDx[i] && kDy[i]) ? 0.7071f : 1.0f;
    const int a = static_cast<int>((rad + 4) * k), b = static_cast<int>((rad + 4 + rad * 0.6f) * k);
    r.drawLine(cx + kDx[i] * a, cy + kDy[i] * a, cx + kDx[i] * b, cy + kDy[i] * b, 3, true);
  }
}

void drawMoon(const GfxRenderer& r, int cx, int cy, int rad) {
  disc(r, cx, cy, rad, Color::Black);
  disc(r, cx + rad * 40 / 100, cy - rad * 30 / 100, rad * 90 / 100, Color::White);
}

// s — сторона квадрата иконки.
void drawWeatherIcon(const GfxRenderer& r, Icon icon, int x, int y, int s) {
  switch (icon) {
    case Icon::Clear:
      drawSun(r, x + s / 2, y + s / 2, s * 20 / 100);
      break;
    case Icon::ClearNight:
      drawMoon(r, x + s / 2, y + s / 2, s * 28 / 100);
      break;
    case Icon::PartlyCloudy:
      drawSun(r, x + s * 32 / 100, y + s * 32 / 100, s * 14 / 100);
      drawCloud(r, x + s * 18 / 100, y + s * 30 / 100, s * 82 / 100, s * 64 / 100);
      break;
    case Icon::PartlyCloudyNight:
      drawMoon(r, x + s * 32 / 100, y + s * 32 / 100, s * 20 / 100);
      drawCloud(r, x + s * 18 / 100, y + s * 30 / 100, s * 82 / 100, s * 64 / 100);
      break;
    case Icon::Cloudy:
      drawCloud(r, x, y + s * 10 / 100, s, s * 76 / 100);
      break;
    case Icon::Fog:
      for (int i = 0; i < 4; ++i) {
        const int ly = y + s * (22 + i * 19) / 100;
        const int inset = (i % 2) ? s * 14 / 100 : s * 4 / 100;
        r.drawLine(x + inset, ly, x + s - s * 4 / 100 - (i % 2 ? 0 : s * 10 / 100), ly, 4, true);
      }
      break;
    case Icon::Drizzle:
    case Icon::Rain:
    case Icon::Snow:
    case Icon::Thunder: {
      drawCloud(r, x, y, s, s * 62 / 100);
      const int by = y + s * 68 / 100;
      for (int i = 0; i < 3; ++i) {
        const int bx = x + s * (26 + i * 22) / 100;
        if (icon == Icon::Rain) {
          r.drawLine(bx + 5, by, bx - 1, by + s * 26 / 100, 3, true);
        } else if (icon == Icon::Drizzle) {
          r.fillRect(bx, by + 2 + (i % 2) * 6, 4, 8, true);
        } else if (icon == Icon::Snow) {
          const int cy = by + 8 + (i % 2) * 6;
          r.drawLine(bx - 5, cy, bx + 5, cy, 2, true);
          r.drawLine(bx, cy - 5, bx, cy + 5, 2, true);
        }
      }
      if (icon == Icon::Thunder) {
        const int bx = x + s * 52 / 100, by2 = y + s * 56 / 100;
        r.drawLine(bx + 6, by2, bx - 4, by2 + s * 22 / 100, 3, true);
        r.drawLine(bx - 4, by2 + s * 22 / 100, bx + 6, by2 + s * 22 / 100, 3, true);
        r.drawLine(bx + 6, by2 + s * 22 / 100, bx - 4, by2 + s * 42 / 100, 3, true);
      }
      break;
    }
    case Icon::Unknown:  // заглушка «нет данных»: облако, перечёркнутое косой чертой
      drawCloud(r, x, y + s * 8 / 100, s, s * 74 / 100);
      r.drawLine(x + s * 12 / 100, y + s * 92 / 100, x + s * 88 / 100, y + s * 8 / 100, 9, false);
      r.drawLine(x + s * 12 / 100, y + s * 92 / 100, x + s * 88 / 100, y + s * 8 / 100, 4, true);
      break;
  }
}

// ---------------------------------------------------------------------------
// Строки-«чипы»: набор коротких фрагментов, переносимый по ширине, центрированный. Возвращает высоту.
// ---------------------------------------------------------------------------
constexpr int kMaxItems = 6;
struct Items {
  char s[kMaxItems][44];
  int n = 0;
  void add(const char* fmt, const char* a = "", const char* b = "", const char* c = "") {
    if (n < kMaxItems) std::snprintf(s[n++], sizeof(s[0]), fmt, a, b, c);
  }
};

int drawItemRows(const GfxRenderer& r, int font, int x, int w, int y, const Items& it) {
  const int lh = r.getLineHeight(font);
  static constexpr const char* kSep = "  \xC2\xB7  ";
  char line[160];
  int used = 0, lines = 0;
  auto flush = [&]() {
    if (!used) return;
    drawCentered(r, font, x, w, y + lines * lh, line);
    ++lines;
    used = 0;
    line[0] = '\0';
  };
  line[0] = '\0';
  for (int i = 0; i < it.n; ++i) {
    char cand[160];
    std::snprintf(cand, sizeof(cand), "%s%s%s", line, used ? kSep : "", it.s[i]);
    if (used && textW(r, font, cand) > w) {
      flush();
      std::snprintf(cand, sizeof(cand), "%s", it.s[i]);
    }
    std::snprintf(line, sizeof(line), "%s", cand);
    used = 1;
  }
  flush();
  return lines * lh;
}

// «17°» / «--» без знака «плюс».
void fmtDeg(char* out, size_t n, float v) {
  if (std::isnan(v)) {
    std::snprintf(out, n, "%s", kDash);
    return;
  }
  std::snprintf(out, n, "%ld\xC2\xB0", std::lround(v));
}

// Десятичный разделитель по языку: «0,4» для ru/de, «0.4» для en.
void fmtMm(char* out, size_t n, float v, Lang lang) {
  if (std::isnan(v)) {
    std::snprintf(out, n, "%s", kDash);
    return;
  }
  std::snprintf(out, n, "%.1f", v);
  if (lang != Lang::En) {
    for (char* p = out; *p; ++p) {
      if (*p == '.') *p = ',';
    }
  }
}

// ---------------------------------------------------------------------------
// Информационный блок: время, день недели, дата (2 формата), чипсы. Возвращает занятую высоту.
// ---------------------------------------------------------------------------
struct Today {
  int year;
  unsigned month, day, hour, minute, weekday, dayOfYear, isoWeek, daysInYear;
  int utcOffsetMin;
  uint32_t epoch;
};

int drawInfoBlock(GfxRenderer& r, int timeFont, int timeTopOffset, int heroH, int x, int y, int w, const Today& t,
                  Lang lang, int pad) {
  const auto& L = calendar_core::labels(lang);
  const int y0 = y;

  drawBigTime(r, timeFont, timeTopOffset, x + pad / 2, y, w - pad, t.hour, t.minute);
  y += heroH + 6;

  drawCentered(r, kFontText, x, w, y, calendar_core::weekdayName(lang, t.weekday), true, kBold);
  y += r.getLineHeight(kFontText) + 4;

  // Дата: «20 сентября 2026» и плашка «20.09.2026».
  char longDate[40], shortDate[20];
  calendar_core::formatLongDate(lang, t.year, t.month, t.day, longDate, sizeof(longDate));
  calendar_core::formatShortDate(lang, t.year, t.month, t.day, shortDate, sizeof(shortDate));
  const int pillW = textW(r, kFontSmall, shortDate, kBold) + 20;
  const int pillH = r.getLineHeight(kFontSmall) + 4;
  const int longW = textW(r, kFontText, longDate);
  if (longW + pillW + 12 <= w - pad) {
    const int startX = x + (w - (longW + 12 + pillW)) / 2;
    r.drawText(kFontText, startX, y, longDate, true);
    r.drawRoundedRect(startX + longW + 12, y, pillW, pillH, 2, pillH / 2, true);
    r.drawText(kFontSmall, startX + longW + 12 + 10, y + 2, shortDate, true, kBold);
    y += std::max(pillH, r.getLineHeight(kFontText)) + 8;
  } else {  // узкая колонка: две строки
    drawCentered(r, kFontText, x, w, y, longDate);
    y += r.getLineHeight(kFontText) + 4;
    r.drawRoundedRect(x + (w - pillW) / 2, y, pillW, pillH, 2, pillH / 2, true);
    r.drawText(kFontSmall, x + (w - pillW) / 2 + 10, y + 2, shortDate, true, kBold);
    y += pillH + 8;
  }

  char chips[48];
  std::snprintf(chips, sizeof(chips), "%s %u  \xC2\xB7  %s %u / %u", L.week, t.isoWeek, L.day, t.dayOfYear,
                t.daysInYear);
  drawCentered(r, kFontSmall, x, w, y, chips);
  y += r.getLineHeight(kFontSmall) + 8;
  return y - y0;
}

// ---------------------------------------------------------------------------
// Блок погоды. Любое недостающее значение — прочерк; нет данных совсем — облако с косой чертой.
//   строка 1: город (слева)               обн. 14:02 (справа)
//   строка 2: иконка  температура   описание / ощущается
//   строка 3: мин · макс · ветер · осадки
//   строка 4: восход · закат · длина дня   (офлайн, по месту)
// ---------------------------------------------------------------------------
void fmtUpdated(char* out, size_t n, const weather_core::Weather& w, const Today& t, Lang lang, bool expired,
                bool stale) {
  const auto& WL = weather_core::labels(lang);
  out[0] = '\0';
  if (!w.valid || w.fetchedEpoch == 0) return;
  std::tm lt{};
  if (!TimeUtils::getLocalDateTime(w.fetchedEpoch, lt)) return;
  char when[24];
  if (lt.tm_mday == static_cast<int>(t.day) && lt.tm_mon + 1 == static_cast<int>(t.month)) {
    std::snprintf(when, sizeof(when), "%02d:%02d", lt.tm_hour, lt.tm_min);
  } else {
    std::snprintf(when, sizeof(when), "%02d.%02d %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min);
  }
  if (stale || expired) {
    std::snprintf(out, n, "%s  \xC2\xB7  %s", WL.stale, when);
  } else {
    std::snprintf(out, n, "%s %s", WL.updated, when);
  }
}

// compact — узкая колонка ландшафта: всё мельче и плотнее, чтобы уместиться по высоте.
int drawWeatherBlock(GfxRenderer& r, int x, int y, int w, const weather_core::Cache& c, const Today& t, Lang lang, bool bottomRule, bool compact) {
  const auto& WL = weather_core::labels(lang);
  const int y0 = y;
  const int pad = compact ? kCompactPad : kSidePad;
  const int left = x + pad;
  const int inner = w - 2 * pad;

  // Данные старше суток-полусуток не выдаём за текущие; «устарело» — после kStaleSec.
  const uint32_t age = (t.epoch >= c.weather.fetchedEpoch) ? t.epoch - c.weather.fetchedEpoch : 0;
  const bool have = c.weather.valid;
  const bool expired = have && age > weather_core::kExpireSec;
  const bool stale = have && age > weather_core::kStaleSec;
  const weather_core::Weather empty{};
  const weather_core::Weather& w0 = (have && !expired) ? c.weather : empty;

  r.drawLine(left, y, left + inner, y, 2, true);
  y += 8;

  // Строка 1: город + время обновления.
  {
    char upd[40];
    fmtUpdated(upd, sizeof(upd), c.weather, t, lang, expired, stale);
    if (compact && !stale && !expired) upd[0] = '\0';  // в узкой колонке места нет: свежесть видно и так
    const int updW = upd[0] ? textW(r, kFontSmall, upd) : 0;
    const char* cityRaw = c.place.city[0] ? c.place.city : WL.unknownPlace;
    const std::string city = r.truncatedText(kFontSmall, cityRaw, inner - updW - (updW ? 12 : 0), kBold);
    r.drawText(kFontSmall, left, y, city.c_str(), true, kBold);
    if (upd[0]) r.drawText(kFontSmall, left + inner - updW, y, upd, true);
    y += r.getLineHeight(kFontSmall) + 4;
  }

  // Строка 2: иконка, температура, описание.
  {
    const int iconS = compact ? 44 : 56;
    const int descFont = compact ? kFontSmall : kFontText;
    const bool ok = !std::isnan(w0.temp);
    const Icon icon = (ok && w0.code >= 0) ? weather_core::iconFor(w0.code, w0.isDay) : Icon::Unknown;
    drawWeatherIcon(r, ok ? icon : Icon::Unknown, left, y, iconS);
    const int tx = left + iconS + (compact ? 10 : 14);
    const int tw = drawTemperature(r, tx, y + (iconS - kTempDigitH) / 2, w0.temp);

    const int textX = tx + tw + (compact ? 10 : 16);
    const int textW2 = left + inner - textX;
    const char* desc = (ok && w0.code >= 0) ? weather_core::description(lang, w0.code) : "";
    if (!desc[0]) desc = WL.noData;

    // Описание: в широком режиме — одна строка + «ощущается»; в узком — до двух строк, «ощущается» уходит в детали.
    std::string d1 = r.truncatedText(descFont, desc, textW2, kBold);
    std::string d2;
    if (compact && textW(r, descFont, desc, kBold) > textW2) {
      const std::string full = desc;
      size_t cut = std::string::npos;
      for (size_t i = 0; i < full.size(); ++i) {
        if (full[i] == ' ' && textW(r, descFont, full.substr(0, i).c_str(), kBold) <= textW2) cut = i;
      }
      if (cut != std::string::npos) {
        d1 = full.substr(0, cut);
        d2 = r.truncatedText(descFont, full.substr(cut + 1).c_str(), textW2, kBold);
      }
    }
    const int dh = r.getLineHeight(descFont);
    const int lines = d2.empty() ? 1 : 2;
    const int blockH = lines * dh + (compact ? 0 : r.getLineHeight(kFontSmall));
    int ty = y + (iconS - blockH) / 2;
    r.drawText(descFont, textX, ty, d1.c_str(), true, kBold);
    if (!d2.empty()) r.drawText(descFont, textX, ty + dh, d2.c_str(), true, kBold);
    if (!compact) {
      char feels[32], fd[16];
      fmtDeg(fd, sizeof(fd), w0.feels);
      std::snprintf(feels, sizeof(feels), "%s %s", WL.feels, fd);
      r.drawText(kFontSmall, textX, ty + dh, feels, true);
    }
    y += iconS + 6;
  }

  // Строка 3: диапазон, ветер, осадки.
  {
    Items it;
    char a[16], b[16], wind[16], mm[16];
    if (compact) {
      char fd[16];
      fmtDeg(fd, sizeof(fd), w0.feels);
      it.add("%s %s", WL.feels, fd);
    }
    fmtDeg(a, sizeof(a), w0.tMin);
    fmtDeg(b, sizeof(b), w0.tMax);
    it.add("%s %s", WL.min, a);
    it.add("%s %s", WL.max, b);
    if (std::isnan(w0.windMs)) {
      std::snprintf(wind, sizeof(wind), "%s", kDash);
    } else {
      std::snprintf(wind, sizeof(wind), "%ld", std::lround(w0.windMs));
    }
    char windTxt[40];
    if (std::isnan(w0.windMs)) {
      std::snprintf(windTxt, sizeof(windTxt), "%s %s", WL.wind, wind);
    } else {
      std::snprintf(windTxt, sizeof(windTxt), "%s %s %s", WL.wind, wind, WL.windUnit);
    }
    it.add("%s", windTxt);
    fmtMm(mm, sizeof(mm), w0.precipMm, lang);
    char pr[48];
    if (w0.precipProb >= 0 && !std::isnan(w0.precipMm)) {
      std::snprintf(pr, sizeof(pr), "%s %s %s (%d %%)", WL.precip, mm, WL.mmUnit, w0.precipProb);
    } else if (std::isnan(w0.precipMm)) {
      std::snprintf(pr, sizeof(pr), "%s %s", WL.precip, mm);
    } else {
      std::snprintf(pr, sizeof(pr), "%s %s %s", WL.precip, mm, WL.mmUnit);
    }
    it.add("%s", pr);
    y += drawItemRows(r, kFontSmall, x + pad, inner, y, it) + 4;
  }

  // Строка 4: солнце — считается офлайн по месту, работает и без сети.
  {
    const auto& CL = calendar_core::labels(lang);
    const auto sun = sun_times::compute(t.year, t.month, t.day, c.place.lat, c.place.lon, t.utcOffsetMin);
    Items it;
    if (!sun.valid) {
      it.add("%s %s", CL.sunrise, kDash);
      it.add("%s %s", CL.sunset, kDash);
    } else if (sun.polarDay || sun.polarNight) {
      it.add("%s", sun.polarDay ? CL.polarDay : CL.polarNight);
    } else {
      char a[12], b[12], d[28];
      std::snprintf(a, sizeof(a), "%02d:%02d", sun.sunriseMin / 60, sun.sunriseMin % 60);
      std::snprintf(b, sizeof(b), "%02d:%02d", sun.sunsetMin / 60, sun.sunsetMin % 60);
      std::snprintf(d, sizeof(d), "%d %s %02d %s", sun.daylightMin / 60, CL.hoursShort, sun.daylightMin % 60,
                    CL.minutesShort);
      it.add("%s %s", CL.sunrise, a);
      it.add("%s %s", CL.sunset, b);
      it.add("%s %s", CL.daylight, d);
    }
    y += drawItemRows(r, kFontSmall, x + pad, inner, y, it) + 4;
  }

  if (bottomRule) {
    r.drawLine(left, y, left + inner, y, 2, true);
    y += 2;
  }
  return y - y0;
}

// ---------------------------------------------------------------------------
// Сетка месяца.
//   сегодня           — чёрная плашка, белая жирная цифра
//   выходные          — редкий растр за цифрой (в BW и на сером экране одинаково)
//   соседние месяцы   — мелкий шрифт
// ---------------------------------------------------------------------------
constexpr int kGridTitleH = 40;
constexpr int kGridDowH = 30;

int gridRowH(int rows, int availH) {
  return std::clamp((availH - kGridTitleH - kGridDowH) / std::max(rows, 1), kMinRowH, kMaxRowH);
}

void drawMonthGrid(GfxRenderer& r, int x, int y, int w, int availH, int viewYear, unsigned viewMonth, const Today& t,
                   bool browsing, Lang lang) {
  MonthGrid g;
  calendar_core::buildMonthGrid(viewYear, viewMonth, g);
  const int rowH = gridRowH(g.rows, availH);

  // Заголовок «Сентябрь 2026». Не на текущем месяце — инверсная плашка: видно, что листаем.
  char title[40];
  std::snprintf(title, sizeof(title), "%s %d", calendar_core::monthName(lang, viewMonth), viewYear);
  const int tw = textW(r, kFontText, title, kBold);
  const int tx = x + (w - tw) / 2;
  if (browsing) {
    r.fillRoundedRect(tx - 14, y, tw + 28, kGridTitleH - 6, 10, Color::Black);
    r.drawText(kFontText, tx, y + 3, title, false, kBold);
  } else {
    r.drawText(kFontText, tx, y + 3, title, true, kBold);
  }

  // Шапка дней недели + линия.
  const int cw = w / calendar_core::kGridCols;
  const int gx = x + (w - cw * calendar_core::kGridCols) / 2;
  const int dowY = y + kGridTitleH;
  for (int c = 0; c < calendar_core::kGridCols; ++c) {
    drawCentered(r, kFontSmall, gx + c * cw, cw, dowY + 2, calendar_core::weekdayShort(lang, c), true,
                 c >= 5 ? kBold : kRegular);
  }
  r.drawLine(gx, dowY + kGridDowH - 2, gx + cw * calendar_core::kGridCols, dowY + kGridDowH - 2, 2, true);

  // Числа.
  const int gridY = dowY + kGridDowH;
  const int inset = kMinGap / 2;
  for (int row = 0; row < g.rows; ++row) {
    for (int c = 0; c < calendar_core::kGridCols; ++c) {
      const auto& cell = g.cells[row * calendar_core::kGridCols + c];
      const int cx = gx + c * cw;
      const int cy = gridY + row * rowH;
      char num[4];
      std::snprintf(num, sizeof(num), "%u", cell.day);

      const bool isToday = cell.kind == CellKind::CurrentMonth && cell.year == t.year && cell.month == t.month &&
                           cell.day == t.day;
      if (isToday) {
        r.fillRoundedRect(cx + inset, cy + inset, cw - 2 * inset, rowH - 2 * inset, 10, Color::Black);
        r.drawText(kFontText, cx + (cw - textW(r, kFontText, num, kBold)) / 2,
                   cy + (rowH - r.getLineHeight(kFontText)) / 2, num, false, kBold);
        continue;
      }
      if (cell.kind == CellKind::CurrentMonth && cell.weekend) {
        r.fillRectDither(cx + inset, cy + inset, cw - 2 * inset, rowH - 2 * inset, Color::LightGray);
      }
      const int f = cell.kind == CellKind::CurrentMonth ? kFontText : kFontSmall;
      r.drawText(f, cx + (cw - textW(r, f, num)) / 2, cy + (rowH - r.getLineHeight(f)) / 2, num, true);
    }
  }
}

}  // namespace

// ===========================================================================

bool CalendarFace::takeSnapshot(Snapshot& s) {
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  std::tm lt{};
  if (!now || !TimeUtils::getLocalDateTime(now, lt)) {
    s = Snapshot{};
    return false;
  }
  s.valid = true;
  s.epoch = now;
  s.year = lt.tm_year + 1900;
  s.month = static_cast<unsigned>(lt.tm_mon + 1);
  s.day = static_cast<unsigned>(lt.tm_mday);
  s.hour = static_cast<unsigned>(lt.tm_hour);
  s.minute = static_cast<unsigned>(lt.tm_min);
  s.weekday = calendar_core::weekday(s.year, s.month, s.day);
  s.dayOfYear = calendar_core::dayOfYear(s.year, s.month, s.day);
  s.isoWeek = calendar_core::isoWeek(s.year, s.month, s.day);
  s.daysInYear = calendar_core::daysInYear(s.year);
  s.minuteKey = now / 60u;
  s.dayKey = static_cast<uint32_t>(calendar_core::daysFromCivil(s.year, s.month, s.day));
  s.utcOffsetMin = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  return true;
}

void CalendarFace::onEnter() {
  monthOffset_ = 0;
  updatesSinceCleanup_ = 0;
  lastNavMs_ = millis();
  takeSnapshot(snap_);
}

void CalendarFace::onExit() {
  weather_.stop();  // если успели поднять Wi-Fi — выключить
  renderer_ = nullptr;
  snap_ = Snapshot{};
}

void CalendarFace::shiftMonth(int delta) {
  if (!snap_.valid) return;
  int y = 0;
  unsigned m = 0;
  if (!calendar_core::addMonths(snap_.year, snap_.month, monthOffset_ + delta, y, m)) {
    LOG_DBG("STANDBY", "Calendar: month offset %d out of range", monthOffset_ + delta);
    return;
  }
  monthOffset_ += delta;
  lastNavMs_ = millis();
}

void CalendarFace::onPagePrev() { shiftMonth(-1); }
void CalendarFace::onPageNext() { shiftMonth(+1); }

StandbyFace::TickResult CalendarFace::tick() {
  const bool hadClock = snap_.valid;
  const uint32_t prevMinute = snap_.minuteKey;
  const uint32_t prevDay = snap_.dayKey;
  takeSnapshot(snap_);

  if (hadClock != snap_.valid) return TickResult::Redraw;  // часы появились или пропали
  if (!snap_.valid) return TickResult::None;

  // Погода: шаг конечного автомата. Может занять секунды (блокирующий HTTP) — поэтому после него
  // снимок времени устарел; следующий tick() его обновит.
  const bool weatherChanged = weather_.step(renderer_, snap_.epoch, currentLang());

  // Вернуться к текущему месяцу, если давно не листали.
  if (monthOffset_ != 0 && millis() - lastNavMs_ >= calendar_config::kMonthAutoReturnSec * 1000u) {
    monthOffset_ = 0;
    return TickResult::Redraw;
  }

  if (snap_.dayKey != prevDay) {
    updatesSinceCleanup_ = 0;
    return TickResult::RedrawWithGhostCleanup;  // полночь: сетка и дата меняются целиком
  }
  if (snap_.minuteKey != prevMinute) {
    if (++updatesSinceCleanup_ >= calendar_config::kGhostCleanupEveryUpdates) {
      updatesSinceCleanup_ = 0;
      return TickResult::RedrawWithGhostCleanup;
    }
    return TickResult::Redraw;
  }
  return weatherChanged ? TickResult::Redraw : TickResult::None;
}

StrId CalendarFace::titleId() const { return StrId::STR_FACE_CALENDAR; }

uint32_t CalendarFace::secondsUntilNextWake() const {
  // До начала следующей минуты; StandbyActivity ограничивает сверху сам.
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (!now) return 60u;
  return 60u - (now % 60u);
}

void CalendarFace::render(GfxRenderer& r, const Rect& vp) {
  renderer_ = &r;
  ensureDigitFonts(r);
  const Lang lang = currentLang();
  drawStatusRow(r, vp, (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15);
  if (!snap_.valid && !takeSnapshot(snap_)) {
    // Часам нельзя верить: активность сама пишет «синхронизация», мы — подсказку по центру (плюс строка статуса выше).
    drawCentered(r, kFontText, vp.x, vp.width, vp.y + vp.height / 2, calendar_core::labels(lang).noClock, true, kBold);
    return;
  }

  const Today t{snap_.year,    snap_.month,      snap_.day,        snap_.hour,       snap_.minute,
                snap_.weekday, snap_.dayOfYear,  snap_.isoWeek,    snap_.daysInYear, snap_.utcOffsetMin,
                snap_.epoch};

  int viewYear = snap_.year;
  unsigned viewMonth = snap_.month;
  calendar_core::addMonths(snap_.year, snap_.month, monthOffset_, viewYear, viewMonth);
  const bool browsing = monthOffset_ != 0;
  const weather_core::Cache& wx = weather_.cache();

  const bool landscape = vp.width > vp.height;
  const int top = vp.y + kTopReserve;
  const int bottom = vp.y + vp.height - kBottomReserve;

  if (landscape) {
    const int leftW = vp.width * kLandscapeLeftPct / 100;
    const int infoH = drawInfoBlock(r, kFontTimeL, kTimeLTopOffset, kTimeLDigitH, vp.x, top, leftW, t, lang, kCompactPad);
    drawWeatherBlock(r, vp.x, top + infoH, leftW, wx, t, lang, /*bottomRule=*/false, /*compact=*/true);
    // Точки-пейджер внизу стоят по центру экрана, правее левой колонки, — разделитель идёт до самого низа.
    r.drawLine(vp.x + leftW, vp.y + kTopReserve, vp.x + leftW, vp.y + vp.height - 8, 2, true);
    const int gx = vp.x + leftW + kSidePad;
    const int gw = vp.width - leftW - 2 * kSidePad;
    drawMonthGrid(r, gx, top, gw, bottom - top, viewYear, viewMonth, t, browsing, lang);
    return;
  }

  // Портрет: время и дата, погода, сетка месяца книзу.
  const int infoH = drawInfoBlock(r, kFontTimeXl, kTimeXlTopOffset, kTimeXlDigitH, vp.x, top, vp.width, t, lang, kSidePad);
  const int wxH = drawWeatherBlock(r, vp.x, top + infoH, vp.width, wx, t, lang, /*bottomRule=*/true, /*compact=*/false);

  MonthGrid probe;
  calendar_core::buildMonthGrid(viewYear, viewMonth, probe);
  const int gridAvail = bottom - (top + infoH + wxH) - kMinGap;
  const int gridH = kGridTitleH + kGridDowH + gridRowH(probe.rows, gridAvail) * probe.rows;
  drawMonthGrid(r, vp.x + kSidePad, bottom - gridH, vp.width - 2 * kSidePad, gridH, viewYear, viewMonth, t, browsing,
                lang);
}
