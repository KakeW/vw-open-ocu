# ESPHome Device Builder

Device Builder is the primary environment for configuring, building and updating the gateway. The example uses ESPHome 2026.9.1, the ESP32-C3 ESP-IDF 5.5.5 toolchain, encrypted native API and encrypted native OTA. The ESPHome API handles management. CAN frames still go to the existing Home Assistant integration through the versioned MQTT/TLS topics. [Device Builder](https://esphome.io/guides/getting_started_hassio/) · [MQTT protocol](mqtt-protocol.md)

## Configure the device

1. Add a device YAML named `vw-open-ocu.yaml` in Device Builder and paste the [complete example](../firmware/esphome/vw-open-ocu.yaml). For the local source in that example, copy this repository's `components/vw_open_ocu/` into `components/vw_open_ocu/` beside the device YAML using file access to the Home Assistant host. The builder must see the component under its own configuration directory. Keep that directory at a reviewed revision. For a private Git source instead, replace the `external_components` block as shown in the [private source runbook](remote-ota.md#private-external-component-source).
2. In Device Builder's secrets editor, add the keys in [secrets.example.yaml](../firmware/esphome/secrets.example.yaml). Use a unique base64-encoded 32-byte API/OTA key and separate MQTT credentials. The broker URI must use `mqtts://`; for a private CA add the PEM as a multiline secret and uncomment `ca_certificate`. Configure the device to reach a time source for TLS certificate validation. Protect YAML secrets, builder backups and built images.
3. Match `gateway_id` to the existing VW Open OCU integration entry. Use `capture_mode: true` for supervised bench or vehicle recording. The example uses `false` and an empty allowlist for normal operation. For selective receive, populate the allowlist with exact `can_id` and `extended` values. Keep the specified RX pin and leave the placeholder TX GPIO physically disconnected from the transceiver. [Hardware notes](hardware.md)
4. Choose **Validate** and **Install** in Device Builder. If a compatible ESPHome OTA service is already installed, preserve the device name, address and existing OTA credentials for the first upload. For an older password-only device, keep its existing OTA configuration in a transitional YAML, then move to the encrypted-only example once a compatible image is reachable. If the board runs the standalone ESP-IDF firmware or has no reachable OTA service, use Device Builder to build the factory-format image and install it once over USB from a supported browser or directly from the host if the serial device is accessible. [ESPHome installation](https://esphome.io/guides/getting_started_hassio/) · [OTA migration and recovery](remote-ota.md#first-flash-and-recovery)
5. Verify the ESPHome API, the VW Open OCU heartbeat and passive bench frames. Use **Install → Wirelessly** for subsequent changes. Over a routed VPN set `wifi.use_address` to the address reachable from the builder. Keep a known-good configuration and a protected copy of its factory and OTA images. [Remote OTA runbook](remote-ota.md#remote-access-behind-mobile-cgnat)

## Diagnose MQTT/TLS from the native API

The example YAML enables optional gateway diagnostic entities under `vw_open_ocu`. If you already have a device YAML, copy the diagnostic fields from the [example](../firmware/esphome/vw-open-ocu.yaml) before rebuilding. Find the entities on the **ESPHome** device page and use Device Builder's wireless logs. These diagnostics are available while the native API works, including when MQTT cannot connect.

| What you see | What to check |
| --- | --- |
| **Wi-Fi IPv4 Ready** is off | The `vw_open_ocu` log distinguishes a missing station interface, a down interface, and a station without an IPv4 address. Verify the station has an address and a route to the broker. |
| **Clock Ready for TLS** is off, connection attempts stay at zero | MQTT has not started. Verify that the ESP32 can reach the configured time source. For SNTP, check DNS and access to the selected NTP server. You can use a reachable local SNTP server or ESPHome's [Home Assistant time source](https://esphome.io/components/time/homeassistant/) over the encrypted API. Clock readiness checks for initialized system time and TLS still validates certificate dates. |
| **MQTT State** is connecting and **Connection Attempts** increases | Read **Last MQTT Error**. A TCP error includes the ESP error and socket errno. A TLS error includes the SDK error, TLS stack error, and certificate verification flags. For a private CA, verify that the running YAML includes `ca_certificate: !secret ocu_mqtt_ca_certificate`. Match the URI host to the certificate identity. |
| **Last MQTT Error** reports authentication or authorization refused | The broker returned an MQTT refusal. Check the gateway's credentials and the listener's authentication policy. The error shows the numeric refusal code. |
| **MQTT Connected** is on and **Heartbeat Failures** increases | Inspect the error for a TWAI status read, payload encoding, or local publish failure. The worker retries the heartbeat before resuming frames. |
| **Heartbeats Sent** increases and HA still sees no status | The local QoS 0 publish succeeded. Subscribe on the same broker to `vw_open_ocu/<gateway_id>/v1/status` and check the gateway topic, publisher write ACL and HA read ACL. Local success gives no broker receipt acknowledgment. |

The logs are rate-limited and report stage, event counts, and numeric errors. The error entity keeps the latest failure even after a successful reconnect. Counters reset when the ESP32 reboots. Logs omit broker credentials, CA contents, and frame payloads. TLS certificate and broker identity verification stay enabled. [ESP-MQTT error fields](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-reference/protocols/mqtt.html)

### MQTT initialization returns null

ESP-IDF 5.5.5's `esp_mqtt_client_init` can return null from four places:

- Allocation of the client object fails.
- `create_client_data` fails to allocate the error record, recursive mutex, outbox, or event group.
- `esp_mqtt_set_config` fails while allocating its configuration, I/O buffers, or copies of strings and optional fields, parsing the URI, selecting an unsupported MQTT protocol, or checking for conflicting settings.
- Allocation of MQTT 5 default properties fails when MQTT 5 support is compiled in. The example build uses MQTT 3.1.1. The initializer also calls `esp_event_loop_create` without checking its return value. Event registration is checked separately by the gateway.

The original gateway supplied both an `mqtts://` URI and an explicit SSL transport. ESP-MQTT's conflict check returns `ESP_ERR_INVALID_ARG` for that combination. The URI alone selects TLS, including verification of the broker identity. A null return happens before the MQTT task starts or connects. The SDK parses the CA at TLS setup, and allocates the MQTT task stack in `esp_mqtt_client_start`. A broker handshake or a task stack allocation failure therefore belongs to a later stage. [Exact ESP-MQTT source bundled with ESP-IDF 5.5.5](https://github.com/espressif/esp-mqtt/blob/0e9ec171b448db2101ea537300976b7ffe595941/mqtt_client.c)

The first attempt logs the configured field lengths and heap snapshot. On a null result, **Last MQTT Error** and the rate-limited gateway log include free 8-bit heap, largest contiguous 8-bit block, free internal 8-bit heap, and largest contiguous internal 8-bit block, measured immediately before the attempt. The SDK returns no error code from this API. Set `esp32.framework.log_level: WARN` to see its warning and error messages in Device Builder's wireless logs. The example uses that setting and leaves serial application logging disabled. Keep credentials in separate YAML secrets. The CA field accepts NUL-terminated PEM via the component's stored string and remains available to ESP-MQTT for the client's lifetime.

## What happens during an update

The component subscribes to ESPHome's OTA lifecycle. On upload start it signals the receive task to stop TWAI and the network worker to stop its MQTT client and discard queued frames. These operations run outside ESPHome's main loop. On upload error or abort, the tasks resume with a new connection epoch. A successful update keeps capture paused until reboot. MQTT continuity changes end an active HA recording. Stop and export important captures before updating. [ESPHome OTA lifecycle](https://esphome.io/components/ota/)

The example enables safe mode and exposes **Reboot into OTA safe mode** through the encrypted native API. Safe mode starts the network and OTA service without loading the capture component. The ESP32 build keeps ESPHome's default rollback support. Bench-test an aborted OTA, a normal OTA and recovery from safe mode before relying on a remote update. [ESPHome safe mode](https://esphome.io/components/safe_mode/) · [ESP32 rollback](https://esphome.io/components/esp32/#advanced-options)

## Firmware boundary

The component owns one ESP32-C3 TWAI controller and requires ESP-IDF 5.5.x. ESPHome owns Wi-Fi, SNTP, API and OTA. The ESP-IDF MQTT client is used by a dedicated worker so a blocked TLS write stays outside the ESPHome loop. The receive task has a bounded driver queue and a bounded handoff queue. It yields periodically when the bus keeps the driver queue busy. The gateway retains controller counters and timestamps frames when it dequeues them. It sends no CAN frames and logs no per-frame messages.

The ESP-IDF legacy driver preserves separate receive-miss and overrun counters. It requires a placeholder TX pin at driver installation. Before starting the controller the component detaches the pin from the TWAI output matrix and sets it to input mode. The transceiver's TXD must remain physically disconnected from the ESP32 and held recessive. [ESP-IDF 5.5.5 TWAI driver](https://github.com/espressif/esp-idf/blob/v5.5.5/components/driver/twai/twai.c)

The [standalone ESP-IDF firmware](../firmware/esp32-c3) remains available for reference and recovery with its original build procedure. It has no OTA service and requires a serial installation to migrate from it to ESPHome.
