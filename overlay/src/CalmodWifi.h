#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Wi-Fi WPA2/WPA3-Enterprise (логин + пароль, обычно PEAP/MSCHAPv2) для CrossMux, где upstream умеет только пароль.
//
// Как это устроено (без переделки хранилища upstream):
//   • Логин и пароль лежат ОДНОЙ строкой в поле password стандартного WifiCredentialStore: "\x1F" + логин + "\x1F" + пароль.
//     Символ 0x1F с клавиатуры не набрать, поэтому спутать с обычным паролем нельзя; на SD строка обфусцируется так же, как
//     обычные пароли; «забыть сеть», галочка «сохранено», автоподключение по сохранённому работают без единой правки.
//   • Все вызовы WiFi.begin(ssid, пароль) в upstream заменены хуком на calmod_wifi::begin(ssid, пароль): обычный пароль
//     идёт как раньше, Enterprise-строка — в WiFi.begin(ssid, PEAP, …).
//   • Экран выбора сети (WifiSelectionActivity) узнаёт Enterprise-сеть по скану (noteScan) и спрашивает логин, затем пароль.
// Настройки (метод EAP, внешняя идентичность, ожидание) — CalendarConfig.h.
namespace calmod_wifi {

constexpr char kSep = '\x1F';

// ---- Чистая логика (без Arduino; host-тесты) --------------------------------------------------------------------------

// "\x1F" + логин + "\x1F" + пароль.
inline std::string compose(const std::string& user, const std::string& pass) {
  std::string s;
  s.reserve(user.size() + pass.size() + 2);
  s += kSep;
  s += user;
  s += kSep;
  s += pass;
  return s;
}

// Enterprise-строка → логин и пароль (true). Обычный пароль (или nullptr) → false, user/pass не меняются.
inline bool split(const char* stored, std::string& user, std::string& pass) {
  if (!stored || stored[0] != kSep) return false;
  const char* end = stored + 1;
  while (*end && *end != kSep) ++end;
  if (*end != kSep) return false;  // нет второго разделителя — не наша строка
  user.assign(stored + 1, end);
  pass.assign(end + 1);
  return true;
}

// ---- Работа с Wi-Fi ---------------------------------------------------------------------------------------------------

// Замена WiFi.begin(ssid, пароль): stored — то, что лежит в хранилище (обычный пароль или Enterprise-строка).
void begin(const char* ssid, const char* stored);

// Ожидание подключения: для Enterprise-сети дольше (рукопожатие TLS медленнее обычного пароля).
unsigned long timeoutMs(unsigned long baseMs);

// ---- Экран выбора сети ------------------------------------------------------------------------------------------------

// Из цикла разбора скана: запомнить, что сеть — Enterprise (index 0 начинает новый скан).
void noteScan(int index, const char* ssid, int authMode);
bool isEnterprise(const std::string& ssid);

// Логин, введённый на первом шаге, ждёт пароля. Привязан к SSID.
bool needUsername(const std::string& ssid);
void setUser(const std::string& ssid, const std::string& user);
bool takeUserEntered();  // true один раз после setUser — пора спрашивать пароль
void clearPending();
// Результат второго шага: пароль из клавиатуры → строка для хранилища (с логином, если он был введён).
std::string finishPassword(const std::string& ssid, const std::string& pass);

}  // namespace calmod_wifi
