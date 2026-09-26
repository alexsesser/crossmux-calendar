#include "CalendarFace.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

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
#include "CalendarLog.h"
#include "CrossPointSettings.h"
#include "HolidayCore.h"
#include "I18nKeys.h"
#include "SunTimes.h"
#include "WeatherCore.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/util/KeyboardEntryActivity.h"
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

const char* screenName(cal_detail::Screen s) {
  switch (s) {
    case cal_detail::Screen::Main:
      return "главный";
    case cal_detail::Screen::Weather:
      return "погода";
    case cal_detail::Screen::Day:
      return "день";
    case cal_detail::Screen::Year:
      return "год";
    case cal_detail::Screen::Place:
      return "место";
  }
  return "?";
}

// Ответ клавиатуры «найти город». Клавиатура — отдельная активность поверх стендбая; грань при этом жива (стендбай лежит
// на стеке активностей) и забирает ответ в ближайшем tick() после возврата. Обе стороны — главная задача.
struct CityMailbox {
  bool ready = false;
  bool cancelled = false;
  int mode = 0;  // см. CalendarFace::openKeyboard
  std::string text;
};
CityMailbox g_cityBox;

// KeyboardEntryActivity отдаёт текст только своей активности-«родителю» (startActivityForResult), а грань — не
// активность. Поэтому клавиатура — наследник, который сам кладёт ответ в ящик в тот момент, когда её закрыли
// (onComplete/onCancel делают setResult + finish(), и менеджер активностей ставит смену в очередь).
class CityEntryActivity final : public KeyboardEntryActivity {
 public:
  using KeyboardEntryActivity::KeyboardEntryActivity;
  void loop() override {
    KeyboardEntryActivity::loop();
    if (done_ || !activityManager.isSwitchPending()) return;
    done_ = true;
    const auto* kb = std::get_if<KeyboardResult>(&result.data);
    g_cityBox.cancelled = result.isCancelled || !kb;
    g_cityBox.text = kb ? kb->text : std::string();
    g_cityBox.ready = true;
  }

 private:
  bool done_ = false;
};

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
                     bool compact, bool manualPlace, cal_detail::HitMap& hit) {
  const auto& WL = weather_core::labels(lang);
  const int y0 = y;
  const int pad = compact ? kCompactPad : kSidePad;
  const int left = x + pad;
  const int inner = w - 2 * pad;
  int cityZoneW = 0, cityZoneH = 0;

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
    const int updW = upd[0] ? textW(r, kFontSmall, upd) : 0;
    const char* cityRaw = c.place.city[0] ? c.place.city : WL.unknownPlace;
    // Место задано вручную (экран «Место») — метка перед названием.
    const int pinW = manualPlace ? r.getLineHeight(kFontSmall) + 3 : 0;
    if (manualPlace) drawGlyph(r, Glyph::Pin, left, y, r.getLineHeight(kFontSmall));
    const Fit city(r, kFontSmall, cityRaw, inner - pinW - updW - (updW ? 12 : 0), kBold);
    r.drawText(kFontSmall, left + pinW, y, city.c_str(), true, kBold);
    if (upd[0]) r.drawText(kFontSmall, left + inner - updW, y, upd, true);
    y += r.getLineHeight(kFontSmall) + 4;
    // Тап по названию города → экран «Место» (добавляется в конце: зона поверх зоны всего блока).
    cityZoneW = std::max(pinW + textW(r, kFontSmall, city.c_str(), kBold) + 24, 120);
    cityZoneH = y - y0 + 8;
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
  hit.add(left, y0, std::min(cityZoneW, inner), cityZoneH, cal_detail::Act::OpenPlace);
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
  cal_log::line("FACE", "календарь открыт; заряд %u%%", static_cast<unsigned>(powerManager.getBatteryPercentage()));
  monthOffset_ = 0;
  updatesSinceCleanup_ = 0;
  cleanupWhy_ = nullptr;
  keyboardOpen_ = false;
  g_cityBox = CityMailbox{};
  lastNavMs_ = millis();
  lastInputMs_ = millis();
  st_ = cal_detail::State{};
  hit_.clear(0, 0);
  takeSnapshot(snap_);
  active_ = this;
}

