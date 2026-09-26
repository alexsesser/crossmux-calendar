#pragma once

#include <cstdint>
#include <string>

#include "CalendarCore.h"
#include "HolidayCore.h"
#include "WeatherCore.h"

class GfxRenderer;

// Сетевая часть грани: «Wi-Fi → место по IP → погода (текущая + 24 ч + 7 дней) → производственный календарь →
// выключить Wi-Fi», плюс поиск города по названию для экрана «Место». Всё, что может «зависнуть» (подключение, TLS,
// HTTP), выполняется в ОТДЕЛЬНОЙ задаче FreeRTOS: upstream ограничивает запрос лишь 60 с, и блокирующий вызов из tick()
// замораживал бы кнопки на минуту. Грань только запускает задачу и забирает готовый результат (step() дешёвый и
// неблокирующий); запись на SD и обновление данных, которые читает render(), — в главной задаче под RenderLock.
//
// Wi-Fi: сканирует эфир и подключается к самой сильной из запомненных сетей, что сейчас видны (дома — домашняя,
// на работе — рабочая), не выключая радио между сканом и подключением. Не лезет в чужие дела: занят Wi-Fi кем-то
// другим — ждёт (но не дольше kWifiBusyTakeoverSec, если он так и не подключился); уже подключён кем-то — пользуется
// и не выключает.
//
// Место: «Авто» — по IP (ipwhois.app), перепроверяется раз в kGeoRefreshMin и сразу при смене сети; «Вручную» —
// выбранный на экране «Место» город (поиск Open-Meteo Geocoding) или место по умолчанию из CalendarConfig.h.
// Режим, ручное место и включённость журнала хранятся на SD отдельно от кэша (calendar_settings.json).
class WeatherClient {
 public:
  // Один шаг из tick(): запускает задачу, когда пора, и забирает результат. true — данные для отрисовки изменились.
  bool step(GfxRenderer* renderer, uint32_t nowEpoch, calendar_core::Lang lang);

  // Грань закрывается. Идущая задача не прерывается посреди TLS: она сама быстро закончит (новых запросов после этого
  // не начинает), выключит Wi-Fi и освободит свои данные; её результат отбрасывается.
  // lockHeld — вызывающий уже держит RenderLock (выход из активности): запись на SD — без повторного захвата.
  void stop(bool lockHeld);

  // Для отрисовки. cache().place — место, для которого показывается погода и считается солнце: в режиме «Вручную» —
  // выбранное, в «Авто» — последнее определённое по IP (пока не определялось — место по умолчанию).
  const weather_core::Cache& cache() const { return cache_; }
  const holiday_core::Store& holidays() const { return hol_; }
  const weather_core::Place& ipPlace() const { return ipPlace_; }  // что сказал сервис геолокации
  const weather_core::Settings& settings() const { return settings_; }

  // ---- Экран «Место». Вызывать под RenderLock (обработчик ввода уже держит его): меняют то, что читает render().
  // Запись настроек на SD — позже, из step() (под RenderLock её не сделать: повторный захват = взаимная блокировка).
  void setAutoLocation(bool on);
  void pinIpPlace();                                  // место по IP → ручное (режим «Вручную»)
  void setManualPlace(const weather_core::Place& p);  // выбранное место → ручное (режим «Вручную»)
  void setSdLog(bool on);

  enum class Search : uint8_t { Idle, Waiting, Found, NotFound, Failed };
  void startSearch(const char* query);  // поиск города по названию; ответ — searchState()/searchHit()
  void pickSearchHit(int i);            // выбранный результат → ручное место
  void clearSearch();
  Search searchState() const { return search_; }
  const char* searchQuery() const { return searchQuery_; }
  int searchCount() const { return nHits_; }
  const weather_core::GeoHit& searchHit(int i) const { return hits_[i]; }
  void pickSaved(int i);    // сохранённое место → ручное (и первым в списке)
  void removeSaved(int i);  // убрать из сохранённых

