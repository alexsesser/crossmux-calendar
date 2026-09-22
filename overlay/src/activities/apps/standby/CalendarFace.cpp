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
#include "CalendarCore.h"
#include "CalendarDetail.h"
#include "CalendarDraw.h"
#include "CrossPointSettings.h"
#include "HolidayCore.h"
#include "I18nKeys.h"
#include "SunTimes.h"
#include "WeatherCore.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {

using namespace cal_draw;
using calendar_core::CellKind;
using calendar_core::Lang;
using calendar_core::MonthGrid;
using weather_core::Icon;

// Раскладка — значения в CalendarConfig.h.
constexpr int kTopReserve = calendar_config::kTopReserve;        // под оверлей активности (заголовок, батарея)
constexpr int kBottomReserve = calendar_config::kBottomReserve;  // под точки-пейджер
constexpr int kSidePad = calendar_config::kSidePad;
constexpr int kCompactPad = calendar_config::kLandscapePad;      // поля левой колонки в ландшафте
constexpr int kLandscapeLeftPct = calendar_config::kLandscapeLeftPct;
constexpr int kMaxRowH = calendar_config::kMaxRowH;
constexpr int kMinRowH = calendar_config::kMinRowH;
constexpr int kMinGap = 6;  // правило upstream: зазор между соседними элементами (не настройка)

// ---------------------------------------------------------------------------
// Информационный блок: время, день недели, дата (2 формата), чипсы. Возвращает занятую высоту.
// ---------------------------------------------------------------------------
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
int drawWeatherBlock(GfxRenderer& r, int x, int y, int w, const weather_core::Cache& c, const Today& t, Lang lang, bool bottomRule,
                     bool compact, cal_detail::HitMap& hit) {
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
    const Fit city(r, kFontSmall, cityRaw, inner - updW - (updW ? 12 : 0), kBold);
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
    // Пока описание помещается (почти всегда), строк в куче не создаём.
    std::string d1s, d2s;
    const char* d1 = desc;
    const char* d2 = "";
    if (textW(r, descFont, desc, kBold) > textW2) {
      d1s = r.truncatedText(descFont, desc, textW2, kBold);
      if (compact) {
        const std::string full = desc;
        size_t cut = std::string::npos;
        for (size_t i = 0; i < full.size(); ++i) {
          if (full[i] == ' ' && textW(r, descFont, full.substr(0, i).c_str(), kBold) <= textW2) cut = i;
        }
        if (cut != std::string::npos) {
          d1s = full.substr(0, cut);
          d2s = r.truncatedText(descFont, full.substr(cut + 1).c_str(), textW2, kBold);
        }
      }
      d1 = d1s.c_str();
      d2 = d2s.c_str();
    }
    const int dh = r.getLineHeight(descFont);
    const int lines = d2[0] ? 2 : 1;
    const int blockH = lines * dh + (compact ? 0 : r.getLineHeight(kFontSmall));
    int ty = y + (iconS - blockH) / 2;
    r.drawText(descFont, textX, ty, d1, true, kBold);
    if (d2[0]) r.drawText(descFont, textX, ty + dh, d2, true, kBold);
    if (!compact) {
      char feels[32], fd[16];
      fmtDeg(fd, sizeof(fd), w0.feels);
      std::snprintf(feels, sizeof(feels), "%s %s", WL.feels, fd);
      r.drawText(kFontSmall, textX, ty + dh, feels, true);
    }
    y += iconS + 6;
  }

  // Строка 3: диапазон (↓ ↑), ветер, осадки — значками вместо слов.
  {
    RichItems it;
    char a[16], b[16], wind[24], pr[40], mm[16];
    if (compact) {
      char fd[16], feels[32];
      fmtDeg(fd, sizeof(fd), w0.feels);
      std::snprintf(feels, sizeof(feels), "%s %s", WL.feels, fd);
      it.add(Glyph::None, "%s", feels);
    }
    fmtDeg(a, sizeof(a), w0.tMin);
    fmtDeg(b, sizeof(b), w0.tMax);
    it.add(Glyph::TempMin, "%s", a);
    it.add(Glyph::TempMax, "%s", b);
    if (std::isnan(w0.windMs)) {
      std::snprintf(wind, sizeof(wind), "%s", kDash);
    } else {
      std::snprintf(wind, sizeof(wind), "%ld %s", std::lround(w0.windMs), WL.windUnit);
    }
    it.add(Glyph::Wind, "%s", wind);
    fmtMm(mm, sizeof(mm), w0.precipMm, lang);
    if (std::isnan(w0.precipMm)) {
      std::snprintf(pr, sizeof(pr), "%s", mm);
    } else if (w0.precipProb >= 0) {
      std::snprintf(pr, sizeof(pr), "%s %s (%d %%)", mm, WL.mmUnit, w0.precipProb);
    } else {
      std::snprintf(pr, sizeof(pr), "%s %s", mm, WL.mmUnit);
    }
    it.add(Glyph::Drop, "%s", pr);
    y += drawRichRows(r, kFontSmall, x + pad / 2, w - pad, y, it, kBold) + 2;
  }

  // Строка 4: солнце — считается офлайн по месту, работает и без сети.
  {
    const auto& CL = calendar_core::labels(lang);
    const auto sun = sun_times::compute(t.year, t.month, t.day, c.place.lat, c.place.lon, t.utcOffsetMin);
    RichItems it;
    if (!sun.valid) {
      it.add(Glyph::Sunrise, "%s", kDash);
      it.add(Glyph::Sunset, "%s", kDash);
    } else if (sun.polarDay || sun.polarNight) {
      it.add(Glyph::None, "%s", sun.polarDay ? CL.polarDay : CL.polarNight);
    } else {
      char a[12], b[12], d[28];
      std::snprintf(a, sizeof(a), "%02d:%02d", sun.sunriseMin / 60, sun.sunriseMin % 60);
      std::snprintf(b, sizeof(b), "%02d:%02d", sun.sunsetMin / 60, sun.sunsetMin % 60);
      std::snprintf(d, sizeof(d), "%d %s %02d %s", sun.daylightMin / 60, CL.hoursShort, sun.daylightMin % 60,
                    CL.minutesShort);
      it.add(Glyph::Sunrise, "%s", a);
      it.add(Glyph::Sunset, "%s", b);
      it.add(Glyph::Daylight, "%s", d);
    }
    y += drawRichRows(r, kFontSmall, x + pad / 2, w - pad, y, it, kBold) + 4;
  }

  if (bottomRule) {
    r.drawLine(left, y, left + inner, y, 2, true);
    y += 2;
  }
  hit.add(left, y0, inner, y - y0, cal_detail::Act::OpenWeather);  // тап по блоку погоды → экран «Погода»
  return y - y0;
}

