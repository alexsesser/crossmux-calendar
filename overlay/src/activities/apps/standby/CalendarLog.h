#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Подробный журнал грани «Календарь» на SD-карте — для диагностики на устройстве без USB-кабеля:
//   /calendar-logs/ГГГГ-ММ-ДД.log (папка видна и на компьютере, и через «Передачу файлов»).
// Что туда попадает:
//   • события календаря — line(), из любой задачи (сеть работает в своей);
//   • все сообщения самой прошивки (LOG_* CrossMux, в том числе ошибки HTTP) — копируются из её кольцевого буфера
//     последних строк, помечены «~».
// Строки копятся в памяти и дописываются на карту раз в kSdLogFlushSec (и при выходе из календаря) — только из
// главной задачи и под RenderLock, как любая запись на SD в прошивке.
// Включается и выключается на экране «Место» (настройка хранится на SD), начальное значение — kSdLogByDefault.
namespace cal_log {

void setEnabled(bool on);  // выключение: накопленное ещё будет дописано ближайшим pump()
bool enabled();

// Одна строка: «ЧЧ:ММ:СС (+аптайм) [tag] текст». Потокобезопасно. В USB-лог (pio device monitor) — тоже.
void line(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Главная задача, на каждом такте: забрать новые сообщения прошивки; по таймеру или по просьбе — дописать на карту.
// lockHeld — вызывающий уже держит RenderLock (выход из активности): тогда пишем без повторного захвата.
void pump(bool forceFlush = false, bool lockHeld = false);

// Папка журналов (для подсказки на экране).
const char* dirPath();

// Чистая часть (host-тест): что в новом снимке кольцевого буфера появилось после прошлого. tail — последняя строка
// прошлого снимка (обновляется). lost — её уже нет в буфере: между снимками строк было больше, чем он держит.
// Якорь — именно последняя строка: самые старые строки вытесняются первыми, а она — последней; и она уникальна — строки
// прошивки начинаются с «[миллисекунды]».
inline std::string newSince(const std::string& snapshot, std::string& tail, bool& lost) {
  lost = false;
  std::string fresh;
  if (tail.empty()) {
    fresh = snapshot;
  } else {
    const size_t k = snapshot.rfind(tail);
    if (k == std::string::npos) {
      lost = !snapshot.empty();
      fresh = snapshot;
    } else {
      fresh = snapshot.substr(k + tail.size());
    }
  }
  if (!snapshot.empty()) {
    size_t start = 0;  // начало последней строки (последний перевод строки не считается — он её конец)
    if (snapshot.size() >= 2) {
      const size_t nl = snapshot.rfind('\n', snapshot.size() - 2);
      if (nl != std::string::npos) start = nl + 1;
    }
    tail = snapshot.substr(start);
  }
  return fresh;
}

}  // namespace cal_log
