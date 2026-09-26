#pragma once

#include <GfxRenderer.h>

#include <cstdint>

#include "CalendarDraw.h"
#include "HolidayCore.h"
#include "WeatherCore.h"

// Вложенные экраны грани «Календарь»: «Погода» (сегодня по часам и 7 дней), «День», «Год», «Место».
// Рисуются в обеих ориентациях; заодно заполняют карту тап-зон — геометрия отрисовки и хит-теста из одного места
// (правило CrossMux №11): разбор тапа смотрит в ту же карту, которую только что заполнил render().
namespace cal_detail {

enum class Screen : uint8_t { Main, Weather, Day, Year, Place };

enum class Act : uint8_t {
  None,
  OpenWeather,  // главный → «Погода»
  OpenDay,      // arg = дата (packDate)
  OpenYear,     // главный → «Год»
  Close,        // ✕ Назад
  GoToday,      // «Сегодня»
  Prev,         // ‹ предыдущий (день)
  Next,         // › следующий (день)
  OpenWeek,     // карточка погоды дня → «Погода», 7 дней
  OpenMonth,    // «Год» → главный на этом месяце; arg = год*100 + месяц
  OpenPlace,    // тап по названию города (главный, «Погода») → «Место»
  SetAuto,      // «Место»: определять по IP
  SetManual,    // «Место»: вручную
  SearchCity,   // «Место»: найти город по названию (клавиатура)
  PinIp,        // «Место»: город по IP → ручное место
  PickHit,      // «Место»: выбрать найденный город; arg = номер
  ToggleLog,    // «Место»: журнал на SD вкл/выкл
  AddCoords,    // «Место»: ввести координаты и подпись (клавиатура)
  PickSaved,    // «Место»: выбрать сохранённое место; arg = номер
  DeleteSaved,  // «Место»: убрать сохранённое место; arg = номер
  Refresh,      // «Погода»: обновить сейчас
};

struct Hit {
  int16_t x, y, w, h;
  Act act;
  int32_t arg;
};

struct HitMap {
  static constexpr int kMax = 64;
  Hit h[kMax];
  int n = 0;
  int vw = 0, vh = 0;  // размеры области, для которой карта построена

  void clear(int w, int hgt) {
    n = 0;
    vw = w;
    vh = hgt;
  }
  void add(int x, int y, int w, int hgt, Act a, int32_t arg = 0) {
    if (n < kMax) h[n++] = Hit{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w),
                               static_cast<int16_t>(hgt), a, arg};
  }
  // Верхний (последний добавленный) элемент под точкой.
  const Hit* at(int px, int py) const {
    for (int i = n - 1; i >= 0; --i) {
      if (px >= h[i].x && px < h[i].x + h[i].w && py >= h[i].y && py < h[i].y + h[i].h) return &h[i];
    }
    return nullptr;
  }
};

constexpr int32_t packDate(int y, unsigned m, unsigned d) { return y * 10000 + static_cast<int32_t>(m) * 100 + static_cast<int32_t>(d); }
inline void unpackDate(int32_t v, int& y, unsigned& m, unsigned& d) {
  y = v / 10000;
  m = static_cast<unsigned>((v / 100) % 100);
  d = static_cast<unsigned>(v % 100);
}

struct State {
  Screen screen = Screen::Main;
  uint8_t page = 0;  // «Погода»: 0 — сегодня по часам, 1 — 7 дней
  int dayY = 0;      // «День»: выбранная дата
  unsigned dayM = 0, dayD = 0;
  int year = 0;      // «Год»: показываемый год
  uint8_t half = 0;  // «Год»: полугодие (0 — январь–июнь, 1 — июль–декабрь): 12 месяцев на 480×800 не читаются
};

// Экран «Место»: режим, что сказал сервис геолокации, ручное место, поиск города, журнал. Собирает грань.
enum class SearchState : uint8_t { Idle, Waiting, Found, NotFound, Failed };
struct PlaceView {
  bool autoLocation = true;
  const weather_core::Place* ip = nullptr;      // fromIp == false — по IP ещё не определялось
  const weather_core::Place* manual = nullptr;
  SearchState search = SearchState::Idle;
  const char* query = "";
  int nHits = 0;
  const weather_core::GeoHit* hits = nullptr;
  bool sdLog = false;
  const char* logDir = "";
  const weather_core::Place* saved = nullptr;  // сохранённые места
  int nSaved = 0;
  const char* note = "";                       // сообщение под карточками (например, «не координаты»)
};

// Архив погоды для дня на экране «День»: 0 — нет запроса, 1 — загружается, 2 — не удалось.
enum class HistState : uint8_t { None, Waiting, Failed };

// Всё, что нужно для рисования, кроме самого состояния экрана.
struct Ctx {
  const cal_draw::Today& t;
  calendar_core::Lang lang;
  const weather_core::Cache& wx;
  const holiday_core::Store& hol;
  const PlaceView& place;
  const weather_core::HistStore& hist;
  HistState histState;  // для дня, открытого на экране «День»
  bool wxRefreshing;     // погода сейчас загружается (кнопка «Обновить» — «Обновляю...»)
};

// Рисует вложенный экран s.screen (Weather / Day / Year / Place) и заполняет карту тап-зон.
void draw(GfxRenderer& r, const Rect& vp, const State& s, const Ctx& c, HitMap& hit);

// Формат сообщения «не координаты: «%s»» на языке интерфейса (для экрана «Место»).
const char* notCoordsFmt(calendar_core::Lang lang);

// Подпись дня недели/даты «Пн 21» и т.п. вынесены в реализацию. Классификация дня с учётом выключателя праздников:
holiday_core::DayInfo dayInfo(const Ctx& c, int y, unsigned m, unsigned d);

}  // namespace cal_detail
