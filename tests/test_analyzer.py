from io import StringIO
from pathlib import Path
import unittest

from custom_components.vw_open_ocu.bap import Assembler
from custom_components.vw_open_ocu.capture import Marker, read_candump, read_csv, write_candump, write_csv
from custom_components.vw_open_ocu.frames import Frame
from tools.can_analyzer.__main__ import compare, load, summarize


class CaptureTests(unittest.TestCase):
    def test_round_trip(self):
        frames = [Frame(1, 0x123, False, b""), Frame(2, 0x123, True, b"\x01"),
                  Frame(3, 0x456, False, b"", 8, True)]
        output = StringIO()
        write_csv([*frames, Marker(3, "test")], output)
        self.assertEqual(list(read_csv(StringIO(output.getvalue()))), [*frames, Marker(3, "test")])
        output = StringIO()
        write_candump(frames, output)
        self.assertEqual(list(read_candump(StringIO(output.getvalue()))), frames)

    def test_reject_fd_and_invalid_standard_identifier(self):
        for line in ["(0.1) can0 123##011", "(0.1) can0 ABC#11"]:
            with self.assertRaises(ValueError):
                list(read_candump(StringIO(line)))

    def test_comparison(self):
        frames = [frame for frame in load(Path("examples/synthetic.csv")) if isinstance(frame, Frame)]
        baseline = summarize(frames[:1])
        action = summarize(frames)
        self.assertEqual(action["STD:123"]["frequency_hz"], 50)
        self.assertEqual(action["STD:123"]["changing_bytes"], [0])
        self.assertEqual(compare(baseline, action)["new"], ["EXT:001ABCDE"])


class BapTests(unittest.TestCase):
    def test_reassembly_and_filtering(self):
        assembler = Assembler({0x1ABCDE})
        self.assertIsNone(assembler.feed(Frame(1, 0x1ABCDF, True, bytes.fromhex("3141"))))
        for frame in load(Path("examples/synthetic.csv")):
            if isinstance(frame, Frame) and (message := assembler.feed(frame)):
                self.assertEqual((message.operation, message.logical_device, message.function), (3, 5, 1))
                self.assertEqual(message.payload, bytes(range(1, 6)))
                break
        else:
            self.fail("missing synthetic BAP message")

    def test_out_of_order_and_timeout(self):
        assembler = Assembler({123}, timeout_us=20)
        for ts, data in [(0, "8003314101"), (1, "C102"), (2, "C00203"),
                         (3, "8003314101"), (24, "C00203")]:
            self.assertIsNone(assembler.feed(Frame(ts, 123, True, bytes.fromhex(data))))

    def test_interleaving_and_index_wrap(self):
        assembler = Assembler({123})
        for group in (0, 1):
            self.assertIsNone(assembler.feed(Frame(0, 123, True, bytes([0x80 | group << 4, 18, 0x31, 0x41]))))
        for index in range(18):
            for group in (1, 0):
                result = assembler.feed(Frame(index + 1, 123, True,
                                              bytes([0xC0 | group << 4 | (index % 16), group])))
                if index < 17:
                    self.assertIsNone(result)
                else:
                    self.assertEqual(result.payload, bytes([group]) * 18)