void CalendarFace::onExit() {
  // Уходим из стендбая (домой, в сон и т.п.) — менеджер активностей держит RenderLock; смена грани — не держит.
  const bool leavingStandby = activityManager.isSwitchPending();
  cal_log::line("FACE", "календарь закрыт (%s)", leavingStandby ? "выход из стендбая" : "другая грань");
  active_ = nullptr;
  weather_.stop(leavingStandby);  // идущий выход в сеть сам быстро закончит и выключит Wi-Fi
  // Полной очистки (белый экран) при выходе больше нет: след цифр под обложкой она не убирала (§13.10), а мигала.
  cal_log::pump(/*forceFlush=*/true, /*lockHeld=*/leavingStandby);
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
  // Возврат на главный экран — резкая смена содержимого (карточки/график/сетка → время и месяц); быстрое обновление
  // этой панели оставляет остаточное изображение.
  if (st_.screen != cal_detail::Screen::Main) {
    cal_log::line("UI", "экран: %s -> главный", screenName(st_.screen));
    requestCleanup("возврат на главный");
  }
  st_.screen = cal_detail::Screen::Main;
}

void CalendarFace::requestCleanup(const char* why) { cleanupWhy_ = why; }

void CalendarFace::openKeyboard(int mode) {
  if (!renderer_ || !input_) return;
  // Заголовок клавиатуры рисуется шрифтом без кириллицы (CONCEPT §4.6) — латиницей.
  const bool de = currentLang() == Lang::De;
  const char* title = mode == 1 ? "Lat, Lon (55.75, 37.62)" : mode == 2 ? "Name" : (de ? "Ort" : "City");
  auto kb = makeUniqueNoThrow<CityEntryActivity>(*renderer_, *input_, title, std::string(), 40, InputType::Text);
  if (!kb) {
    LOG_ERR("STANDBY", "OOM: city keyboard");
    cal_log::line("UI", "нет памяти под клавиатуру");
    return;
  }
  g_cityBox = CityMailbox{};
  g_cityBox.mode = mode;
  keyboardOpen_ = true;
  renderer_->requestNextRefresh(HalDisplay::FULL_REFRESH);  // клавиатура поверх календаря — с полной очисткой
  requestCleanup("возврат с клавиатуры");                   // и обратно — тоже
  cal_log::line("UI", "клавиатура: %s", mode == 1 ? "координаты" : mode == 2 ? "подпись места" : "поиск города");
  activityManager.pushActivity(std::move(kb));
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
      if (st_.screen == Screen::Place && placeFrom_ == Screen::Weather) {  // «Место» открыли с «Погоды» — туда и назад
        st_.screen = Screen::Weather;
        st_.page = 0;
      } else {
        closeDetail();
      }
      break;
    case Act::OpenPlace:
      placeFrom_ = prevScreen;
      placeNote_[0] = '\0';
      st_.screen = Screen::Place;
      break;
    case Act::AddCoords:
      placeNote_[0] = '\0';
      openKeyboard(1);
      break;
    case Act::PickSaved:
      placeNote_[0] = '\0';
      weather_.pickSaved(h.arg);
      break;
    case Act::DeleteSaved:
      weather_.removeSaved(h.arg);
      break;
    case Act::Refresh:
      weather_.requestRefresh();
      cal_log::line("UI", "погода: обновить сейчас");
      break;
    case Act::SetAuto:
      weather_.setAutoLocation(true);
      break;
    case Act::SetManual:
      weather_.setAutoLocation(false);
      break;
    case Act::SearchCity:
      openCitySearch();
      break;
    case Act::PinIp:
      weather_.pinIpPlace();
      break;
    case Act::PickHit:
      weather_.pickSearchHit(h.arg);
      break;
    case Act::ToggleLog:
      weather_.setSdLog(!weather_.settings().sdLog);
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
  // Возврат на главный уже записал и заказал closeDetail().
  if (st_.screen != prevScreen && st_.screen != Screen::Main) {
    cal_log::line("UI", "экран: %s -> %s", screenName(prevScreen), screenName(st_.screen));
    requestCleanup("смена экрана");
  }
}

