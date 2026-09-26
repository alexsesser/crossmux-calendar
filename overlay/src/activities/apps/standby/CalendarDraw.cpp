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
                  calendar_fonts::kTempPt == calendar_config::kTempFontPt &&
                  calendar_fonts::kDayPt == calendar_config::kDayFontPt &&
                  calendar_fonts::kDayBold == calendar_config::kGridDigitsBold,
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
  static EpdFont xl(&calendar_time_xl), l(&calendar_time_l), tmp(&calendar_temp), day(&calendar_day);
  static EpdFontFamily fxl(&xl), fl(&l), ft(&tmp), fd(&day);
  r.insertFont(kFontTimeXl, fxl);
  r.insertFont(kFontTimeL, fl);
  r.insertFont(kFontTemp, ft);
  r.insertFont(kFontDay, fd);
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
// Значки вместо слов. Рисуются примитивами в квадрате s×s; толщина линий зависит от размера.
// ---------------------------------------------------------------------------
namespace {

void arrowV(const GfxRenderer& r, int cx, int y0, int y1, int head, int t) {  // стрелка от y0 к y1 (острие в y1)
  const int dir = y1 >= y0 ? -1 : 1;                                         // куда «назад» от острия
  r.drawLine(cx, y0, cx, y1, t, true);
  r.drawLine(cx, y1, cx - head, y1 + dir * head, t, true);
  r.drawLine(cx, y1, cx + head, y1 + dir * head, t, true);
}

// Полусолнце над горизонтом со стрелкой под ним: вверх — восход, вниз — закат.
void sunOnHorizon(const GfxRenderer& r, int x, int y, int s, bool rising, int t) {
  const int cx = x + s / 2, hy = y + s * 56 / 100, rad = std::max(3, s * 20 / 100);
  static constexpr int kDx[5] = {-10, -7, 0, 7, 10};
  static constexpr int kDy[5] = {0, -7, -10, -7, 0};
  for (int i = 0; i < 5; ++i) {
    const int a = rad + std::max(2, s * 6 / 100), b = a + std::max(2, s * 10 / 100);
    r.drawLine(cx + kDx[i] * a / 10, hy + kDy[i] * a / 10, cx + kDx[i] * b / 10, hy + kDy[i] * b / 10, t, true);
  }
  disc(r, cx, hy, rad, Color::Black);
  r.fillRect(x, hy + 1, s, s - (hy - y) - 1, false);  // нижняя половина диска — под горизонт
  r.drawLine(x + s * 4 / 100, hy, x + s * 96 / 100, hy, t, true);
  const int head = std::max(2, s * 12 / 100);
  if (rising) {
    arrowV(r, cx, y + s * 97 / 100, y + s * 68 / 100, head, t);
  } else {
    arrowV(r, cx, y + s * 68 / 100, y + s * 97 / 100, head, t);
  }
}

}  // namespace

void drawGlyph(const GfxRenderer& r, Glyph g, int x, int y, int s) {
  const int t = std::max(2, s / 10), cx = x + s / 2;
  switch (g) {
    case Glyph::None:
      break;
    case Glyph::TempMin:  // ↓
      arrowV(r, cx, y + s * 10 / 100, y + s * 92 / 100, s * 28 / 100, t + 1);
      break;
    case Glyph::TempMax:  // ↑
      arrowV(r, cx, y + s * 92 / 100, y + s * 10 / 100, s * 28 / 100, t + 1);
      break;
    case Glyph::Wind: {  // три порыва разной длины с завитками на концах
      const int ya = y + s * 28 / 100, yb = y + s * 52 / 100, yc = y + s * 76 / 100;
      r.drawLine(x + s * 6 / 100, ya, x + s * 70 / 100, ya, t, true);
      r.drawLine(x + s * 70 / 100, ya, x + s * 84 / 100, ya - s * 10 / 100, t, true);
      r.drawLine(x + s * 6 / 100, yb, x + s * 92 / 100, yb, t, true);
      r.drawLine(x + s * 6 / 100, yc, x + s * 56 / 100, yc, t, true);
      r.drawLine(x + s * 56 / 100, yc, x + s * 68 / 100, yc + s * 10 / 100, t, true);
      break;
    }
    case Glyph::Drop: {  // капля: острие сверху + круг
      const int apex = y + s * 4 / 100, ccy = y + s * 66 / 100, rad = s * 28 / 100;
      for (int yy = apex; yy <= ccy; ++yy) {
        const int hw = (yy - apex) * rad / std::max(1, ccy - apex);
        r.fillRect(cx - hw, yy, 2 * hw + 1, 1, true);
      }
      disc(r, cx, ccy, rad, Color::Black);
      break;
    }
    case Glyph::Pin: {  // метка на карте: круг с дыркой и острие вниз
      const int rad = std::max(3, s * 30 / 100), ccy = y + s * 36 / 100, tip = y + s * 96 / 100;
      for (int yy = ccy; yy <= tip; ++yy) {  // острие: сужающийся к низу треугольник
        const int hw = (tip - yy) * rad * 8 / 10 / std::max(1, tip - ccy);
        r.fillRect(cx - hw, yy, 2 * hw + 1, 1, true);
      }
      disc(r, cx, ccy, rad, Color::Black);
      disc(r, cx, ccy, std::max(1, rad * 40 / 100), Color::White);
      break;
    }
    case Glyph::Daylight: {  // дуга пути солнца над горизонтом: слева восход, справа закат, на вершине — солнце
      const int w = s * 3 / 2, hy = y + s * 82 / 100, ccx = x + w / 2;
      const int rad = std::min(w / 2 - 3, hy - y - std::max(3, s / 8) - 1);
      int px = ccx - rad, py = hy;
      for (int i = 1; i <= 16; ++i) {
        const double a = 3.14159265358979 * (1.0 - i / 16.0);
        const int nx = ccx + static_cast<int>(std::lround(rad * std::cos(a)));
        const int ny = hy - static_cast<int>(std::lround(rad * std::sin(a)));
        r.drawLine(px, py, nx, ny, t, true);
        px = nx;
        py = ny;
      }
      r.drawLine(x, hy, x + w, hy, t, true);
      disc(r, ccx, hy - rad, std::max(3, s / 7), Color::Black);
      disc(r, ccx - rad, hy, std::max(2, s / 11), Color::Black);
      disc(r, ccx + rad, hy, std::max(2, s / 11), Color::Black);
      break;
    }
    case Glyph::Sunrise:
      sunOnHorizon(r, x, y, s, true, t);
      break;
    case Glyph::Sunset:
      sunOnHorizon(r, x, y, s, false, t);
      break;
  }
}

