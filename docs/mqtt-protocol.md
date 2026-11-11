# MQTT protocol v1

For gateway ID `bench`, the publisher writes `vw_open_ocu/bench/v1/status` and `vw_open_ocu/bench/v1/frames`. Gateway IDs use lowercase ASCII letters, digits, hyphens and underscores, start with a letter or digit, and are at most 32 characters. Assign a unique ID and broker credential to each gateway. Give that credential write access to its two topics only. Give Home Assistant read access. Use MQTT over TLS with hostname verification and a trusted CA.

Both topics use QoS 0 and nonretained messages. The firmware sends a status update on connection and every five seconds while connected. Home Assistant ignores retained messages and considers the gateway offline when the most recent valid heartbeat is 16 seconds old. Its display is refreshed at least every two seconds. This describes gateway connectivity even while the CAN bus is silent.

The JSON status message includes integer `version` (1), `session` (16 lowercase hex digits identifying this boot), boolean `capture_mode`, and unsigned counters `queue_depth`, `queue_capacity`, `queue_peak`, `queue_dropped`, `transport_dropped`, `filtered`, `unsupported_dlc`, `driver_missed`, `driver_overrun`, `bus_errors`, `rx_errors`, and `controller_state`. Driver fields mirror ESP-IDF TWAI status. Queue peak is a sampled high-water mark. Gateway counters are cumulative since boot except for queue depth and the controller's current error/state fields.

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
