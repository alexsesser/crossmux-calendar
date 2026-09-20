#pragma once

// Настройки грани «Календарь», зашитые при сборке (UI настроек в v1 нет).
namespace calendar_config {

// Погода обновляется по Wi-Fi не чаще раза в столько минут (при живых часах и сохранённой сети).
constexpr unsigned kWeatherRefreshMin = 5;
// Город по IP переопределяется не чаще раза в столько минут (и сразу при смене языка интерфейса).
constexpr unsigned kGeoRefreshMin = 180;

// Возврат к текущему месяцу через столько мс после последнего листания.
constexpr unsigned kMonthAutoReturnMs = 60u * 1000u;

// Каждое N-е минутное обновление просим «очистку от ghosting» (на Xteink-платах это полное обновление).
constexpr unsigned kGhostCleanupEveryUpdates = 30;

}  // namespace calendar_config
