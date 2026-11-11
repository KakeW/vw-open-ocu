import json
from io import StringIO
from struct import pack
import unittest
from unittest.mock import patch

from custom_components.vw_open_ocu.capture import Marker, read_csv
from custom_components.vw_open_ocu.frames import Frame
from custom_components.vw_open_ocu.runtime import Gateway, STALE_SECONDS
from custom_components.vw_open_ocu.wire import COUNTERS, decode_batch, decode_status


SESSION = "0102030405060708"


def status(session=SESSION, **updates):
    value = dict.fromkeys(COUNTERS, 0)
    value.update(version=1, session=session, capture_mode=True, queue_capacity=256)
    value.update(updates)
    return json.dumps(value).encode()


def frame(sequence=0, flags_id=0x123, dlc=1, data=b"\x01", timestamp=None):
    return pack(">QQIB", sequence, sequence * 10 if timestamp is None else timestamp,
                flags_id, dlc) + data


def batch(*records, session=SESSION):
    return b"OCU\x01" + bytes.fromhex(session) + b"".join(records)


class WireTests(unittest.TestCase):
    def test_optional_heartbeat_continuity_fields(self):
        self.assertNotIn("connection_epoch", decode_status(status()))
        self.assertIsNone(decode_status(status(last_published_sequence=None))["last_published_sequence"])
        self.assertEqual(decode_status(status(connection_epoch=3))["connection_epoch"], 3)
        for payload in [status(connection_epoch=True), status(connection_epoch=None),
                        status(last_published_sequence=2**64)]:
            with self.assertRaises(ValueError):
                decode_status(payload)

    def test_batch_preserves_types_dlc_and_payload(self):
        packet = batch(frame(), frame(2, 0xC0000123, 8, b""))
        self.assertEqual(decode_batch(packet), (SESSION, [
            Frame(0, 0x123, False, b"\x01", 1, False, 0),
            Frame(20, 0x123, True, b"", 8, True, 2),
        ]))

    def test_rejects_wrong_version_truncation_reserved_flags_and_duplicate_sequence(self):
        for packet in [b"OCU\x02" + batch(frame())[4:], batch(frame())[:-1],
                       batch(frame(flags_id=0x20000123)), batch(frame(), frame()),
                       batch(frame(dlc=9)), batch()]:
            with self.subTest(packet=packet), self.assertRaises(ValueError):
                decode_batch(packet)


