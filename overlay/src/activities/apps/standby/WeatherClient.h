#pragma once

#include <cstdint>

#include "CalendarCore.h"
#include "HolidayCore.h"
#include "WeatherCore.h"

class GfxRenderer;

// Сетевая часть грани: «Wi-Fi → место по IP → погода (текущая + 24 ч + 7 дней) → производственный календарь →
// выключить Wi-Fi». Всё, что может «зависнуть» (подключение, TLS, HTTP), выполняется в ОТДЕЛЬНОЙ задаче FreeRTOS:
// upstream ограничивает запрос лишь 60 с, и блокирующий вызов из tick() замораживал бы кнопки на минуту.
// Грань только запускает задачу и забирает готовый результат (step() дешёвый и неблокирующий); запись на SD и
// обновление данных, которые читает render(), — в основной задаче под RenderLock.
//
// Живёт внутри CalendarFace и крутится из её tick() — отдельного хука в StandbyActivity::loop() не нужно.
// Не лезет в чужие дела: если Wi-Fi занят (например, идёт синхронизация времени) — пропускает цикл; если Wi-Fi уже
// подключён кем-то другим — использует, но не выключает.
// Кэш (место, погода, прогноз, календарь праздников) лежит на SD и переживает перезагрузку и отсутствие сети.
class WeatherClient {
 public:
  // Один шаг из tick(): запускает задачу, когда пора, и забирает результат. true — данные для отрисовки изменились.
  bool step(GfxRenderer* renderer, uint32_t nowEpoch, calendar_core::Lang lang);

  // Грань закрывается. Идущая задача не прерывается (её нельзя безопасно оборвать посреди TLS): она сама доработает,
  // выключит Wi-Fi и освободит свои данные; её результат отбрасывается (будет загружен при следующем входе).
  void stop();

  const weather_core::Cache& cache() const { return cache_; }
  const holiday_core::Store& holidays() const { return hol_; }

  // Что сейчас на экране: открыт ли вложенный экран и сколько мс нет ввода. Плановый запрос не начинается, пока идёт ввод.
  void setUi(bool detailOpen, uint32_t idleMs) {
    detailOpen_ = detailOpen;
    idleMs_ = idleMs;
  }
  // Пользователь смотрит этот год календаря: если его нет в кэше — загрузить по требованию.
  void wantYear(int year) { wantYear_ = (year >= calendar_core::kMinYear && year <= calendar_core::kMaxYear) ? year : 0; }
  // Обновить погоду сейчас (кнопка Confirm на экране «Погода»): не ждёт ни паузы без ввода, ни таймера повтора.
  void requestRefresh() { forceRefresh_ = true; }

 private:
  struct Job;  // данные задачи: вход, результат, флаг готовности (см. .cpp)

  void loadCache();
  void saveCache();
  void loadHolidays();
  void saveHolidays();
  bool weatherDue(uint32_t nowEpoch) const;
  bool holidayWorkPending(uint32_t nowEpoch) const;
  bool due(uint32_t nowEpoch) const;
  void startJob(GfxRenderer& renderer, uint32_t nowEpoch, calendar_core::Lang lang);
  void applyJob(Job& job, uint32_t nowEpoch);

  weather_core::Cache cache_;
  holiday_core::Store hol_;
  bool holDirty_ = false;
  bool cacheDirty_ = false;
  int wantYear_ = 0;
  bool forceRefresh_ = false;
  bool detailOpen_ = false;
  bool loaded_ = false;
  uint32_t idleMs_ = 0;
  uint32_t holBackoffUntilMs_ = 0;  // после неудачи не долбим сервис (millis)
  uint32_t nextTryMs_ = 0;          // не начинать цикл раньше (millis)
  uint32_t lastDueCheckMs_ = 0;     // «пора ли» проверяем не чаще раза в секунду, а не на каждом такте loop()
  Job* job_ = nullptr;              // идущая задача (владелец — мы, пока она не отброшена)
};