// ---------------------------------------------------------------------------
// Сетка месяца.
//   сегодня           — чёрная плашка, белая жирная цифра
//   выходные/праздники — редкий растр за цифрой (в BW и на сером экране одинаково)
//   соседние месяцы   — мелкий шрифт
// ---------------------------------------------------------------------------
constexpr int kGridTitleH = 40;
constexpr int kGridDowH = 30;

// Число дня в ячейке — сгенерированным цифровым шрифтом (крупнее 12 pt интерфейса), по центру ячейки.
void drawGridNumber(GfxRenderer& r, int cx, int cy, int cw, int rowH, const char* num, bool black) {
  r.drawText(kFontDay, cx + (cw - textW(r, kFontDay, num)) / 2, cy + (rowH - kDayDigitH) / 2 - kDayTopOffset, num, black);
}

int gridRowH(int rows, int availH) {
  return std::clamp((availH - kGridTitleH - kGridDowH) / std::max(rows, 1), kMinRowH, kMaxRowH);
}

void drawMonthGrid(GfxRenderer& r, int x, int y, int w, int availH, int viewYear, unsigned viewMonth, const Today& t,
                   bool browsing, Lang lang, const cal_detail::Ctx& ctx, cal_detail::HitMap& hit) {
  MonthGrid g;
  calendar_core::buildMonthGrid(viewYear, viewMonth, g);
  const int rowH = gridRowH(g.rows, availH);

  // Заголовок «Сентябрь 2026». Не на текущем месяце — инверсная плашка: видно, что листаем.
  char title[40];
  std::snprintf(title, sizeof(title), "%s %d", calendar_core::monthName(lang, viewMonth), viewYear);
  const int tw = textW(r, kFontText, title, kBold);
  const int tx = x + (w - tw) / 2;
  hit.add(x, y, w, kGridTitleH, cal_detail::Act::OpenYear);  // тап по названию месяца → «Год»
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

      hit.add(cx + inset, cy + inset, cw - 2 * inset, rowH - 2 * inset, cal_detail::Act::OpenDay,
              cal_detail::packDate(cell.year, cell.month, cell.day));  // тап по числу → «День»
      const bool isToday = cell.kind == CellKind::CurrentMonth && cell.year == t.year && cell.month == t.month &&
                           cell.day == t.day;
      if (isToday) {
        r.fillRoundedRect(cx + inset, cy + inset, cw - 2 * inset, rowH - 2 * inset, 10, Color::Black);
        drawGridNumber(r, cx, cy, cw, rowH, num, false);
        continue;
      }
      // Выходные И праздники/переносы (по производственному календарю; нет данных — выходные + фиксированные праздники).
      if (cell.kind == CellKind::CurrentMonth && cal_detail::dayInfo(ctx, cell.year, cell.month, cell.day).off) {
        r.fillRectDither(cx + inset, cy + inset, cw - 2 * inset, rowH - 2 * inset, Color::LightGray);
      }
      if (cell.kind == CellKind::CurrentMonth) {
        drawGridNumber(r, cx, cy, cw, rowH, num, true);
      } else {
        r.drawText(kFontSmall, cx + (cw - textW(r, kFontSmall, num)) / 2, cy + (rowH - r.getLineHeight(kFontSmall)) / 2,
                   num, true);
      }
    }
  }
}

}  // namespace

