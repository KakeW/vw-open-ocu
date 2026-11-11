# Architecture

The gateway receives Classic CAN in ESP-IDF TWAI listen-only mode. A receiver task reads the controller independently of the network task. The network task sends bounded batches to an authenticated MQTT broker over TLS. Home Assistant subscribes through its existing MQTT integration and owns capture, diagnostics and eventual vehicle-state interpretation.

ESPHome Device Builder is the primary firmware environment. The external component uses ESP-IDF TWAI and ESP-MQTT in separate tasks. ESPHome manages Wi-Fi, time synchronization, the encrypted native API and OTA. Using ESP-MQTT directly in the network worker keeps TLS I/O off ESPHome's main loop. The gateway avoids per-frame logging. The standalone ESP-IDF implementation remains in the repository and uses the same MQTT wire format. [Device Builder setup](esphome.md)

MQTT gives Home Assistant an existing integration, broker ACLs, and connection state. Custom TCP could reduce framing overhead but would require a new client and session protocol. Raw batches use QoS 0 with loss accounting and expire at the point of disconnection. Heartbeats allow the consumer to assess freshness independently of CAN bus activity.

The gateway has a YAML-configured capture mode and an exact frame allowlist for normal mode. A live Home Assistant capture session controls local recording only. Normal mode starts with an empty allowlist pending Passat evidence. Vehicle-state entities and all vehicle commands are gated on future validation. Any future command interface must require authorization, expiry, correlation, acknowledgments and observed vehicle feedback. It must exclude arbitrary remote CAN writes and stale or retained commands.
