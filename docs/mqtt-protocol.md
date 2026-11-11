# MQTT protocol v1

For gateway ID `bench`, the publisher writes `vw_open_ocu/bench/v1/status` and `vw_open_ocu/bench/v1/frames`. Gateway IDs use lowercase ASCII letters, digits, hyphens and underscores, start with a letter or digit, and are at most 32 characters. Assign a unique ID and broker credential to each gateway. Give that credential write access to its two topics only. Give Home Assistant read access. Use MQTT over TLS with hostname verification and a trusted CA.

Both topics use QoS 0 and nonretained messages. The firmware sends a status update on connection and every five seconds while connected. Home Assistant ignores retained messages and considers the gateway offline when the most recent valid heartbeat is 16 seconds old. Its display is refreshed at least every two seconds. This describes gateway connectivity even while the CAN bus is silent.

The JSON status message includes integer `version` (1), `session` (16 lowercase hex digits identifying this boot), boolean `capture_mode`, and unsigned counters `queue_depth`, `queue_capacity`, `queue_peak`, `queue_dropped`, `transport_dropped`, `filtered`, `unsupported_dlc`, `driver_missed`, `driver_overrun`, `bus_errors`, `rx_errors`, and `controller_state`. Driver fields mirror ESP-IDF TWAI status. Queue peak is a sampled high-water mark. Gateway counters are cumulative since boot except for queue depth and the controller's current error/state fields.

New firmware adds two optional v1 heartbeat fields. `connection_epoch` is an unsigned counter that changes on Wi-Fi loss and MQTT connection transitions. Treat a changed value as a discontinuity. `last_published_sequence` is the last sequence in a batch whose local QoS 0 publish call succeeded, or JSON null before the first such batch. Local success does not confirm broker delivery. Older v1 heartbeats remain readable and lack these checks.

Binary batches contain ASCII `OCU` and byte `01`, then an eight-byte session ID in big-endian order. One to 48 frame records follow, with no padding:

| Field | Bytes | Meaning |
| --- | --- | --- |
| Sequence | 8 | Boot-local sequence before enqueue, after software filtering |
| Timestamp | 8 | Monotonic microseconds at TWAI driver dequeue |
| Flags and ID | 4 | Bit 31 extended, bit 30 remote, bit 29 reserved zero, bits 28–0 CAN ID |
| DLC | 1 | Classic CAN length, 0 through 8 |
| Payload | 0–8 | Exactly DLC bytes for data frames, zero bytes for remote frames |

Integers are big-endian. Timestamps measure software dequeue with microsecond resolution, so queueing affects timing accuracy. Unsupported DLCs are counted separately and discarded. Driver misses happen before sequence assignment. Home Assistant counts gaps between accepted frames, including loss it observes across reconnects within the same boot. Gaps and gateway counters overlap and must not be summed. QoS 0 can lose the last batch before a disconnect without a subsequent sequence to expose the loss. On reconnect the firmware discards queued data before resuming.

The gateway subscribes to no MQTT topics. Home Assistant capture controls operate on local memory. There is no CAN transmit or wake endpoint.

## Capture integrity

Home Assistant appends accepted frames in sequence order, rejects duplicate or decreasing positions, and retains sequence as the tie-breaker when timestamps match. CSV exports retain microseconds and sequence numbers. The export response adds `metadata` with capture endpoints, stop reason, in-window sequence gaps, rejected-message counts, and the start and end heartbeat snapshots. The recorder stops on a gateway reboot, a reported connection epoch change, an HA MQTT disconnect, or heartbeat expiry. A status snapshot and the frame stream use separate MQTT topics, so there is no shared end-of-capture barrier.

Gateway loss-counter deltas describe the interval between the two heartbeat samples. It can include traffic just before the capture and omit traffic after the last sample. The export uses null if the samples are the same, or for an individual counter that moved backwards. A positive `end_heartbeat_publish_ahead_by` means the end heartbeat reports a sequence beyond the last received capture frame. That difference is a diagnostic hint, since late delivery, skipped sequences and sampling boundaries can contribute. Zero cannot rule out loss after the last sample. See the [vehicle capture workflow](vehicle-capture.md) for run acceptance checks.