int glyphSize(const GfxRenderer& r, int font) { return r.getLineHeight(font) + 4; }

namespace {
// Ширина самого значка (без зазора до текста).
int glyphW(const GfxRenderer& r, int font, Glyph g) {
  const int s = glyphSize(r, font);
  switch (g) {
    case Glyph::None:
      return 0;
    case Glyph::TempMin:
    case Glyph::TempMax:
      return textW(r, font, "t", kBold) + 1 + s * 6 / 10;
    case Glyph::Daylight:
      return s * 3 / 2;
    default:
      return s;
  }
}
}  // namespace

int glyphTextW(const GfxRenderer& r, int font, Glyph g, const char* text, EpdFontFamily::Style st) {
  return (g == Glyph::None ? 0 : glyphW(r, font, g) + 4) + textW(r, font, text, st);
}

void drawGlyphText(const GfxRenderer& r, int font, Glyph g, int x, int y, const char* text, EpdFontFamily::Style st) {
  const int s = glyphSize(r, font);
  const int gw = glyphW(r, font, g);
  if (g == Glyph::TempMin || g == Glyph::TempMax) {
    const int tw = textW(r, font, "t", kBold), aw = s * 6 / 10;
    r.drawText(font, x, y + (s - r.getLineHeight(font)) / 2, "t", true, kBold);
    drawGlyph(r, g, x + tw + 1 - (s - aw) / 2, y, s);  // стрелка стоит по центру квадрата s — сдвигаем в узкую рамку
  } else if (g != Glyph::None) {
    drawGlyph(r, g, x, y, s);
  }
  if (g != Glyph::None) x += gw + 4;
  r.drawText(font, x, y + (s - r.getLineHeight(font)) / 2, text, true, st);
}

int drawRichRows(const GfxRenderer& r, int font, int x, int w, int y, const RichItems& items, EpdFontFamily::Style st,
                 bool center) {
  constexpr int kGap = 14;  // между элементами одной строки
  const int rowH = glyphSize(r, font);
  int rows = 0, from = 0;
  auto flush = [&](int to) {  // [from, to) — одна строка
    int total = -kGap;
    for (int i = from; i < to; ++i) total += kGap + glyphTextW(r, font, items.it[i].g, items.it[i].s, st);
    int cx = center ? x + (w - total) / 2 : x;
    for (int i = from; i < to; ++i) {
      drawGlyphText(r, font, items.it[i].g, cx, y + rows * rowH, items.it[i].s, st);
      cx += glyphTextW(r, font, items.it[i].g, items.it[i].s, st) + kGap;
    }
    ++rows;
    from = to;
  };
  int cur = -kGap;
  for (int i = 0; i < items.n; ++i) {
    const int iw = glyphTextW(r, font, items.it[i].g, items.it[i].s, st);
    if (i > from && cur + kGap + iw > w) {
      flush(i);
      cur = -kGap;
    }
    cur += kGap + iw;
  }
  if (items.n > from) flush(items.n);
  return rows * rowH;
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
