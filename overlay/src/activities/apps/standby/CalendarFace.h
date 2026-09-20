#pragma once

#include <cstdint>

#include "CalendarCore.h"
#include "StandbyFace.h"
#include "WeatherClient.h"

// Григорианский календарь для Standby: крупное время, день недели, дата в двух форматах,
// номер недели / день года, погода (Open-Meteo, место по IP), восход/закат (офлайн), сетка месяца.
//
// Адаптивна: при width > height — две колонки (слева сведения, справа сетка), иначе одна.
// Up/Down (кнопки или свайп) листают месяц; через kMonthAutoReturnSec без ввода — возврат к текущему.
// Без кучи: всё состояние лежит в самом объекте, которым владеет StandbyActivity.
class CalendarFace final : public StandbyFace {
 public:
  void onEnter() override;
  void onExit() override;
  TickResult tick() override;
  void render(GfxRenderer& renderer, const Rect& viewport) override;
  StrId titleId() const override;
  uint32_t secondsUntilNextWake() const override;

  void onPagePrev() override;  // Up: месяц назад
  void onPageNext() override;  // Down: месяц вперёд

 private:
  struct Snapshot {
    bool valid = false;
    int year = 0;
    unsigned month = 0, day = 0, hour = 0, minute = 0;
    unsigned weekday = 0;    // 0 = Пн
    unsigned dayOfYear = 0;
    unsigned isoWeek = 0;
    unsigned daysInYear = 0;
    uint32_t epoch = 0;
    uint32_t minuteKey = 0;  // epoch / 60: смена = перерисовать время
    uint32_t dayKey = 0;     // дней с 1970: смена = пересчитать сетку
    int utcOffsetMin = 0;
  };

  // Читает часы. false — часам нельзя верить.
  static bool takeSnapshot(Snapshot& out);
  void shiftMonth(int delta);

  Snapshot snap_;
  int monthOffset_ = 0;  // относительно текущего месяца
  uint32_t lastNavMs_ = 0;
  unsigned updatesSinceCleanup_ = 0;


  WeatherClient weather_;
  GfxRenderer* renderer_ = nullptr;  // запоминаем из render(): Wi-Fi-стеку нужен рендерер (NetworkStartup)
};