bool CalendarFace::handleInput(MappedInputManager& input, bool /*immersive*/) {
  if (!active_) return false;
  active_->input_ = &input;
  return active_->onInput(input);
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
    applyHit({0, 0, 0, 0, cal_detail::Act::Close, 0});  // «Место» → «Погода», остальные → главный
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
    case Screen::Place:  // кнопками: ← авто, → вручную, Confirm — найти город
      if (bl) weather_.setAutoLocation(true);
      if (br) weather_.setAutoLocation(false);
      if (conf) openCitySearch();
      break;
    case Screen::Main:
      break;
  }
  if (tapped && hit_.vw == (renderer_ ? renderer_->getScreenWidth() : -1) && hit_.vh == renderer_->getScreenHeight()) {
    if (const cal_detail::Hit* h = hit_.at(tx, ty)) applyHit(*h);
  }
  return true;  // на вложенном экране поглощаем весь ввод
}

cal_detail::PlaceView CalendarFace::placeView() const {
  cal_detail::PlaceView pv;
  const weather_core::Settings& set = weather_.settings();
  pv.autoLocation = set.autoLocation;
  pv.ip = &weather_.ipPlace();
  pv.manual = &set.manual;
  switch (weather_.searchState()) {
    case WeatherClient::Search::Idle:
      pv.search = cal_detail::SearchState::Idle;
      break;
    case WeatherClient::Search::Waiting:
      pv.search = cal_detail::SearchState::Waiting;
      break;
    case WeatherClient::Search::Found:
      pv.search = cal_detail::SearchState::Found;
      break;
    case WeatherClient::Search::NotFound:
      pv.search = cal_detail::SearchState::NotFound;
      break;
    case WeatherClient::Search::Failed:
      pv.search = cal_detail::SearchState::Failed;
      break;
  }
  pv.query = weather_.searchQuery();
  pv.nHits = weather_.searchCount();
  pv.hits = pv.nHits ? &weather_.searchHit(0) : nullptr;
  pv.sdLog = set.sdLog;
  pv.logDir = cal_log::dirPath();
  pv.saved = set.saved;
  pv.nSaved = set.nSaved;
  pv.note = placeNote_;
  return pv;
}

cal_detail::Ctx CalendarFace::makeCtx(const cal_draw::Today& t, calendar_core::Lang lang,
                                      const cal_detail::PlaceView& pv) const {
  cal_detail::HistState hs = cal_detail::HistState::None;
  if (st_.screen == cal_detail::Screen::Day) {
    switch (weather_.historyState(cal_detail::packDate(st_.dayY, st_.dayM, st_.dayD))) {
      case WeatherClient::Hist::Waiting:
        hs = cal_detail::HistState::Waiting;
        break;
      case WeatherClient::Hist::Failed:
        hs = cal_detail::HistState::Failed;
        break;
      case WeatherClient::Hist::None:
        break;
    }
  }
  return cal_detail::Ctx{t, lang, weather_.cache(), weather_.holidays(), pv, weather_.history(), hs, weather_.refreshing()};
}

// ---- Такт ---------------------------------------------------------------------------------------------------------