// ===========================================================================

CalendarFace* CalendarFace::active_ = nullptr;

bool CalendarFace::takeSnapshot(Snapshot& s) {
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  const int utcOffsetMin = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  // tick() зовут на каждом такте loop() (сотни раз в секунду), а меняется снимок раз в минуту: пока минута та же
  // (и пояс тот же), календарные поля пересчитывать не нужно — обновляем только момент.
  if (now && s.valid && now / 60u == s.minuteKey && utcOffsetMin == s.utcOffsetMin) {
    s.epoch = now;
    return true;
  }
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
  s.utcOffsetMin = utcOffsetMin;
  return true;
}

void CalendarFace::onEnter() {
  monthOffset_ = 0;
  updatesSinceCleanup_ = 0;
  wantGhostCleanup_ = false;
  lastNavMs_ = millis();
  lastInputMs_ = millis();
  st_ = cal_detail::State{};
  hit_.clear(0, 0);
  takeSnapshot(snap_);
  active_ = this;
}

void CalendarFace::onExit() {
  active_ = nullptr;
  weather_.stop();  // если успели поднять Wi-Fi — выключить
  renderer_ = nullptr;
  snap_ = Snapshot{};
  st_ = cal_detail::State{};
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
  lastInputMs_ = millis();
}

void CalendarFace::onPagePrev() { shiftMonth(-1); }
void CalendarFace::onPageNext() { shiftMonth(+1); }

// ---- Вложенные экраны: состояние и ввод ------------------------------------------------------------------------

void CalendarFace::closeDetail() {
  // Возврат на главный экран — резкая смена содержимого (карточки/график/сетка → время и месяц); один FAST_REFRESH
  // этого не отрисует чисто на этой панели, отсюда жалоба «остаётся остаточное изображение».
  if (st_.screen != cal_detail::Screen::Main) wantGhostCleanup_ = true;
  st_.screen = cal_detail::Screen::Main;
}

void CalendarFace::shiftDay(int days) {
  const int32_t z = calendar_core::daysFromCivil(st_.dayY, st_.dayM, st_.dayD) + days;
  int y;
  unsigned m, d;
  calendar_core::civilFromDays(z, y, m, d);
  if (y < calendar_core::kMinYear || y > calendar_core::kMaxYear) return;
  st_.dayY = y;
  st_.dayM = m;
  st_.dayD = d;
}

void CalendarFace::shiftYear(int delta) {
  const int y = st_.year + delta;
  if (y >= calendar_core::kMinYear && y <= calendar_core::kMaxYear) st_.year = y;
}

