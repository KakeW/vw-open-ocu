# Passat GTE MY2018 preconditioning capture

The first experiment compares baseline convenience CAN traffic with manual preconditioning activation and deactivation. Keep the original modules connected and use the vehicle's existing controls for both actions. The gateway stays receive-only throughout.

## Prepare the tap and recorder

1. Complete the [bench checks](development.md#bench-validation-still-required) with a known frame source. Confirm the gateway remains electrically silent and determine a capture length that fits at the measured frame rate. Test a deliberate MQTT interruption on the bench and confirm that the capture stops with a reason.
2. Verify the [J255 or J605 connection](hardware.md#passat-gte-my2018-convenience-can-access) against the vehicle diagram and the actual connector. Fit the verified pass-through branch following the vehicle service procedure. Before powering the vehicle for recording, reconnect all original modules, keep the receiver in listen-only mode and keep the transmitter input physically held recessive. Use the protected interface described in the hardware notes.
3. Use current gateway firmware in capture mode and confirm HA is receiving fresh heartbeats. Park in a ventilated location, keep the vehicle stationary and record the initial conditions. Note ignition and wake state, charging connection, initial climate state, and the control used to request heating. Keep these conditions consistent between runs where possible.

## Record baseline and transitions

1. With preconditioning off, start `vw_open_ocu.capture_start` using the gateway ID and, if needed, an appropriate `max_records`. Record a baseline with no user action. Use a comparable recording duration and wake state for each run. Wait for a new heartbeat before stopping where capacity permits. Export with `format: csv` and save both the `data` string and the response metadata in the ignored `captures/` directory.
2. Start another capture with preconditioning off and leave a lead-in before touching the vehicle controls. Call `capture_mark` with `HEATING_ON_REQUEST` near the manual action, then activate preconditioning through the original control. Note the order of the action and marker calls and whether the vehicle indicated that preconditioning actually started. A separate `HEATING_ON_CONFIRMED` marker can record an observed indication. Keep recording through the response and wait for a new heartbeat before stopping if the buffer allows it.
3. With preconditioning active, repeat for manual deactivation using `HEATING_OFF_REQUEST` and, if observed, `HEATING_OFF_CONFIRMED`. Use separate short captures if the complete sequence would fill the recorder. Export each run before starting another. Repeat the baseline and transitions to separate repeatable changes from ordinary wake or user-interface activity.

A CSV marker uses the previous recorded frame's gateway timestamp. The metadata also records the HA action time in UTC and the frame positions seen immediately before and after the marker request. These positions reflect arrival at HA and may include buffered data. They do not measure the exact instant of the vehicle action. Choose an analysis window that includes the manual delay and batching latency. Compare relative positions within each capture because gateway timestamps are monotonic since boot.

## Check and compare

- Confirm a manual stop, nonempty lead-in and follow-up, zero local discards, no in-window sequence gaps or rejected data, and compatible start/end heartbeat snapshots. Inspect increases in driver misses, overruns, queue drops, transport drops, unsupported DLC and bus errors separately. The counters overlap with sequence gaps and must not be added together. Repeat interrupted, truncated or visibly lossy captures.
- Check that the end heartbeat was received after the action and that its publish sequence is consistent with the last received frame. A missing or old snapshot leaves tail coverage uncertain. A clean report cannot prove lossless delivery because the transport uses QoS 0 and the controller timestamps at software dequeue.
- Compare IDs, byte values and frequency, then examine candidate traffic near the markers. For example, a two-second radius is expressed as 2,000,000 microseconds. Adjust it to fit the observed timing and the recorded lead-in and follow-up.

```sh
python3 -m tools.can_analyzer captures/baseline.csv --compare captures/heating-on.csv
python3 -m tools.can_analyzer captures/heating-on.csv --event-window-us 2000000
python3 -m tools.can_analyzer captures/heating-off.csv --event-window-us 2000000
```

Record candidate identifiers with the observed conditions and repeated ON/OFF results. Keep raw traces and export metadata out of Git. A correlation supports further decoding and does not establish a command or payload definition.
