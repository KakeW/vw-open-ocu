# Architecture

The gateway receives Classic CAN in ESP-IDF TWAI listen-only mode. A receiver task reads the controller independently of the network task. The network task sends bounded batches to an authenticated MQTT broker over TLS. Home Assistant subscribes through its existing MQTT integration and owns capture, diagnostics and eventual vehicle-state interpretation.

Native ESP-IDF is the first implementation because it exposes TWAI queue and controller counters directly and lets the receiver run independently of the ESPHome loop and logger. This is an engineering choice pending measurement. An ESPHome external component remains possible using the same wire protocol. The brief reports watchdog resets under verbose USB logging. The gateway avoids per-frame logging.

MQTT gives Home Assistant an existing integration, broker ACLs, and connection state. Custom TCP could reduce framing overhead but would require a new client and session protocol. Raw batches use QoS 0 with loss accounting and expire at the point of disconnection. Heartbeats allow the consumer to assess freshness independently of CAN bus activity.

The gateway has a compile-time capture mode and an exact frame allowlist for normal mode. A live Home Assistant capture session controls local recording only. Normal mode starts with an empty allowlist pending Passat evidence. Vehicle-state entities and all vehicle commands are gated on future validation. Any future command interface must require authorization, expiry, correlation, acknowledgments and observed vehicle feedback. It must exclude arbitrary remote CAN writes and stale or retained commands.