// Полугодия идут подряд через границу года: … → июль–дек 2026 → янв–июнь 2027 → …
void CalendarFace::shiftHalf(int delta) {
  int idx = st_.year * 2 + st_.half + delta;
  const int y = idx / 2;
  if (y < calendar_core::kMinYear || y > calendar_core::kMaxYear) return;
  st_.year = y;
  st_.half = static_cast<uint8_t>(idx % 2);
}

void CalendarFace::showMonth(int year, unsigned month) {
  int y = 0;
  unsigned m = 0;
  const int off = (year - snap_.year) * 12 + static_cast<int>(month) - static_cast<int>(snap_.month);
  if (calendar_core::addMonths(snap_.year, snap_.month, off, y, m)) monthOffset_ = off;
  lastNavMs_ = millis();
  closeDetail();
}

void CalendarFace::applyHit(const cal_detail::Hit& h) {
  using cal_detail::Act;
  using cal_detail::Screen;
  const Screen prevScreen = st_.screen;
  switch (h.act) {
    case Act::OpenWeather:
      st_.screen = Screen::Weather;
      st_.page = 0;
      break;
    case Act::OpenWeek:
      st_.screen = Screen::Weather;
      st_.page = 1;
      break;
    case Act::OpenDay:
      st_.screen = Screen::Day;
      cal_detail::unpackDate(h.arg, st_.dayY, st_.dayM, st_.dayD);
      break;
    case Act::OpenYear: {
      int y = snap_.year;
      unsigned m = snap_.month;
      calendar_core::addMonths(snap_.year, snap_.month, monthOffset_, y, m);
      st_.screen = Screen::Year;
      st_.year = y;
      st_.half = m > 6 ? 1 : 0;
      break;
    }
    case Act::Close:
      closeDetail();
      break;
    case Act::GoToday:
      st_.dayY = snap_.year;
      st_.dayM = snap_.month;
      st_.dayD = snap_.day;
      st_.year = snap_.year;
      st_.half = snap_.month > 6 ? 1 : 0;
      break;
    case Act::Prev:
      shiftDay(-1);
      break;
    case Act::Next:
      shiftDay(+1);
      break;
    case Act::OpenMonth:
      showMonth(h.arg / 100, static_cast<unsigned>(h.arg % 100));
      break;
    case Act::None:
      break;
  }
  // Тип экрана поменялся (главный ⇄ вложенный, «День» ⇄ «Погода» по тапу на карточке…) — так же резко, как и закрытие.
  if (st_.screen != prevScreen) wantGhostCleanup_ = true;
}

bool CalendarFace::handleInput(MappedInputManager& input, bool /*immersive*/) {
  return active_ != nullptr && active_->onInput(input);
}

bool CalendarFace::onInput(MappedInputManager& in) {
  using B = MappedInputManager::Button;
  using SD = MappedInputManager::SwipeDir;
  using cal_detail::Screen;
  if (!snap_.valid) return false;

  int tx = 0, ty = 0;
  const bool tapped = in.wasScreenTapped(tx, ty);
  const SD swipe = in.wasSwipe();
  const bool back = in.wasReleased(B::Back), conf = in.wasReleased(B::Confirm);
  const bool bl = in.wasReleased(B::Left), br = in.wasReleased(B::Right), bu = in.wasReleased(B::Up),
             bd = in.wasReleased(B::Down);
  if (!tapped && swipe == SD::None && !back && !conf && !bl && !br && !bu && !bd) return false;

  RenderLock lock;  // st_ и hit_ читает render() из другой задачи

  if (st_.screen == Screen::Main) {
    // Главный экран: поглощаем только тап по тап-зоне; остальное (инверсия, смена грани, Back…) — по-старому.
    if (!tapped || !renderer_) return false;
    if (hit_.vw != renderer_->getScreenWidth() || hit_.vh != renderer_->getScreenHeight()) {  // карта от другой ориентации
      LOG_DBG("STANDBY", "Calendar tap (%d,%d): hit map %dx%d != screen %dx%d", tx, ty, hit_.vw, hit_.vh,
              renderer_->getScreenWidth(), renderer_->getScreenHeight());
      return false;
    }
    const cal_detail::Hit* h = hit_.at(tx, ty);
    LOG_DBG("STANDBY", "Calendar tap (%d,%d) -> %s", tx, ty, h ? "zone" : "no zone");
    if (!h) return false;
    lastInputMs_ = millis();
    applyHit(*h);
    return true;
  }

  lastInputMs_ = millis();
  if (back) {
    closeDetail();
    return true;
  }
  // Направления: «следующее» — палец влево / кнопка Right; вертикально — палец вверх / кнопка Down (как у активности).
  const bool next = swipe == SD::Left || br, prev = swipe == SD::Right || bl;
  const bool vNext = swipe == SD::Up || bd, vPrev = swipe == SD::Down || bu;

  switch (st_.screen) {
    case Screen::Weather:
      if (next) st_.page = 1;
      if (prev) st_.page = 0;
      if (conf) weather_.requestRefresh();
      break;
    case Screen::Day:
      if (next) shiftDay(+1);
      if (prev) shiftDay(-1);
      if (vNext) shiftDay(+7);
      if (vPrev) shiftDay(-7);
      if (conf) applyHit({0, 0, 0, 0, cal_detail::Act::GoToday, 0});
      break;
    case Screen::Year:
      if (next) shiftHalf(+1);  // свайп/кнопки ←/→ — полугодия, ↑/↓ — годы
      if (prev) shiftHalf(-1);
      if (vNext) shiftYear(+1);
      if (vPrev) shiftYear(-1);
      if (conf) applyHit({0, 0, 0, 0, cal_detail::Act::GoToday, 0});
      break;
    case Screen::Main:
      break;
  }
  if (tapped && hit_.vw == (renderer_ ? renderer_->getScreenWidth() : -1) && hit_.vh == renderer_->getScreenHeight()) {
    if (const cal_detail::Hit* h = hit_.at(tx, ty)) applyHit(*h);
  }
  return true;  // на вложенном экране поглощаем весь ввод
}

