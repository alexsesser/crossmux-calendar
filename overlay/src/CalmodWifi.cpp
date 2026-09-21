#include "CalmodWifi.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "activities/apps/standby/CalendarConfig.h"

#if CROSSPOINT_EMULATED == 0
#if __has_include("esp_eap_client.h")
#include <esp_eap_client.h>
#else
#include <esp_wpa2.h>
#endif
#include <esp_wifi_types.h>
#endif

namespace calmod_wifi {

namespace {

constexpr int kMaxEnterprise = 6;  // сколько Enterprise-сетей помним из последнего скана
char g_ent[kMaxEnterprise][33];
int g_entCount = 0;

char g_user[65];       // логин, введённый на первом шаге
char g_userSsid[33];   // …для какой сети
bool g_userEntered = false;

bool g_driverEnterprise = false;  // Enterprise включён в драйвере (надо выключить перед обычным подключением)
bool g_connectingEnterprise = false;

void disableDriverEnterprise() {
#if CROSSPOINT_EMULATED == 0
  if (!g_driverEnterprise) return;
#if __has_include("esp_eap_client.h")
  esp_wifi_sta_enterprise_disable();
#else
  esp_wifi_sta_wpa2_ent_disable();
#endif
#endif
  g_driverEnterprise = false;
}

}  // namespace

void begin(const char* ssid, const char* stored) {
  std::string user, pass;
  if (split(stored, user, pass)) {
#if CROSSPOINT_EMULATED == 0
    const char* anon = calendar_config::kWifiEapAnonymousIdentity;
    const char* identity = (anon && anon[0]) ? anon : user.c_str();
    const wpa2_auth_method_t method = calendar_config::kWifiEapMethod == 1 ? WPA2_AUTH_TTLS : WPA2_AUTH_PEAP;
    WiFi.begin(ssid, method, identity, user.c_str(), pass.c_str());
    g_driverEnterprise = true;
#else
    // В симуляторе Enterprise нет: подключаемся как к открытой сети, а разобранные данные пишем в журнал (проверка ввода).
    std::fprintf(stderr, "[CALMOD-WIFI] EAP begin ssid='%s' user='%s' pass_len=%zu\n", ssid, user.c_str(), pass.size());
    WiFi.begin(ssid);
#endif
    g_connectingEnterprise = true;
    return;
  }
  g_connectingEnterprise = false;
  disableDriverEnterprise();
  if (stored && stored[0]) {
    WiFi.begin(ssid, stored);
  } else {
    WiFi.begin(ssid);
  }
}

unsigned long timeoutMs(unsigned long baseMs) {
  return g_connectingEnterprise ? baseMs * calendar_config::kWifiEapTimeoutFactor : baseMs;
}

void noteScan(int index, const char* ssid, int authMode) {
  if (index == 0) g_entCount = 0;
#if CROSSPOINT_EMULATED == 0
  const bool ent = authMode == WIFI_AUTH_WPA2_ENTERPRISE || authMode == WIFI_AUTH_WPA3_ENTERPRISE ||
                   authMode == WIFI_AUTH_WPA2_WPA3_ENTERPRISE || authMode == WIFI_AUTH_WPA_ENTERPRISE;
#else
  // Симулятор: CALMOD_SIM_ENTERPRISE=1 делает Enterprise-сетью тестовую (для проверки экранов ввода логина).
  const char* env = std::getenv("CALMOD_SIM_ENTERPRISE");
  const bool ent = env && env[0] == '1' && ssid && std::strstr(ssid, "Local Test");
  (void)authMode;
#endif
  if (!ent || !ssid || !ssid[0] || isEnterprise(ssid) || g_entCount >= kMaxEnterprise) return;
  strlcpy(g_ent[g_entCount++], ssid, sizeof(g_ent[0]));
}

bool isEnterprise(const std::string& ssid) {
  for (int i = 0; i < g_entCount; ++i) {
    if (ssid == g_ent[i]) return true;
  }
  return false;
}

bool needUsername(const std::string& ssid) {
  if (!isEnterprise(ssid)) return false;
  return !(g_user[0] && ssid == g_userSsid);
}

void setUser(const std::string& ssid, const std::string& user) {
  strlcpy(g_user, user.c_str(), sizeof(g_user));
  strlcpy(g_userSsid, ssid.c_str(), sizeof(g_userSsid));
  g_userEntered = true;
}

bool takeUserEntered() {
  const bool v = g_userEntered;
  g_userEntered = false;
  return v;
}

void clearPending() {
  g_user[0] = '\0';
  g_userSsid[0] = '\0';
  g_userEntered = false;
}

std::string finishPassword(const std::string& ssid, const std::string& pass) {
  std::string out = (g_user[0] && ssid == g_userSsid) ? compose(g_user, pass) : pass;
  clearPending();
  return out;
}

}  // namespace calmod_wifi
