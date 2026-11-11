# Development and bench setup

## Offline analyzer

From the checkout root:

```sh
python3 -m unittest discover -s tests -v
python3 -m tools.can_analyzer examples/synthetic.csv --bap-id 1ABCDE --event-window-us 5000
python3 -m tools.can_analyzer captures/baseline.csv --compare captures/action.log
```

Inputs are Classic CAN CSV and compact timestamped, single-interface candump logs. Use `--id STD:123` or `--id EXT:00000123` to filter. The reported frequency uses count minus one divided by the span from first to last frame for that identifier. `--export` uses CSV for a `.csv` destination and candump otherwise. CSV preserves event markers and sequence numbers. Candump preserves CAN ID type and frame data but omits capture markers and sequence numbers.

## Configure and build the gateway

1. Use the bench setup in [Hardware and safety](hardware.md), with GPIO0 physically disconnected from the transceiver input and that input held recessive. Do not connect the bench unit to the vehicle.
2. Configure an MQTT TLS listener and credentials. Restrict gateway writes to its two [topics](mqtt-protocol.md). Configure Home Assistant's MQTT integration on that broker with its own credentials and permission to read the gateway topics. Make an SNTP server available to the ESP32 and use a broker hostname that matches its TLS certificate.
3. Copy `firmware/esp32-c3/main/gateway_config.example.h` to `gateway_config.h` in the same directory. Enter Wi-Fi and broker settings, the gateway ID, SNTP server, and a private CA PEM when applicable. Set `OCU_CAPTURE_MODE` to `1` for the bench. The local header is ignored by Git. Treat the compiled firmware as sensitive because it contains credentials.
4. In an activated ESP-IDF v5.5.2 environment, run:

   ```sh
   cd firmware/esp32-c3
   idf.py set-target esp32c3
   idf.py -p /dev/cu.usbmodemXXXX build flash monitor
   ```

   Replace the serial port with the one enumerated by the board. Flashing replaces the existing ESPHome app. Keep the original configuration and a flashable copy if you need to restore it.

## Configure Home Assistant

1. Copy `custom_components/vw_open_ocu` into the HA configuration directory's `custom_components` folder and restart Home Assistant.
2. Under **Settings → Devices & services → Add integration**, choose **VW Open OCU**. Enter the gateway ID from the header, for example `bench`.
3. Check the device's Connectivity and Last gateway update entities before feeding bench frames. Verify controller and loss diagnostics as traffic starts. There are no vehicle-state values until they have been validated on the Passat.
4. Under **Developer Tools → Actions**, call `vw_open_ocu.capture_start` with `gateway_id: bench`. Feed frames, optionally call `vw_open_ocu.capture_mark` with the gateway ID and a name, then call `vw_open_ocu.capture_stop`. Call `vw_open_ocu.capture_export` with the gateway ID and `format: csv` or `candump`; save the response's `data` string. Compare the result with the bench generator's log.

The recorder stores the first 10,000 records per capture in RAM, including markers. Check the discarded count in the export. Starting a new capture replaces the previous one. A gateway reboot stops the recording so monotonic clocks are never mixed. Markers use the most recently captured frame's timestamp and can lag behind the action being marked. Restarting or unloading HA discards the recording. Enabling firmware capture mode streams continuously while connected, so use it only for diagnostics. Normal mode has an empty allowlist by default.

## Bench validation still required

- Send standard and extended data and remote frames with known frame counts and DLCs. Compare the generator log against exported records and measure queue timestamp error.
- Ramp traffic up to the expected peak, recording frames per second, driver misses, queue drops, transport drops, sequence gaps, and peak depth. Repeat while HA is offline and while Wi-Fi and the broker are disconnected and restored.
- Confirm no stale raw batches appear after reconnect, TLS rejects bad trust or hostname, and counters reflect the tested loss modes. Verify listen-only operation electrically during normal operation and faults.
- Validate the wiring, interface protection, current draw and sleep behavior separately before any on-vehicle capture.

## Software verification

GitHub Actions runs the offline tests, compiles the firmware for ESP32-C3, and runs `tests/ha_smoke.py` against Home Assistant 2026.10.0 and an isolated Mosquitto listener. The smoke test uses synthetic MQTT frames and loopback TCP. The bench procedure covers gateway TLS and physical CAN behavior.
