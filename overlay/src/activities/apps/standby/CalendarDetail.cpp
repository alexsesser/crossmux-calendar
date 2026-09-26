#include "CalendarDetail.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "CalendarConfig.h"
#include "MoonPhase.h"
#include "SunTimes.h"
#include "util/TimeUtils.h"

namespace cal_detail {

using namespace cal_draw;
using calendar_core::Lang;
using weather_core::Icon;

holiday_core::DayInfo dayInfo(const Ctx& c, int y, unsigned m, unsigned d) {
  if (!calendar_config::kHolidaysEnabled) {
    holiday_core::DayInfo r;
    r.off = calendar_core::weekday(y, m, d) >= 5;
    return r;
  }
  return holiday_core::classify(c.hol, calendar_config::kHolidayCountry, y, m, d);
}

namespace {

// ---- Тексты (собственные таблицы ru/en/de, как и остальные тексты грани) ------------------------------------------

struct Txt {
  const char* back;
  const char* today;
  const char* todayLc;   // «сегодня»
  const char* tomorrow;
  const char* yesterday;
  const char* inDays;    // %d
  const char* daysAgo;   // %d
  const char* dayTitle;
  const char* sun;
  const char* moon;
  const char* weather;
  const char* lit;       // %d %%
  const char* lunarDay;  // %d
  const char* nextFull;
  const char* nextNew;
  const char* vsYest;    // %c%d
  const char* fcOnly;    // «Прогноз доступен на 7 дней вперёд»
  const char* sevenDays;
  const char* noFcHint;
  const char* hourly;    // «сегодня»-страница: заголовок по часам
  // Экран «Место». Без «—» и «…»: этих знаков нет во всех подмножествах шрифта (см. kDash).
  const char* placeTitle;
  const char* modeAuto;
  const char* modeManual;
  const char* byIp;
  const char* ipNever;
  const char* ipVia;     // %s сеть, %s когда
  const char* pinIp;
  const char* findCity;
  const char* searching;  // %s запрос
  const char* pick;
  const char* notFound;  // %s запрос
  const char* failed;
  const char* logOn;
  const char* logOff;
  const char* logHint;   // %s папка
  const char* coordsBtn;
  const char* savedTitle;
  const char* notCoords;  // %s ввод
  const char* histArchive;
  const char* histRecorded;
  const char* histLoading;
  const char* histFailed;
  const char* histNone;
  const char* refresh;
  const char* refreshing;
};

const Txt kRu = {"Назад", "Сегодня", "сегодня", "завтра", "вчера", "через %d дн.", "%d дн. назад", "День",
                 "Солнце", "Луна", "Погода", "освещена %d %%", "лунный день %d", "Полнолуние", "новолуние",
                 "%c%d мин к вчера", "Прогноз доступен на 7 дней вперёд", "7 дней",
                 "Загрузится при подключении к Wi-Fi", "сегодня",
                 "Место", "Авто (по IP)", "Вручную", "По IP", "ещё не определялось", "сеть «%s» · %s",
                 "Взять этот город", "Найти город", "Ищу «%s»...", "Выберите город:", "Не найдено: «%s»",
                 "Поиск не удался: нет сети?", "Журнал на SD: вкл", "Журнал на SD: выкл", "%s",
                 "Координаты", "Сохранённые:", "Не координаты: «%s»", "Архив Open-Meteo",
                 "Сохранённый прогноз", "Загружаю архив погоды...", "Архив недоступен: нет связи",
                 "Нет данных за этот день", "Обновить", "Обновляю..."};
const Txt kEn = {"Back", "Today", "today", "tomorrow", "yesterday", "in %d days", "%d days ago", "Day",
                 "Sun", "Moon", "Weather", "%d %% lit", "lunar day %d", "Full moon", "new moon",
                 "%c%d min vs yesterday", "Forecast covers the next 7 days", "7 days",
                 "Loads when Wi-Fi is available", "today",
                 "Location", "Auto (by IP)", "Manual", "By IP", "not detected yet", "network \"%s\" · %s",
                 "Use this city", "Find city", "Searching \"%s\"...", "Pick a city:", "Not found: \"%s\"",
                 "Search failed: no network?", "SD log: on", "SD log: off", "%s",
                 "Coordinates", "Saved:", "Not coordinates: \"%s\"", "Open-Meteo archive",
                 "Saved forecast", "Loading weather archive...", "Archive unavailable: no connection",
                 "No data for this day", "Refresh", "Updating..."};
const Txt kDe = {"Zurück", "Heute", "heute", "morgen", "gestern", "in %d Tagen", "vor %d Tagen", "Tag",
                 "Sonne", "Mond", "Wetter", "%d %% hell", "Mondtag %d", "Vollmond", "Neumond",
                 "%c%d Min zu gestern", "Vorhersage für die nächsten 7 Tage", "7 Tage",
                 "Wird bei WLAN geladen", "heute",
                 "Ort", "Auto (per IP)", "Manuell", "Per IP", "noch nicht ermittelt", "Netz \"%s\" · %s",
                 "Diesen Ort nehmen", "Ort suchen", "Suche \"%s\"...", "Ort wählen:", "Nicht gefunden: \"%s\"",
                 "Suche fehlgeschlagen: kein Netz?", "SD-Log: an", "SD-Log: aus", "%s",
                 "Koordinaten", "Gespeichert:", "Keine Koordinaten: \"%s\"", "Open-Meteo-Archiv",
                 "Gespeicherte Vorhersage", "Lade Wetterarchiv...", "Archiv nicht erreichbar",
                 "Keine Daten für diesen Tag", "Aktualisieren", "Lade..."};

const Txt& txt(Lang l) { return l == Lang::Ru ? kRu : l == Lang::De ? kDe : kEn; }

// ---- Общая геометрия ---------------------------------------------------------------------------------------------

constexpr int kHdrH = 46;
constexpr int kChipH = 36;
constexpr int kNavH = 44;

struct Frame {
  int x, y, w;  // область содержимого под строкой статуса
  int bottom;   // нижняя граница (над точками-пейджером активности)
  bool land;
};

Frame makeFrame(const Rect& vp) {
  return Frame{vp.x, vp.y + calendar_config::kTopReserve, vp.width, vp.y + vp.height - calendar_config::kBottomReserve,
               vp.width > vp.height};
}

int lineH(const GfxRenderer& r, int font) { return r.getLineHeight(font); }

// Кнопка-«таблетка»; filled — чёрная с белым текстом. Возвращает ширину.
int chip(GfxRenderer& r, int x, int y, int h, const char* label, bool filled, int minW = 0) {
  const int w = std::max(minW, textW(r, kFontSmall, label, kBold) + 28);
  if (filled) {
    r.fillRoundedRect(x, y, w, h, h / 2, Color::Black);
  } else {
    r.drawRoundedRect(x, y, w, h, 2, h / 2, true);
  }
  r.drawText(kFontSmall, x + (w - textW(r, kFontSmall, label, kBold)) / 2, y + (h - lineH(r, kFontSmall)) / 2, label,
             !filled, kBold);
  return w;
}

enum class Right { None, Dots, Today, TodayFilled };

// Шапка: «< Назад» · заголовок · (точки страниц | «Сегодня»). Возвращает y под шапкой.
// titleAct — заголовок тоже кнопка (на «Погоде» — открыть «Место»); pin — метка «место задано вручную» перед ним.
int drawHeader(GfxRenderer& r, const Frame& f, const Txt& T, const char* title, Right right, int dotsOn, HitMap& hit,
               Act titleAct = Act::None, bool pin = false) {
  const int pad = calendar_config::kSidePad;
  const int y = f.y;
  const int cy = y + (kHdrH - kChipH) / 2;

  char back[40];
  std::snprintf(back, sizeof(back), "< %s", T.back);
  const int bw = chip(r, f.x + pad, cy, kChipH, back, false);
  hit.add(f.x + pad - 4, y, bw + 8, kHdrH, Act::Close);

  int rw = 0;
  if (right == Right::Today || right == Right::TodayFilled) {
    rw = textW(r, kFontSmall, T.today, kBold) + 28;
    chip(r, f.x + f.w - pad - rw, cy, kChipH, T.today, right == Right::TodayFilled);
    hit.add(f.x + f.w - pad - rw - 4, y, rw + 8, kHdrH, Act::GoToday);
  } else if (right == Right::Dots) {
    rw = 2 * 14 + 8;
    for (int i = 0; i < 2; ++i) {
      const int dx = f.x + f.w - pad - rw + 7 + i * 22;
      if (i == dotsOn) {
        disc(r, dx, y + kHdrH / 2, 7, Color::Black);
      } else {
        r.drawRoundedRect(dx - 7, y + kHdrH / 2 - 7, 14, 14, 2, 7, true);
      }
    }
  }
  // Название — по центру свободного места между кнопкой «Назад» и правым элементом.
  const int tl = f.x + pad + bw + 12, tr = f.x + f.w - pad - rw - 12;
  const int gs = pin ? glyphSize(r, kFontText) : 0, gw = pin ? gs + 4 : 0;
  const Fit tt(r, kFontText, title, tr - tl - gw, kBold);
  int tx = tl + (tr - tl - gw - textW(r, kFontText, tt.c_str(), kBold)) / 2;
  if (pin) {
    drawGlyph(r, Glyph::Pin, tx, y + (kHdrH - gs) / 2, gs);
    tx += gw;
  }
  r.drawText(kFontText, tx, y + (kHdrH - lineH(r, kFontText)) / 2, tt.c_str(), true, kBold);
  if (titleAct != Act::None) hit.add(tl, y, tr - tl, kHdrH, titleAct);
  r.drawLine(f.x + pad, y + kHdrH, f.x + f.w - pad, y + kHdrH, 2, true);
  return y + kHdrH + 8;
}

struct Ymd {
  int y;
  unsigned m, d;
};
Ymd fromEpoch(uint32_t ts, int offSec) {
  Ymd r{};
  const int64_t local = static_cast<int64_t>(ts) + offSec;
  calendar_core::civilFromDays(static_cast<int32_t>(local >= 0 ? local / 86400 : (local - 86399) / 86400), r.y, r.m, r.d);
  return r;
}
int localHour(uint32_t ts, int offSec) {
  const int64_t local = static_cast<int64_t>(ts) + offSec;
  return static_cast<int>(((local % 86400) + 86400) % 86400 / 3600);
}

// ---- Данные погоды: свежесть ---------------------------------------------------------------------------------------

struct WxView {
  weather_core::Weather cur;  // пусто, если данных нет / просрочены
  bool haveCur = false;
  bool stale = false;
  bool expired = false;
  bool fcOk = false;          // прогноз пригоден
};

WxView wxView(const Ctx& c) {
  WxView v;
  const auto& w = c.wx.weather;
  const uint32_t age = c.t.epoch >= w.fetchedEpoch ? c.t.epoch - w.fetchedEpoch : 0;
  if (w.valid) {
    v.expired = age > weather_core::kExpireSec;
    v.stale = age > weather_core::kStaleSec;
    if (!v.expired) {
      v.cur = w;
      v.haveCur = true;
    }
  }
  const auto& f = c.wx.fc;
  const uint32_t fage = c.t.epoch >= f.fetchedEpoch ? c.t.epoch - f.fetchedEpoch : 0;
  v.fcOk = f.valid && fage <= weather_core::kExpireSec;
  return v;
}

void fmtUpdatedLine(char* out, size_t n, const Ctx& c, const WxView& v, Lang lang) {
  const auto& WL = weather_core::labels(lang);
  const uint32_t at = c.wx.fc.valid ? c.wx.fc.fetchedEpoch : c.wx.weather.fetchedEpoch;
  out[0] = '\0';
  if (at == 0) return;
  std::tm lt{};
  if (!TimeUtils::getLocalDateTime(at, lt)) return;
  char when[24];
  if (lt.tm_mday == static_cast<int>(c.t.day) && lt.tm_mon + 1 == static_cast<int>(c.t.month)) {
    std::snprintf(when, sizeof(when), "%02d:%02d", lt.tm_hour, lt.tm_min);
  } else {
    std::snprintf(when, sizeof(when), "%02d.%02d %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min);
  }
  // Источник — обязательная подпись по лицензиям (CC BY 4.0) и Open-Meteo, и MET Norway.
  const weather_core::Provider src = c.wx.fc.valid ? c.wx.fc.provider : c.wx.weather.provider;
  // «обн.» не пишем: рядом кнопка «Обновить»; устаревшие данные — с пометкой.
  if (v.stale || v.expired) {
    std::snprintf(out, n, "%s %s \xC2\xB7 %s", WL.stale, when, weather_core::providerName(src));
  } else {
    std::snprintf(out, n, "%s \xC2\xB7 %s", when, weather_core::providerName(src));
  }
}

// Восход/закат/длина дня строкой из значков (офлайн).
void sunItems(RichItems& it, const Ctx& c, int y, unsigned m, unsigned d, bool withLen) {
  const auto& CL = calendar_core::labels(c.lang);
  const auto s = sun_times::compute(y, m, d, c.wx.place.lat, c.wx.place.lon, c.t.utcOffsetMin);
  if (!s.valid) {
    it.add(Glyph::Sunrise, "%s", kDash);
    it.add(Glyph::Sunset, "%s", kDash);
  } else if (s.polarDay || s.polarNight) {
    it.add(Glyph::None, "%s", s.polarDay ? CL.polarDay : CL.polarNight);
  } else {
    char a[12], b[12], l[28];
    std::snprintf(a, sizeof(a), "%02d:%02d", s.sunriseMin / 60, s.sunriseMin % 60);
    std::snprintf(b, sizeof(b), "%02d:%02d", s.sunsetMin / 60, s.sunsetMin % 60);
    std::snprintf(l, sizeof(l), "%d %s %02d %s", s.daylightMin / 60, CL.hoursShort, s.daylightMin % 60, CL.minutesShort);
    it.add(Glyph::Sunrise, "%s", a);
    it.add(Glyph::Sunset, "%s", b);
    if (withLen) it.add(Glyph::Daylight, "%s", l);
  }
}

// ---- «Погода» ---------------------------------------------------------------------------------------------------

// Сводка «сейчас»: иконка, температура, описание, «ощущается», мин/макс. Возвращает высоту.
int drawCurrent(GfxRenderer& r, int x, int y, int w, const Ctx& c, const WxView& v, bool land) {
  const auto& WL = weather_core::labels(c.lang);
  const int iconS = land ? 48 : 58;
  const auto& cur = v.cur;
  const bool ok = v.haveCur && !std::isnan(cur.temp);
  const Icon icon = (ok && cur.code >= 0) ? weather_core::iconFor(cur.code, cur.isDay) : Icon::Unknown;
  drawWeatherIcon(r, icon, x, y + 6, iconS);
  const int tx = x + iconS + 12;
  const int tw = drawTemperature(r, tx, y + 6 + (iconS - kTempDigitH) / 2, ok ? cur.temp : NAN);
  const int textX = tx + tw + 14;
  const char* desc = (ok && cur.code >= 0) ? weather_core::description(c.lang, cur.code) : "";
  if (!desc[0]) desc = WL.noData;
  const int rangeW = land ? 0 : 92;
  const Fit d(r, kFontText, desc, x + w - textX - rangeW, kBold);
  char fd[16], feels[40];
  fmtDeg(fd, sizeof(fd), ok ? cur.feels : NAN);
  std::snprintf(feels, sizeof(feels), "%s %s", WL.feels, fd);
  const int blockH = lineH(r, kFontText) + lineH(r, kFontSmall);
  const int ty = y + 6 + (iconS - blockH) / 2;
  r.drawText(kFontText, textX, ty, d.c_str(), true, kBold);
  r.drawText(kFontSmall, textX, ty + lineH(r, kFontText), feels, true);
  char a[16], b[16], ra[40], rb[40];
  fmtDeg(a, sizeof(a), ok ? cur.tMin : NAN);
  fmtDeg(b, sizeof(b), ok ? cur.tMax : NAN);
  std::snprintf(ra, sizeof(ra), "%s", a);
  std::snprintf(rb, sizeof(rb), "%s", b);
  if (!land) {
    const int rowH = glyphSize(r, kFontSmall) + 2;
    drawGlyphText(r, kFontSmall, Glyph::TempMin, x + w - glyphTextW(r, kFontSmall, Glyph::TempMin, ra, kBold), ty - 2, ra, kBold);
    drawGlyphText(r, kFontSmall, Glyph::TempMax, x + w - glyphTextW(r, kFontSmall, Glyph::TempMax, rb, kBold), ty - 2 + rowH, rb, kBold);
  }
  return iconS + 12;
}

// График 24 часов: температура линией, вероятность осадков столбиками, ночь затенена растром.
void drawHourGraph(GfxRenderer& r, int x, int y, int w, int h, const weather_core::FcHour* hs, int n, int offSec) {
  const int lhS = lineH(r, kFontSmall);
  const int labelsH = lhS + 2, barsH = 26;
  const int chartTop = y + lhS + 8;
  const int barsBottom = y + h - labelsH - 2;
  const int chartBot = barsBottom - barsH - 6;
  if (n < 2 || chartBot <= chartTop + 10) return;
  const int step = (w - 24) / (n - 1);
  auto xi = [&](int i) { return x + 12 + i * step; };
  float mn = 1e9f, mx = -1e9f;
  for (int i = 0; i < n; ++i) {
    if (std::isnan(hs[i].temp)) continue;
    mn = std::min(mn, hs[i].temp);
    mx = std::max(mx, hs[i].temp);
  }
  if (mn > mx) return;
  mn -= 1.5f;
  mx += 1.5f;
  auto yi = [&](float t) { return chartTop + static_cast<int>(std::lround((mx - t) / (mx - mn) * (chartBot - chartTop))); };

  // Ночь.
  for (int i = 0; i < n;) {
    if (!hs[i].isDay) {
      int j = i;
      while (j + 1 < n && !hs[j + 1].isDay) ++j;
      const int x0 = std::max(x, xi(i) - step / 2), x1 = std::min(x + w, xi(j) + step / 2);
      r.fillRectDither(x0, y, x1 - x0, h - labelsH, Color::LightGray);
      i = j + 1;
    } else {
      ++i;
    }
  }
  // Осадки + ось.
  r.drawLine(x + 6, barsBottom, x + w - 6, barsBottom, 2, true);
  const int bw = std::max(3, step * 6 / 10);
  for (int i = 0; i < n; ++i) {
    // Высота — вероятность осадков (Open-Meteo) или, если её нет, их количество: 4 мм за час и больше — во всю высоту
    // (MET Norway).
    int bh = 0;
    if (hs[i].prob > 0) {
      bh = hs[i].prob * barsH / 100;
    } else if (hs[i].prob < 0 && hs[i].mm > 0) {
      bh = std::max(2, static_cast<int>(std::min(hs[i].mm, 4.0f) * barsH / 4));
    }
    if (bh > 0) r.fillRect(xi(i) - bw / 2, barsBottom - bh, bw, bh, true);
  }
  // Линия температуры.
  int px = 0, py = 0;
  bool have = false;
  for (int i = 0; i < n; ++i) {
    if (std::isnan(hs[i].temp)) {
      have = false;
      continue;
    }
    const int cx = xi(i), cy = yi(hs[i].temp);
    if (have) r.drawLine(px, py, cx, cy, 3, true);
    px = cx;
    py = cy;
    have = true;
  }
  // Точки и подписи каждые 3 часа.
  for (int i = 0; i < n; i += 3) {
    if (!std::isnan(hs[i].temp)) {
      disc(r, xi(i), yi(hs[i].temp), 5, Color::Black);
      char t[16];
      fmtDeg(t, sizeof(t), hs[i].temp);
      const int tw = textW(r, kFontSmall, t, kBold);
      r.drawText(kFontSmall, std::clamp(xi(i) - tw / 2, x, x + w - tw), yi(hs[i].temp) - lhS - 6, t, true, kBold);
    }
    char hh[8];
    std::snprintf(hh, sizeof(hh), "%02d", localHour(hs[i].ts, offSec));
    r.drawText(kFontSmall, xi(i) - textW(r, kFontSmall, hh) / 2, y + h - labelsH, hh, true);
  }
}

// Шапка почасовой таблицы значками над колонками (как в «7 днях»): t° — температура, капля — осадки (% или мм), ветер.
// Координаты колонок — те же, что в drawHourTable. Возвращает высоту.
int drawHourTableHeader(GfxRenderer& r, int x, int y, int w) {
  const int nf = kFontSmall;
  r.drawText(nf, x + w * 38 / 100, y, "t\xC2\xB0", true, kBold);
  auto rightAt = [&](Glyph g, int right) { return right - (glyphTextW(r, nf, g, "") - 4); };
  drawGlyphText(r, nf, Glyph::Drop, rightAt(Glyph::Drop, x + w - 96), y, "");
  drawGlyphText(r, nf, Glyph::Wind, rightAt(Glyph::Wind, x + w - 8), y, "");
  const int h = std::max(glyphSize(r, nf), lineH(r, nf)) + 4;
  r.drawLine(x, y + h - 2, x + w, y + h - 2, 1, true);
  return h + 2;
}

// Таблица каждые 3 часа. Первая строка — ближайший час, в рамке.
void drawHourTable(GfxRenderer& r, int x, int y, int w, int rowH, const weather_core::FcHour* hs, int n, int offSec,
                   const Ctx& c) {
  const auto& WL = weather_core::labels(c.lang);
  const int lhS = lineH(r, kFontSmall);
  const int nf = rowH >= lineH(r, kFontText) + 2 ? kFontText : kFontSmall;  // в тесной (ландшафт) таблице — мельче
  const int lhT = lineH(r, nf);
  const int iconS = std::min(30, rowH - 4);
  for (int k = 0, i = 0; i < n && k < 8; i += 3, ++k) {
    const int ry = y + k * rowH;
    if (k > 0) r.drawLine(x, ry, x + w, ry, 1, true);
    if (k == 0) r.drawRoundedRect(x, ry, w, rowH, 2, 8, true);
    char t[12], tp[16], pr[20], wd[24];
    std::snprintf(t, sizeof(t), "%02d:00", localHour(hs[i].ts, offSec));
    fmtDeg(tp, sizeof(tp), hs[i].temp);
    if (hs[i].prob >= 0) {
      std::snprintf(pr, sizeof(pr), "%d %%", hs[i].prob);
    } else if (!std::isnan(hs[i].mm)) {  // MET Norway: вероятности нет — количество за час
      char mm[12];
      fmtMm(mm, sizeof(mm), hs[i].mm, c.lang);
      std::snprintf(pr, sizeof(pr), "%s %s", mm, WL.mmUnit);
    } else {
      std::snprintf(pr, sizeof(pr), "%s", kDash);
    }
    if (std::isnan(hs[i].wind)) std::snprintf(wd, sizeof(wd), "%s", kDash);
    else std::snprintf(wd, sizeof(wd), "%ld %s", std::lround(hs[i].wind), WL.windUnit);
    r.drawText(nf, x + 8, ry + (rowH - lhT) / 2, t, true, kBold);
    const Icon ic = hs[i].code >= 0 ? weather_core::iconFor(hs[i].code, hs[i].isDay) : Icon::Unknown;
    drawWeatherIcon(r, ic, x + w * 24 / 100, ry + (rowH - iconS) / 2, iconS);
    r.drawText(nf, x + w * 38 / 100, ry + (rowH - lhT) / 2, tp, true, kBold);
    r.drawText(kFontSmall, x + w - 96 - textW(r, kFontSmall, pr), ry + (rowH - lhS) / 2, pr, true);
    r.drawText(kFontSmall, x + w - 8 - textW(r, kFontSmall, wd), ry + (rowH - lhS) / 2, wd, true);
  }
}

void drawNoForecast(GfxRenderer& r, int x, int y, int w, const Ctx& c, const Txt& T) {
  drawWeatherIcon(r, Icon::Unknown, x + (w - 64) / 2, y + 30, 64);
  drawCentered(r, kFontText, x, w, y + 106, weather_core::labels(c.lang).noData, true, kBold);
  const Fit h(r, kFontSmall, T.noFcHint, w - 16);
  drawCentered(r, kFontSmall, x, w, y + 106 + lineH(r, kFontText) + 6, h.c_str());
}

void drawWeatherToday(GfxRenderer& r, const Frame& f, const Ctx& c, const Txt& T, const WxView& v, int y) {
  const int pad = calendar_config::kSidePad;
  const auto& fc = c.wx.fc;
  int i0 = 0;
  while (v.fcOk && i0 < fc.nHours && fc.h[i0].ts + 3600u <= c.t.epoch) ++i0;
  const int n = v.fcOk ? fc.nHours - i0 : 0;
  const weather_core::FcHour* hs = &fc.h[i0];

  if (!f.land) {
    const int w = f.w - 2 * pad, x = f.x + pad;
    y += drawCurrent(r, x, y, w, c, v, false);
    RichItems it;
    sunItems(it, c, c.t.year, c.t.month, c.t.day, true);
    y += drawRichRows(r, kFontSmall, x, w, y, it, kBold) + 4;
    if (n < 2) {
      drawNoForecast(r, x, y, w, c, T);
      return;
    }
    const int graphH = 170;
    drawHourGraph(r, x, y, w, graphH, hs, n, fc.utcOffsetSec);
    y += graphH + 4;
    y += drawHourTableHeader(r, x, y, w);
    const int foot = kChipH + 6;  // подвал: кнопка «Обновить», время обновления, соседняя страница
    const int rowH = std::clamp((f.bottom - foot - y) / 8, 28, 40);
    drawHourTable(r, x, y, w, rowH, hs, n, fc.utcOffsetSec, c);
    return;
  }
  // Ландшафт: слева сводка и солнце, справа график и таблица.
  const int leftW = 330 * f.w / 800, gap = 22;
  const int lx = f.x + pad, rx = lx + leftW + gap, rw = f.w - 2 * pad - leftW - gap;
  int ly = y + drawCurrent(r, lx, y, leftW, c, v, true);
  char a[16], b[16];
  fmtDeg(a, sizeof(a), v.haveCur ? v.cur.tMin : NAN);
  fmtDeg(b, sizeof(b), v.haveCur ? v.cur.tMax : NAN);
  RichItems it;
  it.add(Glyph::TempMin, "%s", a);
  it.add(Glyph::TempMax, "%s", b);
  ly += drawRichRows(r, kFontSmall, lx, leftW, ly, it, kBold, false) + 6;
  RichItems sun;
  sunItems(sun, c, c.t.year, c.t.month, c.t.day, true);
  ly += drawRichRows(r, kFontSmall, lx, leftW, ly, sun, kBold) + 6;
  if (n < 2) {
    drawNoForecast(r, rx, y, rw, c, T);
    return;
  }
  const int graphH = 118;
  drawHourGraph(r, rx, y, rw, graphH, hs, n, fc.utcOffsetSec);
  int ty = y + graphH + 4;
  ty += drawHourTableHeader(r, rx, ty, rw);
  const int foot = lineH(r, kFontSmall) + 6;  // справа внизу — только переход на «7 дней»; кнопка «Обновить» — слева
  const int rowH = std::clamp((f.bottom - foot - ty) / 8, 22, 30);
  drawHourTable(r, rx, ty, rw, rowH, hs, n, fc.utcOffsetSec, c);
}

void drawWeatherWeek(GfxRenderer& r, const Frame& f, const Ctx& c, const Txt& T, const WxView& v, int y) {
  const int pad = calendar_config::kSidePad;
  const int x = f.x + pad, w = f.w - 2 * pad;
  const auto& fc = c.wx.fc;
  if (!v.fcOk || fc.nDays == 0) {
    drawNoForecast(r, x, y, w, c, T);
    return;
  }
  const auto& WL = weather_core::labels(c.lang);
  float gmin = 1e9f, gmax = -1e9f;
  for (int i = 0; i < fc.nDays; ++i) {
    if (!std::isnan(fc.d[i].tMin)) gmin = std::min(gmin, fc.d[i].tMin);
    if (!std::isnan(fc.d[i].tMax)) gmax = std::max(gmax, fc.d[i].tMax);
  }
  if (gmin > gmax) {
    gmin = 0;
    gmax = 1;
  }
  if (gmax - gmin < 1.f) gmax = gmin + 1.f;
  const int lhT = lineH(r, kFontText), lhS = lineH(r, kFontSmall);
  // Шапка колонок значками: t↓ (минимум дня), t↑ (максимум), капля (осадки), в ландшафте ещё ветер. Полоса между t↓ и t↑ —
  // шкала недели; чем правее конец чёрной части, тем теплее день, поэтому отдельной подписи ей не нужно.
  const int hdrH = glyphSize(r, kFontSmall) + 6;
  const int foot = kChipH + 6;  // подвал с кнопкой «Обновить»
  (void)lhS;
  const int rowH = std::clamp((f.bottom - foot - (y + hdrH)) / fc.nDays, f.land ? 34 : 44, 82);
  const int col1 = f.land ? 150 : 110, col4 = f.land ? 190 : 96, colW = f.land ? 100 : 0;
  {
    const int iconH = std::min(44, rowH - 6);
    const int hx0 = x + col1 + iconH + 10, hx1 = x + w - col4 - colW - 8;
    const int px = x + w - colW - 4;
    auto rightAt = [&](Glyph g, int right) { return right - (glyphTextW(r, kFontSmall, g, "") - 4); };
    drawGlyphText(r, kFontSmall, Glyph::TempMin, hx0, y, "");
    drawGlyphText(r, kFontSmall, Glyph::TempMax, rightAt(Glyph::TempMax, hx1), y, "");
    drawGlyphText(r, kFontSmall, Glyph::Drop, rightAt(Glyph::Drop, px), y, "");
    if (f.land) drawGlyphText(r, kFontSmall, Glyph::Wind, rightAt(Glyph::Wind, x + w - 4), y, "");
    y += hdrH;
    r.drawLine(x, y - 3, x + w, y - 3, 1, true);
  }
  for (int i = 0; i < fc.nDays; ++i) {
    const auto& d = fc.d[i];
    const int ry = y + i * rowH;
    const Ymd dt = fromEpoch(d.ts, fc.utcOffsetSec);
    const bool isToday = dt.y == c.t.year && dt.m == c.t.month && dt.d == c.t.day;
    const auto di = dayInfo(c, dt.y, dt.m, dt.d);
    if (i > 0) r.drawLine(x, ry, x + w, ry, 1, true);
    if (isToday) r.fillRectDither(x, ry + 1, w, rowH - 1, Color::LightGray);
    else if (di.off) r.fillRectDither(x, ry + 1, w, rowH - 1, Color::LightGray);
    const unsigned wd = calendar_core::weekday(dt.y, dt.m, dt.d);
    char l1[32], l2[32];
    if (isToday) std::snprintf(l1, sizeof(l1), "%s", T.today);
    else std::snprintf(l1, sizeof(l1), "%s %u", calendar_core::weekdayShort(c.lang, wd), dt.d);
    if (isToday) std::snprintf(l2, sizeof(l2), "%s %u", calendar_core::weekdayShort(c.lang, wd), dt.d);
    else std::snprintf(l2, sizeof(l2), "%s", calendar_core::monthNameGenitive(c.lang, dt.m));
    const bool tight = rowH < lhT + lhS + 4;  // ландшафт: в строке нет места на две строки — дата в одну
    if (tight) {
      char one[48];
      std::snprintf(one, sizeof(one), "%s", l1);
      if (!isToday) {
        char ab[16];
        abbrTo(ab, sizeof(ab), l2, 3);
        std::snprintf(one, sizeof(one), "%s %s", l1, ab);
      }
      r.drawText(kFontText, x + 6, ry + (rowH - lhT) / 2, one, true, kBold);
    } else {
      const int ty = ry + (rowH - lhT - lhS) / 2;
      r.drawText(kFontText, x + 6, ty, l1, true, kBold);
      r.drawText(kFontSmall, x + 6, ty + lhT, l2, true);
    }
    const Icon ic = d.code >= 0 ? weather_core::iconFor(d.code, true) : Icon::Unknown;
    const int iconS = std::min(44, rowH - 6);
    drawWeatherIcon(r, ic, x + col1, ry + (rowH - iconS) / 2, iconS);

    // Диапазон температур полосой на общей шкале недели.
    const int rx0 = x + col1 + iconS + 10, rx1 = x + w - col4 - colW - 8;
    char mn[16], mx[16];
    fmtDeg(mn, sizeof(mn), d.tMin);
    fmtDeg(mx, sizeof(mx), d.tMax);
    const int mnW = textW(r, kFontText, mn, kBold), mxW = textW(r, kFontText, mx, kBold);
    r.drawText(kFontText, rx0, ry + (rowH - lhT) / 2, mn, true, kBold);
    r.drawText(kFontText, rx1 - mxW, ry + (rowH - lhT) / 2, mx, true, kBold);
    const int tx0 = rx0 + mnW + 8, tx1 = rx1 - mxW - 8, trkY = ry + rowH / 2 - 5;
    if (tx1 - tx0 > 20) {
      r.drawRoundedRect(tx0, trkY, tx1 - tx0, 10, 2, 5, true);
      if (!std::isnan(d.tMin) && !std::isnan(d.tMax)) {
        const int a = tx0 + static_cast<int>((d.tMin - gmin) / (gmax - gmin) * (tx1 - tx0));
        const int b = tx0 + static_cast<int>((d.tMax - gmin) / (gmax - gmin) * (tx1 - tx0));
        r.fillRoundedRect(a, trkY, std::max(8, b - a), 10, 5, Color::Black);
      }
    }
    // Осадки (мм и %), в ландшафте ещё ветер.
    char mm[16], p1[32], p2[24];
    fmtMm(mm, sizeof(mm), d.precipMm, c.lang);
    std::snprintf(p1, sizeof(p1), "%s %s", mm, WL.mmUnit);
    if (d.prob >= 0) std::snprintf(p2, sizeof(p2), "%d %%", d.prob); else std::snprintf(p2, sizeof(p2), "%s", kDash);
    const int px = x + w - colW - 4;
    // MET Norway вероятности осадков для России не даёт — только миллиметры, одной строкой.
    const bool mmOnly = d.prob < 0 && fc.provider == weather_core::Provider::MetNo;
    if (tight || mmOnly) {
      char both[56];
      if (mmOnly) {
        std::snprintf(both, sizeof(both), "%s", p1);
      } else {
        std::snprintf(both, sizeof(both), "%s  \xC2\xB7  %s", p1, p2);
      }
      r.drawText(kFontSmall, px - textW(r, kFontSmall, both), ry + (rowH - lhS) / 2, both, true);
    } else {
      r.drawText(kFontSmall, px - textW(r, kFontSmall, p1), ry + (rowH - 2 * lhS - 2) / 2, p1, true);
      r.drawText(kFontSmall, px - textW(r, kFontSmall, p2), ry + (rowH - 2 * lhS - 2) / 2 + lhS + 2, p2, true);
    }
    if (f.land) {
      char wd2[24];
      if (std::isnan(d.windMax)) std::snprintf(wd2, sizeof(wd2), "%s", kDash);
      else std::snprintf(wd2, sizeof(wd2), "%ld %s", std::lround(d.windMax), WL.windUnit);
      r.drawText(kFontSmall, x + w - 4 - textW(r, kFontSmall, wd2), ry + (rowH - lhS) / 2, wd2, true);
    }
  }
}

void drawWeather(GfxRenderer& r, const Frame& f, const State& s, const Ctx& c, HitMap& hit) {
  const Txt& T = txt(c.lang);
  const WxView v = wxView(c);
  const char* title = c.wx.place.city[0] ? c.wx.place.city : weather_core::labels(c.lang).unknownPlace;
  int y = drawHeader(r, f, T, title, Right::Dots, s.page, hit, Act::OpenPlace, !c.place.autoLocation);
  if (s.page == 0) drawWeatherToday(r, f, c, T, v, y); else drawWeatherWeek(r, f, c, T, v, y);

  // Подвал: кнопка «Обновить», время обновления и источник, подсказка соседней страницы. В ландшафте на странице «сегодня»
  // кнопка и время — под левой колонкой (справа до низа идёт таблица), иначе — во всю ширину.
  char upd[96];
  fmtUpdatedLine(upd, sizeof(upd), c, v, c.lang);
  const int pad = calendar_config::kSidePad;
  const int lhS = lineH(r, kFontSmall);
  const char* hint = s.page == 0 ? T.sevenDays : T.hourly;
  char hintFull[40];
  std::snprintf(hintFull, sizeof(hintFull), s.page == 0 ? "%s >" : "< %s", hint);
  const int hw = textW(r, kFontSmall, hintFull);
  const bool splitLand = f.land && s.page == 0;
  const int areaW = splitLand ? 330 * f.w / 800 : f.w - 2 * pad - hw - 8;
  const int fy = f.bottom - kChipH - 2;
  const int bx = f.x + pad;
  const int bw = chip(r, bx, fy, kChipH, c.wxRefreshing ? T.refreshing : T.refresh, c.wxRefreshing);
  hit.add(bx - 4, fy - 3, bw + 8, kChipH + 6, Act::Refresh);
  const Fit u(r, kFontSmall, upd, areaW - bw - 8);
  r.drawText(kFontSmall, bx + bw + 8, fy + (kChipH - lhS) / 2, u.c_str(), true);
  const int hy = splitLand ? f.bottom - lhS - 2 : fy + (kChipH - lhS) / 2;
  r.drawText(kFontSmall, f.x + f.w - pad - hw, hy, hintFull, true, kBold);
}

// ---- «День» -----------------------------------------------------------------------------------------------------

// Карточка: рамка вокруг уже нарисованного содержимого. Заголовок карточки — верхней строкой.
struct Card {
  int x, y, w;
  int cy;  // текущая строка содержимого
};
Card beginCard(GfxRenderer& r, int x, int y, int w, const char* caption) {
  Card k{x, y, w, y + 6};
  const Fit cap(r, kFontSmall, caption, w - 24, kBold);  // длинная подпись места не вылезает за рамку
  r.drawText(kFontSmall, x + 12, k.cy, cap.c_str(), true, kBold);
  k.cy += lineH(r, kFontSmall) + 2;
  return k;
}
int endCard(GfxRenderer& r, Card& k) {
  const int h = k.cy - k.y + 6;
  r.drawRoundedRect(k.x, k.y, k.w, h, 2, 12, true);
  return h + 8;
}

// Длина дня (минуты) раз в 5 суток за год — для мини-графика на экране «День».
struct SunSeries {
  static constexpr int kPts = 74;
  int year = 0;
  double lat = 0, lon = 0;
  int offMin = 0;
  bool valid = false;
  uint16_t v[kPts] = {};
  uint16_t lo = 0, hi = 0;
};

const SunSeries& sunSeries(const Ctx& c, int year) {
  static SunSeries cache;  // 160 байт статики вместо пересчёта на каждый кадр
  const double lat = c.wx.place.lat, lon = c.wx.place.lon;
  if (cache.valid && cache.year == year && cache.lat == lat && cache.lon == lon && cache.offMin == c.t.utcOffsetMin) return cache;
  cache.year = year;
  cache.lat = lat;
  cache.lon = lon;
  cache.offMin = c.t.utcOffsetMin;
  cache.lo = 1440;
  cache.hi = 0;
  const int32_t jan1 = calendar_core::daysFromCivil(year, 1, 1);
  const int ndays = static_cast<int>(calendar_core::daysInYear(year));
  for (int i = 0; i < SunSeries::kPts; ++i) {
    int yy;
    unsigned mm, dd;
    calendar_core::civilFromDays(jan1 + std::min(i * 5, ndays - 1), yy, mm, dd);
    const auto q = sun_times::compute(yy, mm, dd, lat, lon, c.t.utcOffsetMin);
    cache.v[i] = static_cast<uint16_t>(q.valid ? q.daylightMin : 0);
    cache.lo = std::min(cache.lo, cache.v[i]);
    cache.hi = std::max(cache.hi, cache.v[i]);
  }
  cache.valid = true;
  return cache;
}

int drawSunCard(GfxRenderer& r, int x, int y, int w, const Ctx& c, const Txt& T, const State& s, bool withCurve) {
  const auto& CL = calendar_core::labels(c.lang);
  char cap[80];
  std::snprintf(cap, sizeof(cap), "%s  \xC2\xB7  %s", T.sun, c.wx.place.city[0] ? c.wx.place.city : "");
  Card k = beginCard(r, x, y, w, cap);
  const auto sun = sun_times::compute(s.dayY, s.dayM, s.dayD, c.wx.place.lat, c.wx.place.lon, c.t.utcOffsetMin);
  char l1[64], l2[64];
  RichItems l1Items, l2Items;
  if (!sun.valid || sun.polarDay || sun.polarNight) {
    l1Items.add(Glyph::None, "%s", !sun.valid ? kDash : sun.polarDay ? CL.polarDay : CL.polarNight);
  } else {
    std::snprintf(l1, sizeof(l1), "%02d:%02d", sun.sunriseMin / 60, sun.sunriseMin % 60);
    l1Items.add(Glyph::Sunrise, "%s", l1);
    std::snprintf(l1, sizeof(l1), "%02d:%02d", sun.sunsetMin / 60, sun.sunsetMin % 60);
    l1Items.add(Glyph::Sunset, "%s", l1);
    // Разница с предыдущим днём.
    int py;
    unsigned pm, pd;
    calendar_core::civilFromDays(calendar_core::daysFromCivil(s.dayY, s.dayM, s.dayD) - 1, py, pm, pd);
    const auto prev = sun_times::compute(py, pm, pd, c.wx.place.lat, c.wx.place.lon, c.t.utcOffsetMin);
    std::snprintf(l2, sizeof(l2), "%d %s %02d %s", sun.daylightMin / 60, CL.hoursShort, sun.daylightMin % 60, CL.minutesShort);
    l2Items.add(Glyph::Daylight, "%s", l2);
    if (prev.valid && !prev.polarDay && !prev.polarNight) {
      const int dm = sun.daylightMin - prev.daylightMin;
      std::snprintf(l2, sizeof(l2), T.vsYest, dm >= 0 ? '+' : '-', std::abs(dm));
      l2Items.add(Glyph::None, "%s", l2);
    }
  }
  k.cy += drawRichRows(r, kFontText, x, w, k.cy, l1Items, kBold) + 2;
  if (l2Items.n) k.cy += drawRichRows(r, kFontSmall, x, w, k.cy, l2Items) + 4;
  if (!withCurve) return endCard(r, k);
  // Длина дня за год: кривая с точкой на выбранной дате. 74 расчёта восхода/заката (двойная точность без FPU —
  // заметные миллисекунды) кэшируются: пересчёт только при смене года, места или пояса, а не на каждом листании дня.
  const int gh = 40, gw = w - 24, gx = x + 12, gy = k.cy;
  const SunSeries& ss = sunSeries(c, s.dayY);
  if (ss.hi > ss.lo) {
    auto px = [&](int i) { return gx + i * gw / (SunSeries::kPts - 1); };
    auto py2 = [&](uint16_t v) { return gy + gh - 4 - static_cast<int>((v - ss.lo) * (gh - 8) / (ss.hi - ss.lo)); };
    for (int i = 1; i < SunSeries::kPts; ++i) r.drawLine(px(i - 1), py2(ss.v[i - 1]), px(i), py2(ss.v[i]), 2, true);
    const int di = std::min(SunSeries::kPts - 1, static_cast<int>(calendar_core::dayOfYear(s.dayY, s.dayM, s.dayD) - 1) / 5);
    disc(r, px(di), py2(ss.v[di]), 5, Color::Black);
  }
  k.cy += gh + 2;
  return endCard(r, k);
}

int drawMoonCard(GfxRenderer& r, int x, int y, int w, const Ctx& c, const Txt& T, const State& s) {
  Card k = beginCard(r, x, y, w, T.moon);
  const double jd = moon_phase::julianFromLocal(s.dayY, s.dayM, s.dayD, 12, 0, c.t.utcOffsetMin);
  const auto mi = moon_phase::at(jd);
  const auto nf = moon_phase::nextFullMoon(jd, c.t.utcOffsetMin);
  const auto nn = moon_phase::nextNewMoon(jd, c.t.utcOffsetMin);
  const int rad = 27;
  drawMoonPhase(r, mi.fraction, x + 16 + rad, k.cy + rad + 2, rad);
  const int tx = x + 16 + 2 * rad + 14, tw = w - (tx - x) - 10;
  char l2[64], l3[64];
  std::snprintf(l2, sizeof(l2), T.lit, static_cast<int>(std::lround(mi.illum * 100)));
  std::snprintf(l3, sizeof(l3), T.lunarDay, mi.lunarDay);
  const Fit name(r, kFontText, moon_phase::phaseName(c.lang, mi.phaseIdx), tw, kBold);
  int ty = k.cy + 2;
  r.drawText(kFontText, tx, ty, name.c_str(), true, kBold);
  ty += lineH(r, kFontText);
  r.drawText(kFontSmall, tx, ty, l2, true);
  ty += lineH(r, kFontSmall);
  r.drawText(kFontSmall, tx, ty, l3, true);
  k.cy += std::max(2 * rad + 8, ty + lineH(r, kFontSmall) - k.cy + 4);
  char e1[48], e2[48], both[120], mo1[16], mo2[16];
  abbrTo(mo1, sizeof(mo1), calendar_core::monthNameGenitive(c.lang, nf.month), 3);
  abbrTo(mo2, sizeof(mo2), calendar_core::monthNameGenitive(c.lang, nn.month), 3);
  std::snprintf(e1, sizeof(e1), "%s %u %s", T.nextFull, nf.day, mo1);
  std::snprintf(e2, sizeof(e2), "%s %u %s", T.nextNew, nn.day, mo2);
  std::snprintf(both, sizeof(both), "%s  \xC2\xB7  %s", e1, e2);
  if (textW(r, kFontSmall, both) <= w - 16) {
    drawCentered(r, kFontSmall, x, w, k.cy, both);
    k.cy += lineH(r, kFontSmall) + 4;
  } else {  // узкая колонка: два события — две строки
    drawCentered(r, kFontSmall, x, w, k.cy, e1);
    drawCentered(r, kFontSmall, x, w, k.cy + lineH(r, kFontSmall), e2);
    k.cy += 2 * lineH(r, kFontSmall) + 4;
  }
  return endCard(r, k);
}

int drawWeatherCard(GfxRenderer& r, int x, int y, int w, const Ctx& c, const Txt& T, const State& s, HitMap& hit) {
  const WxView v = wxView(c);
  const int32_t target = calendar_core::daysFromCivil(s.dayY, s.dayM, s.dayD);
  const int32_t today = calendar_core::daysFromCivil(c.t.year, c.t.month, c.t.day);
  const bool past = target < today;
  const weather_core::HistDay* h = past ? c.hist.find(packDate(s.dayY, s.dayM, s.dayD), c.wx.place.lat, c.wx.place.lon) : nullptr;
  // Прошедший день — в заголовке карточки источник: «Архив Open-Meteo» или «Сохранённый прогноз» (записан устройством).
  const char* head = !h ? T.weather : h->src == weather_core::HistSource::Archive ? T.histArchive : T.histRecorded;
  char cap[96];
  std::snprintf(cap, sizeof(cap), "%s  \xC2\xB7  %s", head, c.wx.place.city[0] ? c.wx.place.city : "");
  Card k = beginCard(r, x, y, w, cap);
  const auto& fc = c.wx.fc;
  int idx = -1;
  if (v.fcOk) {
    for (int i = 0; i < fc.nDays; ++i) {
      const Ymd dt = fromEpoch(fc.d[i].ts, fc.utcOffsetSec);
      if (calendar_core::daysFromCivil(dt.y, dt.m, dt.d) == target) idx = i;
    }
  }
  if (past) {  // прошедший день — история: архив Open-Meteo или сохранённый устройством прогноз
    if (!h) {
      const char* msg = c.histState == HistState::Waiting  ? T.histLoading
                        : c.histState == HistState::Failed ? T.histFailed
                                                           : T.histNone;
      const Fit t(r, kFontSmall, msg, w - 16);
      drawCentered(r, kFontSmall, x, w, k.cy + 4, t.c_str());
      k.cy += lineH(r, kFontSmall) + 10;
      return endCard(r, k);
    }
    const auto& WL = weather_core::labels(c.lang);
    const int iconS = 46;
    drawWeatherIcon(r, h->code >= 0 ? weather_core::iconFor(h->code, true) : Icon::Unknown, x + 12, k.cy + 2, iconS);
    const int tx = x + 12 + iconS + 12, tw = w - (tx - x) - 10;
    const char* desc = h->code >= 0 ? weather_core::description(c.lang, h->code) : WL.noData;
    const Fit dn(r, kFontText, desc, tw, kBold);
    r.drawText(kFontText, tx, k.cy + 2, dn.c_str(), true, kBold);
    char a[16], b[16], mm[16], pr[24];
    fmtDeg(a, sizeof(a), h->tMin);
    fmtDeg(b, sizeof(b), h->tMax);
    fmtMm(mm, sizeof(mm), h->mm, c.lang);
    std::snprintf(pr, sizeof(pr), "%s %s", mm, WL.mmUnit);
    RichItems ri;  // та же строка, что и у прогноза дня: t↓ t↑ осадки
    ri.add(Glyph::TempMin, "%s", a);
    ri.add(Glyph::TempMax, "%s", b);
    ri.add(Glyph::Drop, "%s", pr);
    drawRichRows(r, kFontSmall, tx, tw, k.cy + 2 + lineH(r, kFontText), ri, kRegular, false);
    k.cy += std::max(iconS + 8, lineH(r, kFontText) + glyphSize(r, kFontSmall) + 8);
    return endCard(r, k);
  }
  if (idx < 0) {
    const Fit t(r, kFontSmall, T.fcOnly, w - 16);
    drawCentered(r, kFontSmall, x, w, k.cy + 4, t.c_str());
    k.cy += lineH(r, kFontSmall) + 10;
    return endCard(r, k);
  }
  const auto& d = fc.d[idx];
  const auto& WL = weather_core::labels(c.lang);
  const int iconS = 46;
  drawWeatherIcon(r, d.code >= 0 ? weather_core::iconFor(d.code, true) : Icon::Unknown, x + 12, k.cy + 2, iconS);
  const int tx = x + 12 + iconS + 12, tw = w - (tx - x) - 10;
  const char* desc = d.code >= 0 ? weather_core::description(c.lang, d.code) : WL.noData;
  const Fit dn(r, kFontText, desc, tw, kBold);
  r.drawText(kFontText, tx, k.cy + 2, dn.c_str(), true, kBold);
  char a[16], b[16], mm[16], pr[24];
  fmtDeg(a, sizeof(a), d.tMin);
  fmtDeg(b, sizeof(b), d.tMax);
  fmtMm(mm, sizeof(mm), d.precipMm, c.lang);
  std::snprintf(pr, sizeof(pr), "%s %s", mm, WL.mmUnit);
  RichItems ri;
  ri.add(Glyph::TempMin, "%s", a);
  ri.add(Glyph::TempMax, "%s", b);
  ri.add(Glyph::Drop, "%s", pr);
  drawRichRows(r, kFontSmall, tx, tw, k.cy + 2 + lineH(r, kFontText), ri, kRegular, false);
  k.cy += std::max(iconS + 8, lineH(r, kFontText) + glyphSize(r, kFontSmall) + 8);
  const int ch = endCard(r, k);
  hit.add(x, y, w, ch - 8, Act::OpenWeek);
  return ch;
}

// Фиксированные «слоты» заголовка дня — чтобы ничего не «прыгало» при смене даты: день недели, дата, «через N дн.»
// (всегда своей строкой под датой), чипсы, строка праздника (высота зарезервирована, даже когда пустая).
int drawDayTitle(GfxRenderer& r, int x, int y, int w, const Ctx& c, const Txt& T, const State& s) {
  const int lhT = lineH(r, kFontText), lhS = lineH(r, kFontSmall);
  drawCentered(r, kFontText, x, w, y, calendar_core::weekdayName(c.lang, calendar_core::weekday(s.dayY, s.dayM, s.dayD)), true, kBold);
  y += lhT + 2;
  char date[48];
  calendar_core::formatLongDate(c.lang, s.dayY, s.dayM, s.dayD, date, sizeof(date));
  drawCentered(r, kFontText, x, w, y, date);
  y += lhT + 6;

  const int diff = static_cast<int>(calendar_core::daysFromCivil(s.dayY, s.dayM, s.dayD) -
                                    calendar_core::daysFromCivil(c.t.year, c.t.month, c.t.day));
  char rel[48];
  if (diff == 0) std::snprintf(rel, sizeof(rel), "%s", T.todayLc);
  else if (diff == 1) std::snprintf(rel, sizeof(rel), "%s", T.tomorrow);
  else if (diff == -1) std::snprintf(rel, sizeof(rel), "%s", T.yesterday);
  else if (diff > 0) std::snprintf(rel, sizeof(rel), T.inDays, diff);
  else std::snprintf(rel, sizeof(rel), T.daysAgo, -diff);
  const int pillH = lhS + 8, pw = textW(r, kFontSmall, rel, kBold) + 28;
  r.drawRoundedRect(x + (w - pw) / 2, y, pw, pillH, 2, pillH / 2, true);
  r.drawText(kFontSmall, x + (w - pw) / 2 + 14, y + 4, rel, true, kBold);
  y += pillH + 8;

  const auto& CL = calendar_core::labels(c.lang);
  char chips[96];
  const unsigned diy = calendar_core::daysInYear(s.dayY), doy = calendar_core::dayOfYear(s.dayY, s.dayM, s.dayD);
  std::snprintf(chips, sizeof(chips), "%s %u  \xC2\xB7  %s %u / %u", CL.week, calendar_core::isoWeek(s.dayY, s.dayM, s.dayD), CL.day, doy, diy);
  const Fit tc(r, kFontSmall, chips, w - 8);
  drawCentered(r, kFontSmall, x, w, y, tc.c_str());
  y += lhS + 8;

  if (calendar_config::kHolidaysEnabled) {
    const auto di = dayInfo(c, s.dayY, s.dayM, s.dayD);
    const char* lb = holiday_core::label(c.lang, di);
    const int slotH = lhT + 12;
    if (lb[0]) {
      if (di.off) r.fillRectDither(x + 2, y + 2, w - 4, slotH - 4, Color::LightGray);
      r.drawRoundedRect(x, y, w, slotH, 2, 10, true);
      const Fit t(r, kFontText, lb, w - 20, kBold);
      drawCentered(r, kFontText, x, w, y + (slotH - lhT) / 2, t.c_str(), true, kBold);
    }
    y += slotH + 8;
  }
  return y;
}

void drawDay(GfxRenderer& r, const Frame& f, const State& s, const Ctx& c, HitMap& hit) {
  const Txt& T = txt(c.lang);
  const int pad = calendar_config::kSidePad;
  const bool isToday = s.dayY == c.t.year && s.dayM == c.t.month && s.dayD == c.t.day;
  int y = drawHeader(r, f, T, T.dayTitle, isToday ? Right::TodayFilled : Right::Today, 0, hit);

  const int32_t cur = calendar_core::daysFromCivil(s.dayY, s.dayM, s.dayD);
  auto shifted = [&](int delta, int& yy, unsigned& mm, unsigned& dd) { calendar_core::civilFromDays(cur + delta, yy, mm, dd); };

  const int navY = f.bottom - kNavH;
  if (!f.land) {
    const int x = f.x + pad, w = f.w - 2 * pad;
    y = drawDayTitle(r, x, y, w, c, T, s);
    y += drawSunCard(r, x, y, w, c, T, s, /*withCurve=*/true);
    y += drawMoonCard(r, x, y, w, c, T, s);
    y += drawWeatherCard(r, x, y, w, c, T, s, hit);
  } else {
    const int gap = 22, colW = (f.w - 2 * pad - gap) / 2;
    const int lx = f.x + pad, rx = lx + colW + gap;
    int ly = drawDayTitle(r, lx, y, colW, c, T, s);
    drawSunCard(r, lx, ly, colW, c, T, s, /*withCurve=*/false);
    int ry = drawMoonCard(r, rx, y, colW, c, T, s);
    drawWeatherCard(r, rx, y + ry, colW, c, T, s, hit);
  }
  // Навигация: предыдущий и следующий день — кнопками внизу.
  int py, ny;
  unsigned pm, pd, nm, nd;
  shifted(-1, py, pm, pd);
  shifted(1, ny, nm, nd);
  char lp[48], ln[48];
  std::snprintf(lp, sizeof(lp), "< %s %u", calendar_core::weekdayShort(c.lang, calendar_core::weekday(py, pm, pd)), pd);
  std::snprintf(ln, sizeof(ln), "%s %u >", calendar_core::weekdayShort(c.lang, calendar_core::weekday(ny, nm, nd)), nd);
  const int bx = f.x + pad, bw = (f.w - 2 * pad - 12) / 2;
  chip(r, bx, navY, kNavH, lp, false, bw);
  chip(r, bx + bw + 12, navY, kNavH, ln, false, bw);
  hit.add(bx, navY, bw, kNavH, Act::Prev);
  hit.add(bx + bw + 12, navY, bw, kNavH, Act::Next);
}

// ---- «Год» ------------------------------------------------------------------------------------------------------

void drawYear(GfxRenderer& r, const Frame& f, const State& s, const Ctx& c, HitMap& hit) {
  const Txt& T = txt(c.lang);
  const int pad = calendar_config::kSidePad;
  // Название: «2026 · 1/2» (полугодие: 1/2 — январь–июнь, 2/2 — июль–декабрь).
  const int m0 = s.half * 6 + 1;
  char ttl[32];
  std::snprintf(ttl, sizeof(ttl), "%d  \xC2\xB7  %d/2", s.year, s.half + 1);
  int y0 = drawHeader(r, f, T, ttl, s.year == c.t.year ? Right::TodayFilled : Right::Today, 0, hit);

  // 6 месяцев: портрет 2×3, ландшафт 3×2 — крупные ячейки (12 месяцев сразу слипаются в цифровую кашу).
  const int cols = f.land ? 3 : 2, rows = f.land ? 2 : 3;
  const int gapX = f.land ? 14 : 16, gapY = 8;
  const int x0 = f.x + pad, w = f.w - 2 * pad;
  const int mw = (w - (cols - 1) * gapX) / cols;
  const int mh = (f.bottom - y0 - (rows - 1) * gapY) / rows;
  const int lhS = lineH(r, kFontSmall);
  const int titleH = lhS + 4;
  const int cw = mw / 7;
  // Шапка дней недели и 6 строк недель; если строки тесные — узкий шрифт.
  int rowH = (mh - titleH) / 7;
  const int nf = rowH >= lhS + 2 ? kFontSmall : SMALL_FONT_ID;
  const int lhN = lineH(r, nf);
  rowH = std::max(12, rowH);

  for (int i = 0; i < 6; ++i) {
    const int m = m0 + i;
    const int col = i % cols, row = i / cols;
    const int mx = x0 + col * (mw + gapX), my = y0 + row * (mh + gapY);
    const bool curMonth = s.year == c.t.year && static_cast<unsigned>(m) == c.t.month;
    hit.add(mx, my, mw, mh, Act::OpenMonth, s.year * 100 + m);

    const char* mn = calendar_core::monthName(c.lang, m);
    const int tw = textW(r, kFontSmall, mn, kBold);
    if (curMonth) {
      r.fillRoundedRect(mx + (mw - tw) / 2 - 12, my, tw + 24, titleH - 2, 9, Color::Black);
      r.drawText(kFontSmall, mx + (mw - tw) / 2, my + 1, mn, false, kBold);
    } else {
      r.drawText(kFontSmall, mx + (mw - tw) / 2, my + 1, mn, true, kBold);
    }
    const int gx = mx + (mw - cw * 7) / 2;
    int gy = my + titleH;
    for (int d = 0; d < 7; ++d) {
      char ab[8];
      abbrTo(ab, sizeof(ab), calendar_core::weekdayShort(c.lang, d), 1);
      drawCentered(r, nf, gx + d * cw, cw, gy + (rowH - lhN) / 2, ab, true, d >= 5 ? kBold : kRegular);
    }
    r.drawLine(gx, gy + rowH - 1, gx + cw * 7, gy + rowH - 1, 1, true);
    gy += rowH;

    const unsigned lead = calendar_core::weekday(s.year, m, 1), dim = calendar_core::daysInMonth(s.year, m);
    for (unsigned d = 1; d <= dim; ++d) {
      const unsigned idx = lead + d - 1;
      const int cx = gx + static_cast<int>(idx % 7) * cw, cy = gy + static_cast<int>(idx / 7) * rowH;
      const bool isToday = curMonth && d == c.t.day;
      const auto di = dayInfo(c, s.year, m, d);
      char num[4];
      std::snprintf(num, sizeof(num), "%u", d);
      const int nw = textW(r, nf, num, isToday ? kBold : kRegular);
      if (isToday) {
        r.fillRoundedRect(cx + 1, cy + 1, cw - 2, rowH - 2, 6, Color::Black);
      } else if (di.off) {
        r.fillRectDither(cx + 1, cy + 1, cw - 2, rowH - 2, Color::LightGray);
      }
      r.drawText(nf, cx + (cw - nw) / 2, cy + (rowH - lhN) / 2, num, !isToday, isToday ? kBold : kRegular);
    }
  }
}


// ---- «Место» ----------------------------------------------------------------------------------------------------

// «14:02» сегодня, «25.09 14:02» — другой день.
void fmtWhen(uint32_t epoch, const Ctx& c, char* out, size_t n) {
  out[0] = '\0';
  std::tm lt{};
  if (!epoch || !TimeUtils::getLocalDateTime(epoch, lt)) return;
  if (lt.tm_mday == static_cast<int>(c.t.day) && lt.tm_mon + 1 == static_cast<int>(c.t.month)) {
    std::snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
  } else {
    std::snprintf(out, n, "%02d.%02d %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min);
  }
}

// Крупная жирная строка и мелкая под ней, обе обрезаны по ширине. Возвращает новую y.
int twoLines(GfxRenderer& r, int x, int y, int w, const char* big, const char* small) {
  const Fit a(r, kFontText, big, w, kBold);
  r.drawText(kFontText, x, y, a.c_str(), true, kBold);
  y += lineH(r, kFontText) + 2;
  if (small && small[0]) {
    const Fit b(r, kFontSmall, small, w);
    r.drawText(kFontSmall, x, y, b.c_str(), true);
    y += lineH(r, kFontSmall) + 2;
  }
  return y;
}

// Режим (авто / вручную), что сказал сервис геолокации, ручное место, поиск города, журнал на SD.
// Портрет — одной колонкой; ландшафт — слева режим и места, справа поиск и журнал.
void drawPlace(GfxRenderer& r, const Frame& f, const Ctx& c, HitMap& hit) {
  const Txt& T = txt(c.lang);
  const PlaceView& pv = c.place;
  const int y0 = drawHeader(r, f, T, T.placeTitle, Right::None, 0, hit);
  const int pad = calendar_config::kSidePad;
  const int lhT = lineH(r, kFontText), lhS = lineH(r, kFontSmall);
  constexpr int kBtnH = 44, kGap = 12;
  int lx = f.x + pad, lw = f.w - 2 * pad, ly = y0;
  if (f.land) lw = (f.w - 2 * pad - 24) / 2;

  // Переключатель режима: выбранный — чёрный.
  const int bw = (lw - kGap) / 2;
  chip(r, lx, ly, kBtnH, T.modeAuto, pv.autoLocation, bw);
  hit.add(lx, ly, bw, kBtnH, Act::SetAuto);
  chip(r, lx + bw + kGap, ly, kBtnH, T.modeManual, !pv.autoLocation, bw);
  hit.add(lx + bw + kGap, ly, bw, kBtnH, Act::SetManual);
  ly += kBtnH + 12;

  // Что сказал сервис геолокации — видно и в режиме «Вручную» (например, в офисе IP «уводит» в другую страну).
  {
    Card k = beginCard(r, lx, ly, lw, T.byIp);
    const bool known = pv.ip && pv.ip->fromIp;
    char info[112] = "";
    if (known) {
      char when[24];
      fmtWhen(pv.ip->ipEpoch, c, when, sizeof(when));
      std::snprintf(info, sizeof(info), T.ipVia, pv.ip->ssid[0] ? pv.ip->ssid : "?", when);
    }
    k.cy = twoLines(r, lx + 12, k.cy, lw - 24, known ? (pv.ip->city[0] ? pv.ip->city : "?") : T.ipNever, info);
    if (known) {
      k.cy += 4;
      const int w = chip(r, lx + 12, k.cy, kChipH, T.pinIp, false);
      hit.add(lx + 8, k.cy - 3, w + 8, kChipH + 6, Act::PinIp);
      k.cy += kChipH + 2;
    }
    ly += endCard(r, k);
  }
  // Место «вручную» и поиск города.
  {
    Card k = beginCard(r, lx, ly, lw, T.modeManual);
    char coords[48] = "";
    if (pv.manual) std::snprintf(coords, sizeof(coords), "%.4f, %.4f", pv.manual->lat, pv.manual->lon);
    k.cy = twoLines(r, lx + 12, k.cy, lw - 24, pv.manual && pv.manual->city[0] ? pv.manual->city : "?", coords);
    k.cy += 4;
    const int w = chip(r, lx + 12, k.cy, kChipH, T.findCity, false);
    hit.add(lx + 8, k.cy - 3, w + 8, kChipH + 6, Act::SearchCity);
    const int cx = lx + 12 + w + 12;
    const int w2 = chip(r, cx, k.cy, kChipH, T.coordsBtn, false);
    hit.add(cx - 4, k.cy - 3, w2 + 8, kChipH + 6, Act::AddCoords);
    k.cy += kChipH + 2;
    ly += endCard(r, k);
  }

  // Правая колонка (ландшафт) или продолжение (портрет): поиск, внизу — журнал.
  const int rx = f.land ? lx + lw + 24 : lx;
  const int rw = lw;
  int ry = f.land ? y0 : ly;
  const int logY = f.bottom - kChipH - 2;
  if (pv.search != SearchState::Idle) {
    char st[112];
    switch (pv.search) {
      case SearchState::Waiting:
        std::snprintf(st, sizeof(st), T.searching, pv.query);
        break;
      case SearchState::Found:
        std::snprintf(st, sizeof(st), "%s", T.pick);
        break;
      case SearchState::NotFound:
        std::snprintf(st, sizeof(st), T.notFound, pv.query);
        break;
      default:
        std::snprintf(st, sizeof(st), "%s", T.failed);
        break;
    }
    const Fit t(r, kFontSmall, st, rw, kBold);
    r.drawText(kFontSmall, rx, ry, t.c_str(), true, kBold);
    ry += lhS + 6;
    if (pv.search == SearchState::Found) {
      // Одной строкой «Город  Регион, Страна» — чтобы все kMaxGeoHits вариантов помещались и в портрете.
      const int rowH = lhS + 18;
      for (int i = 0; i < pv.nHits && ry + rowH <= logY - 8; ++i) {
        const weather_core::GeoHit& h = pv.hits[i];
        r.drawRoundedRect(rx, ry, rw, rowH, 2, 10, true);
        const int ty = ry + (rowH - lhS) / 2;
        const Fit name(r, kFontSmall, h.name, rw / 2, kBold);
        r.drawText(kFontSmall, rx + 12, ty, name.c_str(), true, kBold);
        const int nx = rx + 12 + textW(r, kFontSmall, name.c_str(), kBold) + 10;
        const Fit reg(r, kFontSmall, h.region, rx + rw - 12 - nx);
        r.drawText(kFontSmall, nx, ty, reg.c_str(), true);
        hit.add(rx, ry, rw, rowH, Act::PickHit, i);
        ry += rowH + 6;
      }
    }
  }

  // Поиска нет — сохранённые места: тап — выбрать, «x» справа — убрать.
  if (pv.search == SearchState::Idle) {
    if (pv.note && pv.note[0]) {
      const Fit t(r, kFontSmall, pv.note, rw, kBold);
      r.drawText(kFontSmall, rx, ry, t.c_str(), true, kBold);
      ry += lhS + 6;
    }
    if (pv.nSaved > 0 && ry + lhS + 6 < logY - 8) {
      r.drawText(kFontSmall, rx, ry, T.savedTitle, true, kBold);
      ry += lhS + 6;
      const int rowH = lhS + 18, delW = rowH + 6;
      for (int i = 0; i < pv.nSaved && ry + rowH <= logY - 8; ++i) {
        const weather_core::Place& p = pv.saved[i];
        const bool current = !pv.autoLocation && pv.manual &&
                             weather_core::samePlace(p.lat, p.lon, pv.manual->lat, pv.manual->lon);
        const int pw = rw - delW - 6;  // зона выбора; справа, через зазор, — кнопка удаления
        r.drawRoundedRect(rx, ry, pw, rowH, current ? 3 : 2, 10, true);
        const int ty = ry + (rowH - lhS) / 2;
        int nx = rx + 12;
        if (current) {
          drawGlyph(r, Glyph::Pin, nx, ty, lhS);
          nx += lhS + 4;
        }
        const Fit name(r, kFontSmall, p.city[0] ? p.city : "?", pw / 2, kBold);
        r.drawText(kFontSmall, nx, ty, name.c_str(), true, kBold);
        nx += textW(r, kFontSmall, name.c_str(), kBold) + 10;
        char co[40];
        std::snprintf(co, sizeof(co), "%.2f, %.2f", p.lat, p.lon);
        if (textW(r, kFontSmall, co) <= rx + pw - 12 - nx) r.drawText(kFontSmall, nx, ty, co, true);
        hit.add(rx, ry, pw, rowH, Act::PickSaved, i);
        const int dx = rx + rw - delW;
        r.drawRoundedRect(dx, ry, delW, rowH, 2, 10, true);
        drawCentered(r, kFontSmall, dx, delW, ty, "x", true, kBold);
        hit.add(dx, ry, delW, rowH, Act::DeleteSaved, i);
        ry += rowH + 6;
      }
    }
  }

  // Журнал на SD — внизу, всегда на одном месте.
  char hint[64];
  std::snprintf(hint, sizeof(hint), T.logHint, pv.logDir);
  const int w = chip(r, rx, logY, kChipH, pv.sdLog ? T.logOn : T.logOff, pv.sdLog);
  hit.add(rx - 4, logY - 3, w + 8, kChipH + 6, Act::ToggleLog);
  if (textW(r, kFontSmall, hint) <= rw - w - 12) {  // путь к папке — только целиком (в узкой колонке ландшафта не влезает)
    r.drawText(kFontSmall, rx + w + 12, logY + (kChipH - lhS) / 2, hint, true);
  }
}

}  // namespace

const char* notCoordsFmt(Lang lang) { return txt(lang).notCoords; }

void draw(GfxRenderer& r, const Rect& vp, const State& s, const Ctx& c, HitMap& hit) {
  const Frame f = makeFrame(vp);
  switch (s.screen) {
    case Screen::Weather:
      drawWeather(r, f, s, c, hit);
      break;
    case Screen::Day:
      drawDay(r, f, s, c, hit);
      break;
    case Screen::Year:
      drawYear(r, f, s, c, hit);
      break;
    case Screen::Place:
      drawPlace(r, f, c, hit);
      break;
    case Screen::Main:
      break;
  }
}

}  // namespace cal_detail