  // ---- История погоды (экран «День», прошедшие даты; даты — год*10000 + месяц*100 + день).
  enum class Hist : uint8_t { None, Waiting, Failed };
  // Смотрят прошедший день date: архивной записи нет — загрузить из архива (сразу ±3 дня, чтобы листание соседних
  // дней не требовало новых запросов). today — сегодняшняя дата.
  void wantHistory(int32_t date, int32_t today);
  Hist historyState(int32_t date) const;
  const weather_core::HistStore& history() const { return hist_; }

  // Что сейчас на экране: открыт ли вложенный экран и сколько мс нет ввода. Плановый запрос не начинается, пока идёт ввод.
  void setUi(bool detailOpen, uint32_t idleMs) {
    detailOpen_ = detailOpen;
    idleMs_ = idleMs;
  }
  // Пользователь смотрит этот год календаря: если его нет в кэше — загрузить по требованию.
  void wantYear(int year) { wantYear_ = (year >= calendar_core::kMinYear && year <= calendar_core::kMaxYear) ? year : 0; }
  // Обновить погоду сейчас (кнопка Confirm на экране «Погода»): не ждёт ни паузы без ввода, ни таймера повтора.
  void requestRefresh() { forceRefresh_ = true; }
  bool refreshing() const { return forceRefresh_ || wxInJob_; }  // погода сейчас загружается

 private:
  struct Job;  // данные задачи: вход, результат, флаг готовности (см. .cpp)

  void loadCache();
  void saveCache();
  void loadHolidays();
  void saveHolidays();
  void loadSettings();
  void saveSettings(bool lockHeld = false);
  void loadHistory();
  void saveHistory();
  void applyEffectivePlace();  // cache_.place ← ручное / по IP; погода для другого места — сбросить и загрузить заново
  bool weatherDue(uint32_t nowEpoch) const;
  bool holidayWorkPending(uint32_t nowEpoch) const;
  bool due(uint32_t nowEpoch) const;
  void startJob(GfxRenderer& renderer, uint32_t nowEpoch, calendar_core::Lang lang);
  void applyJob(Job& job, uint32_t nowEpoch);

  weather_core::Cache cache_;
  weather_core::Place ipPlace_;
  weather_core::Settings settings_;
  holiday_core::Store hol_;
  bool holDirty_ = false;
  bool cacheDirty_ = false;
  bool settingsDirty_ = false;
  bool forceGeo_ = false;  // включили «Авто» — определить место по IP в ближайший выход в сеть
  int wantYear_ = 0;
  bool forceRefresh_ = false;
  bool detailOpen_ = false;
  bool loaded_ = false;
  uint32_t idleMs_ = 0;
  uint32_t holBackoffUntilMs_ = 0;  // после неудачи не долбим сервис (millis)
  uint32_t nextTryMs_ = 0;          // не начинать цикл раньше (millis)
  uint32_t lastDueCheckMs_ = 0;     // «пора ли» проверяем не чаще раза в секунду, а не на каждом такте loop()
  uint32_t busySinceMs_ = 0;        // с какого момента Wi-Fi занят кем-то другим (0 — не занят)
  Search search_ = Search::Idle;
  bool searchQueued_ = false;       // запрос поиска ещё не отправлен в сеть
  char searchQuery_[64] = "";
  weather_core::GeoHit hits_[weather_core::kMaxGeoHits];
  int nHits_ = 0;
  Job* job_ = nullptr;              // идущая задача (владелец — мы, пока она не отброшена)
  weather_core::HistStore hist_;
  bool histDirty_ = false;
  bool wxInJob_ = false;            // идущая задача загружает погоду
  int32_t histWant_ = 0;            // какой прошедший день нужен (0 — никакой)
  int32_t histToday_ = 0;
  bool histQueued_ = false;         // запрос архива ещё не отправлен в сеть
  bool histInJob_ = false;          // идущая задача грузит архив для histWant_
  int32_t histFailed_ = 0;          // для этого дня архив не загрузился…
  uint32_t histFailedMs_ = 0;       // …тогда (millis): повтор не раньше kRetryAfterFailMin
};
