#pragma once

#define OCU_WIFI_SSID "replace-me"
#define OCU_WIFI_PASSWORD "replace-me"
#define OCU_MQTT_URI "mqtts://broker.example.net:8883"
#define OCU_MQTT_USERNAME "replace-me"
#define OCU_MQTT_PASSWORD "replace-me"
#define OCU_GATEWAY_ID "bench"
#define OCU_SNTP_SERVER "pool.ntp.org"

// Set to a PEM string literal for a private CA. NULL uses the ESP-IDF CA bundle.
#define OCU_MQTT_CA_PEM NULL

// 1 forwards all Classic CAN frames. 0 forwards only the allowlist.
#define OCU_CAPTURE_MODE 0
// Add entries as: , { .id = 0x123, .extended = false }
#define OCU_ALLOWLIST
