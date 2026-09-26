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

// Чистая часть (host-тест): копирование кольцевого буфера сообщений прошивки (lib/Logging: 16 ячеек по 256 байт,
// logHead — куда ляжет следующая строка). Читаем по ячейкам, а не текстом: строка длиннее 255 байт в ячейке обрезана
// без перевода строки и «склеивалась» со следующей — прежний поиск по тексту из-за этого путал старое с новым.
constexpr size_t kRingLines = 16;   // MAX_LOG_LINES
constexpr size_t kRingEntry = 256;  // MAX_ENTRY_LEN

struct RingCursor {
  bool started = false;
  size_t head = 0;   // logHead на момент прошлого чтения
  std::string last;  // содержимое последней прочитанной ячейки (head-1): изменилось — кольцо обернулось
};

// Новые строки (по порядку, каждая с переводом строки) дописываются в out. lost — между чтениями строк было не меньше,
// чем ячеек в кольце: часть могла пропасть (тогда в out — всё кольцо). Первое чтение — всё, что в кольце уже есть.
inline void collect(const char (*ring)[kRingEntry], size_t head, RingCursor& cur, std::string& out, bool& lost) {
  auto slot = [ring](size_t i) {
    const char* e = ring[i % kRingLines];
    size_t n = 0;
    while (n < kRingEntry && e[n]) ++n;
    return std::string(e, n);
  };
  lost = false;
  head %= kRingLines;
  size_t from = head, count = kRingLines;  // всё кольцо, от самой старой ячейки
  if (cur.started) {
    if (slot(cur.head + kRingLines - 1) == cur.last) {
      from = cur.head;
      count = (head + kRingLines - cur.head) % kRingLines;
    } else {
      lost = true;
    }
  }
  for (size_t k = 0; k < count; ++k) {
    const std::string s = slot(from + k);
    if (s.empty()) continue;
    out += s;
    if (out.back() != '\n') out += '\n';
  }
  cur.started = true;
  cur.head = head;
  cur.last = slot(head + kRingLines - 1);
}

}  // namespace cal_log
