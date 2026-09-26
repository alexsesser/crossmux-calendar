#include "CalendarLog.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string_view>

#include "CalendarConfig.h"
#include "activities/RenderLock.h"
#include "util/TimeUtils.h"

// lib/Logging/Logging.cpp: RTC-кольцо последних сообщений LOG_* (16 ячеек по 256 байт) и его головка. Только читаем.
extern char logMessages[cal_log::kRingLines][cal_log::kRingEntry];
extern size_t logHead;
extern uint32_t rtcLogMagic;  // 0xDEADBEEF — кольцо инициализировано (иначе в RTC-памяти мусор после холодного старта)

namespace cal_log {

namespace {

constexpr const char* kDir = "/calendar-logs";
constexpr size_t kFlushAt = 16 * 1024;  // накопилось столько — пишем, не дожидаясь таймера
constexpr size_t kMaxBuf = 96 * 1024;   // карта долго не пишется (нет карты, ошибка) — больше не держим, старое выкидываем
constexpr uint32_t kPollEveryMs = 2000;  // даже без смены головки: кольцо могло обернуться ровно на полный круг

std::mutex g_mtx;  // g_buf — из любой задачи
std::string g_buf;
std::atomic<bool> g_on{calendar_config::kSdLogByDefault};
std::atomic<bool> g_finalFlush{false};  // выключили — дописать накопленное ещё один раз

// Только главная задача.
uint32_t g_lastFlushMs = 0;
uint32_t g_lastPollMs = 0;
size_t g_lastHead = static_cast<size_t>(-1);
RingCursor g_cursor;

void stamp(char* out, size_t n) {
  const uint32_t ms = millis();
  std::tm lt{};
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (now && TimeUtils::getLocalDateTime(now, lt)) {
    std::snprintf(out, n, "%02d:%02d:%02d (+%lu.%03lus)", lt.tm_hour, lt.tm_min, lt.tm_sec,
                  static_cast<unsigned long>(ms / 1000), static_cast<unsigned long>(ms % 1000));
  } else {
    std::snprintf(out, n, "--:--:-- (+%lu.%03lus)", static_cast<unsigned long>(ms / 1000),
                  static_cast<unsigned long>(ms % 1000));
  }
}

void trimLocked() {
  if (g_buf.size() <= kMaxBuf) return;
  const size_t nl = g_buf.find('\n', g_buf.size() - kMaxBuf / 2);
  g_buf.erase(0, nl == std::string::npos ? g_buf.size() : nl + 1);
  g_buf.insert(0, "... (журнал долго не записывался на карту: старые строки выброшены) ...\n");
}

void appendLocked(const char* text, size_t len) {
  g_buf.append(text, len);
  trimLocked();
}

// Новые сообщения прошивки из её кольцевого буфера.
void mirror(uint32_t now) {
  const size_t head = logHead;
  if (head == g_lastHead && now - g_lastPollMs < kPollEveryMs) return;
  g_lastHead = head;
  g_lastPollMs = now;
  if (rtcLogMagic != 0xDEADBEEFu || head >= kRingLines) return;  // кольцо ещё не заведено
  bool lost = false;
  std::string fresh;
  collect(logMessages, head, g_cursor, fresh, lost);
  if (fresh.empty() && !lost) return;
  char st[40];
  stamp(st, sizeof(st));
  std::string out;
  out.reserve(fresh.size() + 256);
  if (lost) {
    out += st;
    out += " ~ ... (часть сообщений прошивки могла пропасть) ...\n";
  }
  size_t pos = 0;
  while (pos < fresh.size()) {
    const size_t nl = fresh.find('\n', pos);
    const size_t end = nl == std::string::npos ? fresh.size() : nl;
    const std::string_view l(fresh.data() + pos, end - pos);
    pos = end + 1;
    // Свои строки в журнале уже есть (в кольцо прошивки они больше не пишутся, но старые могли остаться).
    if (l.empty() || l.find("] [CAL] ") != std::string_view::npos) continue;
    out += st;
    out += " ~ ";
    out.append(l.data(), l.size());
    out += '\n';
  }
  std::lock_guard<std::mutex> lk(g_mtx);
  appendLocked(out.data(), out.size());
}

bool writeChunk(const std::string& chunk) {
  char path[48];
  std::tm lt{};
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (now && TimeUtils::getLocalDateTime(now, lt)) {
    std::snprintf(path, sizeof(path), "%s/%04d-%02d-%02d.log", kDir, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
  } else {
    std::snprintf(path, sizeof(path), "%s/no-clock.log", kDir);
  }
  if (!Storage.ensureDirectoryExists(kDir)) return false;
  HalFile f = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!f.isOpen()) return false;
  return f.write(chunk.data(), chunk.size()) == chunk.size();  // закроет деструктор
}

void flush(bool lockHeld) {
  std::string chunk;
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_buf.empty()) return;
    chunk.swap(g_buf);
  }
  bool ok;
  if (lockHeld) {
    ok = writeChunk(chunk);
  } else {
    RenderLock lock;  // SD и экран на части плат делят шину
    ok = writeChunk(chunk);
  }
  if (!ok) {  // вернуть на место — попробуем в следующий раз (объём ограничен kMaxBuf)
    LOG_ERR("CAL", "log write to SD failed (%u bytes kept)", static_cast<unsigned>(chunk.size()));
    std::lock_guard<std::mutex> lk(g_mtx);
    chunk += g_buf;
    g_buf.swap(chunk);
    trimLocked();
  }
}

}  // namespace