StandbyFace::TickResult CalendarFace::tick() {
  cal_log::pump();  // журнал: сообщения прошивки + раз в kSdLogFlushSec запись на карту

  // Вернулись с клавиатуры «найти город»: координаты — сразу ручное место, название — искать в сети.
  if (g_cityBox.ready) {
    g_cityBox.ready = false;
    keyboardOpen_ = false;
    lastInputMs_ = millis();  // пока была клавиатура, tick() не вызывался: не закрыть «Место» по таймауту сразу
    const int mode = g_cityBox.mode;
    if (mode == 2) {  // подпись к введённым координатам; пусто или «отмена» — подписью будут сами координаты
      RenderLock lock;
      weather_core::Place p;
      if (!g_cityBox.cancelled && !g_cityBox.text.empty()) {
        weather_core::copyUtf8(p.city, sizeof(p.city), g_cityBox.text.c_str());
      } else {
        std::snprintf(p.city, sizeof(p.city), "%.4f, %.4f", pendingLat_, pendingLon_);
      }
      p.lat = pendingLat_;
      p.lon = pendingLon_;
      weather_.clearSearch();
      weather_.setManualPlace(p);  // и в сохранённые
      return TickResult::Redraw;
    }
    if (mode == 1 && !g_cityBox.cancelled && !g_cityBox.text.empty()) {
      double la = 0, lo = 0;
      if (weather_core::parseCoords(g_cityBox.text.c_str(), la, lo)) {
        pendingLat_ = la;
        pendingLon_ = lo;
        openKeyboard(2);  // теперь — подпись
      } else {
        RenderLock lock;
        std::snprintf(placeNote_, sizeof(placeNote_), cal_detail::notCoordsFmt(currentLang()), g_cityBox.text.c_str());
      }
      return TickResult::Redraw;
    }
    if (mode == 0 && !g_cityBox.cancelled && !g_cityBox.text.empty()) {
      RenderLock lock;  // меняем то, что читает render()
      double la = 0, lo = 0;
      if (weather_core::parseCoords(g_cityBox.text.c_str(), la, lo)) {
        weather_core::Place p;
        std::snprintf(p.city, sizeof(p.city), "%.4f, %.4f", la, lo);
        p.lat = la;
        p.lon = lo;
        weather_.clearSearch();
        weather_.setManualPlace(p);
      } else {
        weather_.startSearch(g_cityBox.text.c_str());
      }
    } else {
      cal_log::line("UI", "клавиатура закрыта без ввода");
    }
    return TickResult::Redraw;
  }

  const bool hadClock = snap_.valid;
  const uint32_t prevMinute = snap_.minuteKey;
  const uint32_t prevDay = snap_.dayKey;
  takeSnapshot(snap_);

  if (hadClock != snap_.valid) return TickResult::Redraw;  // часы появились или пропали
  if (!snap_.valid) return TickResult::None;

  const bool detail = st_.screen != cal_detail::Screen::Main;
  // «День» с прошедшей датой — архив погоды для неё (клиент сам решит, нужен ли запрос).
  if (st_.screen == cal_detail::Screen::Day) {
    weather_.wantHistory(cal_detail::packDate(st_.dayY, st_.dayM, st_.dayD),
                         cal_detail::packDate(snap_.year, snap_.month, snap_.day));
  }

  // Вложенный экран закрывается сам после kDetailAutoCloseSec без ввода. «Место» с незавершённым поиском — ждёт ответа.
  const bool searching = st_.screen == cal_detail::Screen::Place && weather_.searchState() == WeatherClient::Search::Waiting;
  if (detail && !searching && millis() - lastInputMs_ >= calendar_config::kDetailAutoCloseSec * 1000u) {
    RenderLock lock;  // st_ читает render()
    closeDetail();
    return TickResult::RedrawWithGhostCleanup;
  }

  // Раз в 10 минут — состояние в журнал: видно, что устройство живо, чем заряжено, что с Wi-Fi и погодой.
  // Первая — когда кэш уже прочитан (иначе «погоде -1 мин» при каждом открытии).
  if (cal_log::enabled() && weather_.ready() && (lastBeatMs_ == 0 || millis() - lastBeatMs_ >= 10u * 60u * 1000u)) {
    lastBeatMs_ = millis();
    const auto& w = weather_.cache().weather;
    const long age = (w.valid && snap_.epoch >= w.fetchedEpoch) ? static_cast<long>((snap_.epoch - w.fetchedEpoch) / 60) : -1;
    cal_log::line("BEAT", "заряд %u%%, память %u, Wi-Fi режим %d статус %d, экран %s, место %s «%s», погоде %ld мин",
                  static_cast<unsigned>(powerManager.getBatteryPercentage()), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<int>(WiFi.getMode()), static_cast<int>(WiFi.status()), screenName(st_.screen),
                  weather_.settings().autoLocation ? "авто" : "вручную", weather_.cache().place.city, age);
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
    // RedrawWithGhostCleanup StandbyActivity выполняет только на Xteink-платах; на Paper Mono просим очистку сами.
    requestCleanup("полночь");
    return TickResult::RedrawWithGhostCleanup;  // полночь: сетка и дата меняются целиком
  }
  if (snap_.minuteKey != prevMinute) {
    if (calendar_config::kGhostCleanupEveryUpdates && ++updatesSinceCleanup_ >= calendar_config::kGhostCleanupEveryUpdates) {
      updatesSinceCleanup_ = 0;
      requestCleanup("плановая");  // время и сегодняшняя плашка стоят на месте много минут подряд
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
  if (cleanupWhy_) {
    // StandbyActivity::render() вызывает r.displayBuffer() сразу после этого render() и без подмены берёт
    // FAST_REFRESH. requestNextRefresh() — обычный публичный метод GfxRenderer (не хук): этот один кадр пойдёт
    // FULL_REFRESH — так же, как по кнопке «Обновление экрана» в верхнем меню (FrontlightPanelActivity). Драйвер
    // Paper Mono чистит остаточное изображение только им: HALF_REFRESH он выполняет как обычное быстрое обновление.
    r.requestNextRefresh(HalDisplay::FULL_REFRESH);
    cal_log::line("EPD", "полная очистка экрана (%s)", cleanupWhy_);
    cleanupWhy_ = nullptr;
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
  const cal_detail::PlaceView pv = placeView();
  const cal_detail::Ctx ctx = makeCtx(t, lang, pv);

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
    drawWeatherBlock(r, vp.x, top + infoH, leftW, wx, t, lang, /*bottomRule=*/false, /*compact=*/true, !pv.autoLocation, hit_);
    // Точки-пейджер внизу стоят по центру экрана, правее левой колонки, — разделитель идёт до самого низа.
    r.drawLine(vp.x + leftW, vp.y + kTopReserve, vp.x + leftW, vp.y + vp.height - 8, 2, true);
    const int gx = vp.x + leftW + kSidePad;
    const int gw = vp.width - leftW - 2 * kSidePad;
    drawMonthGrid(r, gx, top, gw, bottom - top, viewYear, viewMonth, t, browsing, lang, ctx, hit_);
    return;
  }

  // Портрет: время и дата, погода, сетка месяца книзу.
  const int infoH = drawInfoBlock(r, kFontTimeXl, kTimeXlTopOffset, kTimeXlDigitH, vp.x, top, vp.width, t, lang, kSidePad);
  const int wxH = drawWeatherBlock(r, vp.x, top + infoH, vp.width, wx, t, lang, /*bottomRule=*/true, /*compact=*/false,
                                   !pv.autoLocation, hit_);

  MonthGrid probe;
  calendar_core::buildMonthGrid(viewYear, viewMonth, probe);
  const int gridAvail = bottom - (top + infoH + wxH) - kMinGap;
  const int gridH = kGridTitleH + kGridDowH + gridRowH(probe.rows, gridAvail) * probe.rows;
  drawMonthGrid(r, vp.x + kSidePad, bottom - gridH, vp.width - 2 * kSidePad, gridH, viewYear, viewMonth, t, browsing, lang,
                ctx, hit_);
}
