import json
from struct import pack
import unittest
from unittest.mock import patch

from custom_components.vw_open_ocu.frames import Frame
from custom_components.vw_open_ocu.runtime import Gateway, STALE_SECONDS
from custom_components.vw_open_ocu.wire import COUNTERS, decode_batch


SESSION = "0102030405060708"


def status(session=SESSION, **updates):
    value = dict.fromkeys(COUNTERS, 0)
    value.update(version=1, session=session, capture_mode=True, queue_capacity=256)
    value.update(updates)
    return json.dumps(value).encode()


def frame(sequence=0, flags_id=0x123, dlc=1, data=b"\x01"):
    return pack(">QQIB", sequence, sequence * 10, flags_id, dlc) + data


def batch(*records, session=SESSION):
    return b"OCU\x01" + bytes.fromhex(session) + b"".join(records)


class WireTests(unittest.TestCase):
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
            self.assertEqual(gateway.sequence_gaps, 0)
            gateway.ingest(batch(frame(9)))
            self.assertIsNone(gateway.last_sequence)

    def test_capture_capacity_marker_and_exports(self):
        gateway = Gateway()
        gateway.heartbeat(status())
        gateway.start_capture()
        with self.assertRaises(ValueError):
            gateway.mark("too early")
        with patch("custom_components.vw_open_ocu.runtime.CAPTURE_LIMIT", 3):
            gateway.ingest(batch(frame()))
            gateway.mark("bench")
            gateway.ingest(batch(frame(1), frame(2)))
        gateway.recording = False
        self.assertEqual(gateway.capture_discarded, 1)
        self.assertIn("bench", gateway.export("csv"))
        self.assertEqual(gateway.export("candump"), "(0.000000) can0 123#01\n(0.000010) can0 123#01\n")
