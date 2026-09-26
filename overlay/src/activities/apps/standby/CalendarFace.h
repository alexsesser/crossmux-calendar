#pragma once

#include <cstdint>

#include "CalendarDetail.h"
#include "MappedInputManager.h"

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

  // Перехват ввода для вложенных экранов (погода, день, год). Вызывается хуком в начале StandbyActivity::loop(),
  // ДО обработки Back/свайпов активностью. true — событие поглощено, активность его не обрабатывает.
  // Главный экран: поглощает только тап по тап-зоне (погода, число, название месяца); всё остальное — по-старому.
  // Вложенный экран: поглощает весь ввод (Back закрывает экран, свайпы листают страницы/дни/годы).
  static bool handleInput(MappedInputManager& input, bool immersive);
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

  bool onInput(MappedInputManager& input);
  void applyHit(const cal_detail::Hit& h);
  void closeDetail();
  void shiftDay(int days);
  void shiftYear(int delta);
  void shiftHalf(int delta);
  void showMonth(int year, unsigned month);
  void openCitySearch();  // клавиатура «найти город» поверх стендбая (экран «Место»)
  void requestCleanup(const char* why);  // следующий кадр — с полной очисткой экрана (см. render())
  cal_detail::PlaceView placeView() const;
  cal_detail::Ctx makeCtx(const cal_draw::Today& t, calendar_core::Lang lang, const cal_detail::PlaceView& pv) const;

  Snapshot snap_;
  int monthOffset_ = 0;  // относительно текущего месяца
  uint32_t lastNavMs_ = 0;
  unsigned updatesSinceCleanup_ = 0;
  // Не null — следующий render() просит FULL_REFRESH (как кнопка «Обновление экрана» в верхнем меню) вместо
  // обычного FAST_REFRESH: экран целиком сменился (главный ⇄ вложенный) или подошла плановая чистка. Строка — причина
  // для журнала. Через GfxRenderer::requestNextRefresh() — обычный публичный метод рендерера, не хук.
  const char* cleanupWhy_ = nullptr;
  uint32_t lastBeatMs_ = 0;       // последняя строка «состояние» в журнале
  bool keyboardOpen_ = false;     // открыта клавиатура «найти город» (грань ждёт ответ в tick())
  MappedInputManager* input_ = nullptr;  // из handleInput(): нужен клавиатуре


  cal_detail::State st_;    // какой экран открыт, страница, дата, год
  cal_detail::HitMap hit_;  // тап-зоны последнего кадра (заполняет render, читает handleInput)
  uint32_t lastInputMs_ = 0;

  static CalendarFace* active_;  // активная грань — для статического handleInput (RTTI в сборке нет)

  WeatherClient weather_;
  GfxRenderer* renderer_ = nullptr;  // запоминаем из render(): Wi-Fi-стеку нужен рендерер (NetworkStartup)
};
