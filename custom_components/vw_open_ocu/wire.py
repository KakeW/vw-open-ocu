"""Decode version 1 gateway messages without a Home Assistant dependency."""

import json
import re
from struct import Struct

from .frames import Frame

MAGIC = b"OCU\x01"
HEADER_SIZE = 12
MAX_FRAMES = 48
PREFIX = Struct(">QQIB")
COUNTERS = (
    "queue_depth", "queue_capacity", "queue_peak", "queue_dropped",
    "transport_dropped", "filtered", "unsupported_dlc", "driver_missed",
    "driver_overrun", "bus_errors", "rx_errors", "controller_state",
)


def decode_batch(payload: bytes) -> tuple[str, list[Frame]]:
    if not HEADER_SIZE < len(payload) <= HEADER_SIZE + MAX_FRAMES * (PREFIX.size + 8):
        raise ValueError("invalid batch size")
    if payload[:4] != MAGIC:
        raise ValueError("unsupported batch version")
    session = payload[4:12].hex()
    offset = HEADER_SIZE
    frames = []
    while offset < len(payload):
        if len(frames) >= MAX_FRAMES or len(payload) - offset < PREFIX.size:
            raise ValueError("truncated frame or oversized batch")
        sequence, timestamp, flags_id, dlc = PREFIX.unpack_from(payload, offset)
        offset += PREFIX.size
        if flags_id & (1 << 29) or dlc > 8:
            raise ValueError("unsupported CAN flags or DLC")
        remote = bool(flags_id & (1 << 30))
        end = offset if remote else offset + dlc
        if end > len(payload) or (frames and (
            sequence <= frames[-1].sequence or timestamp < frames[-1].timestamp_us
        )):
            raise ValueError("truncated or out-of-order frame")
        frames.append(Frame(timestamp, flags_id & 0x1FFFFFFF, bool(flags_id & (1 << 31)),
                            payload[offset:end], dlc, remote, sequence))
        offset = end
    return session, frames


def decode_status(payload: bytes) -> dict:
    if len(payload) > 1024:
        raise ValueError("oversized heartbeat")
    status = json.loads(payload)
    if (not isinstance(status, dict) or type(status.get("version")) is not int
            or status["version"] != 1 or not isinstance(status.get("session"), str)
            or not re.fullmatch(r"[0-9a-f]{16}", status["session"])
            or type(status.get("capture_mode")) is not bool):
        raise ValueError("invalid heartbeat header")
    for key in COUNTERS:
        value = status.get(key)
        if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
            raise ValueError(f"invalid heartbeat counter {key}")
    if not 0 <= status["queue_depth"] <= status["queue_capacity"] or not (
        0 <= status["queue_peak"] <= status["queue_capacity"]
    ):
        raise ValueError("invalid heartbeat queue size")
    return {key: status[key] for key in ("session", "capture_mode", *COUNTERS)}
