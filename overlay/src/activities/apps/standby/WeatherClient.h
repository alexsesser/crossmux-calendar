#pragma once

#include <cstdint>

#include "CalendarCore.h"
#include "HolidayCore.h"
#include "WeatherCore.h"

class GfxRenderer;

// Цикл «Wi-Fi → место по IP → погода → выключить Wi-Fi», раз в kWeatherRefreshMin минут.
// Живёт внутри CalendarFace и крутится из её tick() — отдельного хука в StandbyActivity::loop() не нужно.
//
// Не лезет в чужие дела: если Wi-Fi уже используется (например, идёт синхронизация времени) —
// пропускает цикл; если Wi-Fi поднят и подключён кем-то другим — использует, но не выключает.
// Кэш (место + последняя погода) лежит на SD: город и данные переживают перезагрузку и отсутствие сети.
class WeatherClient {
 public:
  // Один шаг. Быстрый, кроме момента самой загрузки (блокирующий HTTP, единицы секунд).
  // true — данные для отрисовки изменились.
  bool step(GfxRenderer* renderer, uint32_t nowEpoch, calendar_core::Lang lang);

  // Грань закрывается: если мы подняли Wi-Fi и ещё не выключили — выключить.
  void stop();

  const weather_core::Cache& cache() const { return cache_; }
  const holiday_core::Store& holidays() const { return hol_; }

  // Что сейчас на экране: открыт ли вложенный экран и сколько мс нет ввода. Запрос не начинается, пока идёт ввод
  // (он блокирует цикл на пару секунд и «съел» бы свайп).
  void setUi(bool detailOpen, uint32_t idleMs) {
    detailOpen_ = detailOpen;
    idleMs_ = idleMs;
  }
  // Пользователь смотрит этот год календаря: если его нет в кэше — загрузить по требованию.
  void wantYear(int year) { wantYear_ = (year >= calendar_core::kMinYear && year <= calendar_core::kMaxYear) ? year : 0; }
  // Обновить погоду сейчас (кнопка Confirm на экране «Погода»).
  void requestRefresh() { forceRefresh_ = true; }

 private:
  enum class Phase : uint8_t { Idle, Connecting };

  void loadCache();
  void saveCache();
  void loadHolidays();
  void saveHolidays();
  bool weatherDue(uint32_t nowEpoch) const;
  bool holidayWorkPending(uint32_t nowEpoch) const;
  bool fetchHolidays(uint32_t nowEpoch);
  bool due(uint32_t nowEpoch) const;
  void startCycle(GfxRenderer& renderer);
  bool fetchAll(uint32_t nowEpoch, calendar_core::Lang lang);
  void finish(bool ok);
  void teardownWifi();

  weather_core::Cache cache_;
  holiday_core::Store hol_;
  bool holDirty_ = false;
  int wantYear_ = 0;
  bool forceRefresh_ = false;
  bool detailOpen_ = false;
  uint32_t idleMs_ = 0;
  uint32_t holBackoffUntilMs_ = 0;  // после неудачи не долбим сервис (millis)
  Phase phase_ = Phase::Idle;
  bool loaded_ = false;
  bool ownsWifi_ = false;
  bool cacheDirty_ = false;
  uint32_t nextTryMs_ = 0;    // не начинать цикл раньше (millis)
  uint32_t phaseStartMs_ = 0;
};
