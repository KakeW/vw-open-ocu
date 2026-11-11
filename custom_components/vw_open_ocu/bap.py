"""Candidate receive-only BAP framing. See docs/can-protocol.md."""

from dataclasses import dataclass

from .frames import Frame


@dataclass(frozen=True, slots=True)
class Message:
    timestamp_us: int
    can_id: int
    operation: int
    logical_device: int
    function: int
    payload: bytes


@dataclass(slots=True)
class Pending:
    started_us: int
    header: int
    size: int
    payload: bytearray
    index: int = 0


class Assembler:
    def __init__(self, channels: set[int], timeout_us: int = 1_000_000) -> None:
        self.channels = frozenset(channels)
        self.timeout_us = timeout_us
        self.pending: dict[tuple[int, int], Pending] = {}

    def feed(self, frame: Frame) -> Message | None:
        data = frame.data
        if not frame.extended or frame.remote or frame.can_id not in self.channels or not data:
            return None
        if not data[0] & 0x80:
            if len(data) < 2:
                return None
            return self._message(frame, int.from_bytes(data[:2], "big"), data[2:])

        group = (data[0] >> 4) & 3
        index = data[0] & 15
        key = (frame.can_id, group)
        pending = self.pending.pop(key, None)
        if not data[0] & 0x40:
            if len(data) < 4 or index != 0:
                return None
            pending = Pending(frame.timestamp_us, int.from_bytes(data[2:4], "big"),
                              data[1], bytearray(data[4:]))
        else:
            if (pending is None or index != pending.index or len(data) < 2
                    or not 0 <= frame.timestamp_us - pending.started_us <= self.timeout_us):
                return None
            pending.payload.extend(data[1:])
            pending.index = (index + 1) & 15

        if len(pending.payload) >= pending.size:
            return self._message(frame, pending.header, bytes(pending.payload[:pending.size]))
        self.pending[key] = pending
        return None

    @staticmethod
    def _message(frame: Frame, header: int, payload: bytes) -> Message:
        return Message(frame.timestamp_us, frame.can_id, (header >> 12) & 7,
                       (header >> 6) & 63, header & 63, payload)