class GatewayTests(unittest.TestCase):
    def test_start_requires_fresh_capture_enabled_status(self):
        gateway = Gateway()
        with self.assertRaises(ValueError):
            gateway.start_capture()
        self.assertFalse(gateway.heartbeat(status(queue_depth=True)))
        self.assertTrue(gateway.heartbeat(status(capture_mode=False)))
        with self.assertRaises(ValueError):
            gateway.start_capture()

    def test_session_and_freshness(self):
        gateway = Gateway()
        with patch("custom_components.vw_open_ocu.runtime.monotonic", return_value=1):
            gateway.heartbeat(status())
            gateway.start_capture()
            gateway.ingest(batch(frame(4)))
            gateway.ingest(batch(frame(7)))
            gateway.ingest(batch(frame(7)))
            self.assertEqual(gateway.sequence_gaps, 2)
            self.assertEqual(len(gateway.records), 2)
        with patch("custom_components.vw_open_ocu.runtime.monotonic", return_value=1 + STALE_SECONDS):
            self.assertFalse(gateway.online)
            gateway.ingest(batch(frame(8)))
            self.assertEqual(len(gateway.records), 2)
            gateway.heartbeat(status("0000000000000001"))
            self.assertFalse(gateway.recording)
            self.assertEqual(gateway.capture_metadata()["stop_reason"], "heartbeat_timeout")
            self.assertEqual(gateway.sequence_gaps, 0)
            gateway.ingest(batch(frame(9)))
            self.assertIsNone(gateway.last_sequence)

    def test_capture_capacity_marker_and_exports(self):
        gateway = Gateway()
        gateway.heartbeat(status())
        gateway.start_capture(max_records=3)
        with self.assertRaises(ValueError):
            gateway.mark("too early")
        gateway.ingest(batch(frame()))
        gateway.mark("bench")
        gateway.ingest(batch(frame(1), frame(2)))
        gateway.stop_capture()
        self.assertEqual(gateway.capture_discarded, 1)
        self.assertIn("bench", gateway.export("csv"))
        self.assertEqual(gateway.export("candump"), "(0.000000) can0 123#01\n(0.000010) can0 123#01\n")

    def test_capture_preserves_ties_order_and_marker_positions(self):
        gateway = Gateway()
        gateway.heartbeat(status(connection_epoch=1, last_published_sequence=3))
        gateway.ingest(batch(frame(3)))
        gateway.start_capture()
        gateway.ingest(batch(frame(10, timestamp=100)))
        gateway.mark("HEATING_ON_REQUEST")
        gateway.ingest(batch(frame(10, timestamp=100)))  # Duplicate batch.
        gateway.ingest(batch(frame(11, timestamp=99)))   # Decreasing time across batches.
        gateway.ingest(batch(frame(12, 0x80000123, timestamp=100)))
        gateway.ingest(b"invalid")
        gateway.ingest(batch(frame(13), session="0000000000000001"))
        gateway.heartbeat(status(connection_epoch=1, queue_dropped=2, last_published_sequence=14))
        gateway.stop_capture()
        records = list(read_csv(StringIO(gateway.export("csv"))))
        self.assertEqual(records, [
            Frame(100, 0x123, False, b"\x01", sequence=10),
            Marker(100, "HEATING_ON_REQUEST"),
            Frame(100, 0x123, True, b"\x01", sequence=12),
        ])
        meta = gateway.capture_metadata()
        self.assertEqual(meta["sequence_gaps_between_received_frames"], 1)
        self.assertEqual(meta["duplicate_or_out_of_order_frames"], 2)
        self.assertEqual(meta["invalid_messages"], 1)
        self.assertEqual(meta["other_session_batches"], 1)
        self.assertEqual(meta["heartbeat_counter_deltas"]["queue_dropped"], 2)
        self.assertEqual(meta["end_heartbeat_publish_ahead_by"], 2)
        self.assertEqual(meta["markers"][0]["previous_received_frame"]["sequence"], 10)
        self.assertEqual(meta["markers"][0]["next_received_frame"]["sequence"], 12)

    def test_reconnect_and_reboot_stop_capture_without_mixing_sessions(self):
        gateway = Gateway()
        gateway.heartbeat(status(connection_epoch=1, driver_missed=2))
        gateway.start_capture()
        gateway.ingest(batch(frame()))
        gateway.mark("last marker")
        gateway.heartbeat(status(connection_epoch=4, driver_missed=1))
        meta = gateway.capture_metadata()
        self.assertEqual(meta["stop_reason"], "gateway_connection_changed")
        self.assertIsNone(meta["heartbeat_counter_deltas"]["driver_missed"])
        self.assertIsNone(meta["markers"][0]["next_received_frame"])
        gateway.start_capture()
        gateway.heartbeat(status("0000000000000001", connection_epoch=1))
        meta = gateway.capture_metadata()
        self.assertEqual(meta["stop_reason"], "gateway_reboot")
        self.assertEqual(meta["end_heartbeat"]["status"]["session"], SESSION)
        self.assertIsNone(meta["heartbeat_counter_deltas"])

    def test_broker_disconnect_requires_new_heartbeat(self):
        gateway = Gateway()
        gateway.heartbeat(status())
        gateway.start_capture()
        gateway.broker_disconnected()
        self.assertFalse(gateway.online)
        self.assertEqual(gateway.capture_metadata()["stop_reason"], "ha_mqtt_disconnected")
        with self.assertRaises(ValueError):
            gateway.start_capture()
        gateway.heartbeat(status())
        gateway.start_capture()
        gateway.stop_capture()
        self.assertIsNone(gateway.capture_metadata()["heartbeat_counter_deltas"])