cal_detail::Ctx CalendarFace::makeCtx(const cal_draw::Today& t, calendar_core::Lang lang) const {
  return cal_detail::Ctx{t, lang, weather_.cache(), weather_.holidays()};
}

// ---- Такт ---------------------------------------------------------------------------------------------------------

StandbyFace::TickResult CalendarFace::tick() {
  const bool hadClock = snap_.valid;
  const uint32_t prevMinute = snap_.minuteKey;
  const uint32_t prevDay = snap_.dayKey;
  takeSnapshot(snap_);

  if (hadClock != snap_.valid) return TickResult::Redraw;  // часы появились или пропали
  if (!snap_.valid) return TickResult::None;

  const bool detail = st_.screen != cal_detail::Screen::Main;

  // Вложенный экран закрывается сам после kDetailAutoCloseSec без ввода.
  if (detail && millis() - lastInputMs_ >= calendar_config::kDetailAutoCloseSec * 1000u) {
    closeDetail();
    return TickResult::RedrawWithGhostCleanup;
  }

  // Что смотрит пользователь → какой год производственного календаря нужен (если его нет в кэше — запросим по требованию).
  int needYear = snap_.year;
  {
    unsigned m = 0;
    calendar_core::addMonths(snap_.year, snap_.month, monthOffset_, needYear, m);
  }
  if (st_.screen == cal_detail::Screen::Day) needYear = st_.dayY;
  if (st_.screen == cal_detail::Screen::Year) needYear = st_.year;
  weather_.wantYear(needYear);
  weather_.setUi(detail, millis() - lastInputMs_);

  // Погода и календарь: шаг конечного автомата. Может занять секунды (блокирующий HTTP) — поэтому после него
  // снимок времени устарел; следующий tick() его обновит.
  const bool netChanged = weather_.step(renderer_, snap_.epoch, currentLang());

  if (detail) {  // минутные перерисовки под открытым экраном не нужны (времени на нём нет)
    return (netChanged || snap_.dayKey != prevDay) ? TickResult::Redraw : TickResult::None;
  }

  // Вернуться к текущему месяцу, если давно не листали.
  if (monthOffset_ != 0 && millis() - lastNavMs_ >= calendar_config::kMonthAutoReturnSec * 1000u) {
    monthOffset_ = 0;
    return TickResult::Redraw;
  }

  if (snap_.dayKey != prevDay) {
    updatesSinceCleanup_ = 0;
    // RedrawWithGhostCleanup доходит до HALF_REFRESH только на Xteink-платах (StandbyActivity сама решает так);
    // на Paper Mono просим то же самое сами — см. wantGhostCleanup_ в render().
    wantGhostCleanup_ = true;
    return TickResult::RedrawWithGhostCleanup;  // полночь: сетка и дата меняются целиком
  }
  if (snap_.minuteKey != prevMinute) {
    if (++updatesSinceCleanup_ >= calendar_config::kGhostCleanupEveryUpdates) {
      updatesSinceCleanup_ = 0;
      wantGhostCleanup_ = true;  // плановая чистка: время и сегодняшняя плашка стоят на месте много минут подряд
      return TickResult::RedrawWithGhostCleanup;
    }
    return TickResult::Redraw;
  }
  return netChanged ? TickResult::Redraw : TickResult::None;
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
  if (wantGhostCleanup_) {
    // StandbyActivity::render() вызывает r.displayBuffer() сразу после этого render() и без override берёт
    // FAST_REFRESH; requestNextRefresh() — обычный публичный метод GfxRenderer (не хук), он подменит режим этого
    // одного кадра на HALF_REFRESH, что на партиях чёрного (крупное время, плашка «сегодня», карточки вложенных
    // экранов) чистит остаточное изображение, которое FAST_REFRESH не трогает. Тот же режим StandbyActivity сама
    // использует для RedrawWithGhostCleanup на Xteink-платах — здесь просим его напрямую, потому что Paper Mono
    // под тот путь не попадает (gpio.isXteinkDevice() == false).
    r.requestNextRefresh(HalDisplay::HALF_REFRESH);
    wantGhostCleanup_ = false;
    LOG_DBG("STANDBY", "Calendar: HALF_REFRESH requested (ghost cleanup)");
  }
  ensureDigitFonts(r);
  const Lang lang = currentLang();
  drawStatusRow(r, vp, (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15);
  hit_.clear(r.getScreenWidth(), r.getScreenHeight());
  if (!snap_.valid && !takeSnapshot(snap_)) {
    // Часам нельзя верить: активность сама пишет «синхронизация», мы — подсказку по центру (плюс строка статуса выше).
    drawCentered(r, kFontText, vp.x, vp.width, vp.y + vp.height / 2, calendar_core::labels(lang).noClock, true, kBold);
    return;
  }

  const Today t{snap_.year,    snap_.month,      snap_.day,        snap_.hour,       snap_.minute,
                snap_.weekday, snap_.dayOfYear,  snap_.isoWeek,    snap_.daysInYear, snap_.utcOffsetMin,
                snap_.epoch};
  const cal_detail::Ctx ctx = makeCtx(t, lang);

  if (st_.screen != cal_detail::Screen::Main) {
    cal_detail::draw(r, vp, st_, ctx, hit_);
    return;
  }

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
    drawWeatherBlock(r, vp.x, top + infoH, leftW, wx, t, lang, /*bottomRule=*/false, /*compact=*/true, hit_);
    // Точки-пейджер внизу стоят по центру экрана, правее левой колонки, — разделитель идёт до самого низа.
    r.drawLine(vp.x + leftW, vp.y + kTopReserve, vp.x + leftW, vp.y + vp.height - 8, 2, true);
    const int gx = vp.x + leftW + kSidePad;
    const int gw = vp.width - leftW - 2 * kSidePad;
    drawMonthGrid(r, gx, top, gw, bottom - top, viewYear, viewMonth, t, browsing, lang, ctx, hit_);
    return;
  }

  // Портрет: время и дата, погода, сетка месяца книзу.
  const int infoH = drawInfoBlock(r, kFontTimeXl, kTimeXlTopOffset, kTimeXlDigitH, vp.x, top, vp.width, t, lang, kSidePad);
  const int wxH = drawWeatherBlock(r, vp.x, top + infoH, vp.width, wx, t, lang, /*bottomRule=*/true, /*compact=*/false, hit_);

  MonthGrid probe;
  calendar_core::buildMonthGrid(viewYear, viewMonth, probe);
  const int gridAvail = bottom - (top + infoH + wxH) - kMinGap;
  const int gridH = kGridTitleH + kGridDowH + gridRowH(probe.rows, gridAvail) * probe.rows;
  drawMonthGrid(r, vp.x + kSidePad, bottom - gridH, vp.width - 2 * kSidePad, gridH, viewYear, viewMonth, t, browsing, lang,
                ctx, hit_);
}
