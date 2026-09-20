#pragma once

#include <cstdint>

#include "CalendarCore.h"
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

 private:
  enum class Phase : uint8_t { Idle, Connecting };

  void loadCache();
  void saveCache();
  bool due(uint32_t nowEpoch) const;
  void startCycle(GfxRenderer& renderer);
  bool fetchAll(uint32_t nowEpoch, calendar_core::Lang lang);
  void finish(bool ok);
  void teardownWifi();

  weather_core::Cache cache_;
  Phase phase_ = Phase::Idle;
  bool loaded_ = false;
  bool ownsWifi_ = false;
  bool cacheDirty_ = false;
  uint32_t nextTryMs_ = 0;    // не начинать цикл раньше (millis)
  uint32_t phaseStartMs_ = 0;
};
