#include "wifi_mgr.h"
#include "config.h"
#include "secrets.h"
#include <WiFi.h>
#include <time.h>

static uint32_t sBackoffMs   = 1000;
static uint32_t sNextAttempt = 0;
static uint32_t sDownSince   = 0;

bool wifiIsConnected() {
    return WiFi.status() == WL_CONNECTED;
}

int wifiRssi() {
    return wifiIsConnected() ? WiFi.RSSI() : 0;
}

bool wifiBegin(uint32_t timeoutMs) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);          // keep latency low for realtime audio
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.printf("[WIFI] connecting to \"%s\" (MAC %s)",
                  WIFI_SSID, WiFi.macAddress().c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(300);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WIFI] initial connect failed (status=%d) — will retry in the background\n",
                      (int)WiFi.status());
        return false;
    }

    Serial.printf("[WIFI] connected, IP %s, gateway %s, RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
    sBackoffMs = 1000;
    return true;
}

void wifiEnsure() {
    if (wifiIsConnected()) {
        if (sDownSince != 0) {
            Serial.printf("[WIFI] reconnected, IP %s\n", WiFi.localIP().toString().c_str());
            sDownSince = 0;
            sBackoffMs = 1000;
        }
        return;
    }

    uint32_t now = millis();
    if (sDownSince == 0) {
        sDownSince   = now;
        sNextAttempt = now + 5000;   // give the built-in auto-reconnect a chance first
        Serial.println("[WIFI] link down");
        return;
    }

    if ((int32_t)(now - sNextAttempt) < 0) {
        return;
    }

    Serial.printf("[WIFI] retrying (backoff %u ms)\n", (unsigned)sBackoffMs);
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    sNextAttempt = now + sBackoffMs;
    sBackoffMs   = (sBackoffMs * 2 > 30000) ? 30000 : sBackoffMs * 2;
}

// -----------------------------------------------------------------------------
// Clock
//
// configTzTime rather than configTime: it sets TZ as well as the servers, so
// localtime_r below returns IST directly and no offset arithmetic is done by
// hand anywhere in the firmware.
// -----------------------------------------------------------------------------
void timeSyncBegin() {
    configTzTime(NTP_TZ, NTP_SERVER_1, NTP_SERVER_2);
    Serial.println("[TIME] SNTP started (" NTP_SERVER_1 ", TZ " NTP_TZ ")");
}

bool timeIsSynced() {
    return time(nullptr) > (time_t)NTP_VALID_EPOCH;
}

bool timeWaitSync(uint32_t timeoutMs) {
    if (!wifiIsConnected()) {
        Serial.println("[TIME] no link — starting without a clock");
        return false;
    }

    const uint32_t start = millis();
    while (!timeIsSynced()) {
        if (millis() - start >= timeoutMs) {
            Serial.printf("[TIME] no answer in %u ms — starting without a clock\n",
                          (unsigned)timeoutMs);
            return false;
        }
        delay(50);
    }

    char stamp[64];
    timeNowLocal(stamp, sizeof(stamp));
    Serial.printf("[TIME] synced in %u ms: %s\n",
                  (unsigned)(millis() - start), stamp);
    return true;
}

bool timeNowLocal(char* out, size_t n) {
    if (out == nullptr || n == 0) {
        return false;
    }
    out[0] = '\0';

    const time_t now = time(nullptr);
    if (now <= (time_t)NTP_VALID_EPOCH) {
        return false;
    }

    struct tm local;
    if (localtime_r(&now, &local) == nullptr) {
        return false;
    }
    return strftime(out, n, TIME_PROMPT_FORMAT, &local) > 0;
}
