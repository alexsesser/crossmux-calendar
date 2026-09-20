#include "CalendarDraw.h"

#include <Arduino.h>
#include <EpdFont.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>

#include "CalendarConfig.h"
#include "CalendarFonts.h"  // данные шрифта: включается ровно в одном месте
#include "I18nKeys.h"
#include "components/UITheme.h"

namespace cal_draw {

using weather_core::Icon;

// Размеры шрифта цифр в CalendarConfig.h изменены, а шрифт не перегенерирован — сборка останавливается здесь.
static_assert(calendar_fonts::kTimePortraitPt == calendar_config::kTimeFontPortraitPt &&
                  calendar_fonts::kTimeLandscapePt == calendar_config::kTimeFontLandscapePt &&
                  calendar_fonts::kTempPt == calendar_config::kTempFontPt,
              "Размеры шрифта цифр в CalendarConfig.h изменены — запустите ./tools/gen_digit_fonts.sh");

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

int textW(const GfxRenderer& r, int font, const char* s, EpdFontFamily::Style st) {
  return r.getTextWidth(font, s, st);
}

// Текст по центру горизонтального отрезка [x, x+w); y — верх строки.
void drawCentered(const GfxRenderer& r, int font, int x, int w, int y, const char* s, bool black,
                  EpdFontFamily::Style st) {
  r.drawText(font, x + (w - textW(r, font, s, st)) / 2, y, s, black, st);
}

// Шрифты регистрируются в рендерере один раз (он хранит их до перезагрузки); объекты должны жить всё это время.
void abbrTo(char* out, size_t outSize, const char* s, int n) {
  size_t o = 0;
  for (int cnt = 0; *s && cnt < n; ++cnt) {
    const unsigned char ch = static_cast<unsigned char>(*s);
    const size_t len = ch < 0x80 ? 1 : (ch >> 5) == 6 ? 2 : (ch >> 4) == 14 ? 3 : 4;
    if (o + len + 1 > outSize) break;
    for (size_t i = 0; i < len && s[i]; ++i) out[o++] = s[i];
    s += len;
  }
  if (outSize) out[o] = '\0';
}

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

namespace {
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

}  // namespace

// Луна по фракции цикла: контур + освещённая часть построчно (для растрового экрана — ровно то, что нужно).
//   освещённый отрезок строки: растущая (f < ½) — [cx + k·hw, cx + hw], убывающая — [cx − hw, cx − k·hw],
//   где hw — полуширина диска в этой строке, k = cos(2πf): новолуние k = 1 (пусто), полнолуние k = −1 (всё).
void drawMoonPhase(const GfxRenderer& r, double f, int cx, int cy, int rad) {
  const double k = std::cos(2.0 * 3.14159265358979 * f);
  disc(r, cx, cy, rad, Color::Black);
  const int ri = rad - 3;
  disc(r, cx, cy, ri, Color::White);
  for (int dy = -ri; dy <= ri; ++dy) {
    const double hw = std::sqrt(static_cast<double>(ri * ri - dy * dy));
    int x0, x1;
    if (f < 0.5) {
      x0 = cx + static_cast<int>(std::lround(k * hw));
      x1 = cx + static_cast<int>(std::lround(hw));
    } else {
      x0 = cx - static_cast<int>(std::lround(hw));
      x1 = cx - static_cast<int>(std::lround(k * hw));
    }
    if (x1 > x0) r.fillRect(x0, cy + dy, x1 - x0, 1, true);
  }
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

}  // namespace cal_draw