const char* dirPath() { return kDir; }

bool enabled() { return g_on.load(); }

void setEnabled(bool on) {
  const bool was = g_on.exchange(on);
  if (was == on) return;
  if (on) {
    g_finalFlush = false;
    g_cursor = RingCursor{};  // первым делом скопируем то, что уже лежит в кольце прошивки: видно, что было перед включением
    g_lastHead = static_cast<size_t>(-1);
    line("LOG", "журнал включён");
  } else {
    char st[40];
    stamp(st, sizeof(st));
    std::lock_guard<std::mutex> lk(g_mtx);
    char out[80];
    const int n = std::snprintf(out, sizeof(out), "%s [LOG] журнал выключен\n", st);
    if (n > 0) appendLocked(out, std::min<size_t>(static_cast<size_t>(n), sizeof(out) - 1));
    g_finalFlush = true;
  }
}

void line(const char* tag, const char* fmt, ...) {
  char msg[384];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  // В USB-лог — напрямую, не через LOG_DBG: тот пишет ещё и в 16-строчное кольцо прошивки, и частые строки календаря
  // вытесняли из него сообщения самой прошивки раньше, чем журнал успевал их скопировать.
  if (logSerial) logSerial.printf("[%lu] [CAL] [%s] %s\n", static_cast<unsigned long>(millis()), tag, msg);
#endif
  if (!g_on.load()) return;
  char st[40];
  stamp(st, sizeof(st));
  char out[448];
  const int n = std::snprintf(out, sizeof(out), "%s [%s] %s\n", st, tag, msg);
  if (n <= 0) return;
  size_t len = std::min<size_t>(static_cast<size_t>(n), sizeof(out) - 1);
  if (out[len - 1] != '\n') out[len - 1] = '\n';  // обрезанная строка — всё равно с переводом строки
  std::lock_guard<std::mutex> lk(g_mtx);
  appendLocked(out, len);
}

void pump(bool forceFlush, bool lockHeld) {
  const bool on = g_on.load();
  if (!on && !g_finalFlush.load()) return;
  const uint32_t now = millis();
  if (on) mirror(now);
  size_t pending;
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    pending = g_buf.size();
  }
  if (!on) {  // выключили: дописать последнее и забыть
    g_finalFlush = false;
    if (pending) flush(lockHeld);
    return;
  }
  if (!pending) return;
  if (forceFlush || pending >= kFlushAt || now - g_lastFlushMs >= calendar_config::kSdLogFlushSec * 1000u) {
    // Плановая запись не ждёт, пока панель обновляется (RenderLock занят рендером): иначе стоит весь главный цикл.
    if (!forceFlush && !lockHeld && RenderLock::peek()) return;
    g_lastFlushMs = now;
    flush(lockHeld);
  }
}

}  // namespace cal_log
