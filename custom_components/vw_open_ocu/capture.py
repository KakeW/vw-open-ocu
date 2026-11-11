"""Read and write timestamped CSV and compact single-interface candump logs."""

import csv
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
import re
from typing import Iterable, Iterator, TextIO

from .frames import Frame


@dataclass(frozen=True, slots=True)
class Marker:
    timestamp_us: int
    name: str


Record = Frame | Marker
CANDUMP = re.compile(
    r"^\((\d+(?:\.\d+)?)\)\s+(\S+)\s+([0-9A-Fa-f]{3}|[0-9A-Fa-f]{8})#"
    r"([0-9A-Fa-f]{0,16}|R[0-8]?)$"
)


def _timestamp(value: str, scale: int) -> int:
    parsed = Decimal(value) * scale
    if not parsed.is_finite() or parsed < 0 or parsed != parsed.to_integral_value():
        raise ValueError("timestamp must represent nonnegative whole microseconds")
    return int(parsed)


def read_csv(source: TextIO, gvret_time_unit: str | None = None) -> Iterator[Record]:
    reader = csv.DictReader(source)
    fields = set(reader.fieldnames or ())
    if {"Time Stamp", "ID", "Extended", "Bus", "LEN"}.issubset(fields):
        if gvret_time_unit not in {"us", "ms"}:
            raise ValueError("specify --gvret-time-unit us or ms for this CSV")
        yield from _read_gvret(reader, 1 if gvret_time_unit == "us" else 1000)
        return
    time_field = "timestamp_us" if "timestamp_us" in fields else "timestamp_ms"
    if not {time_field, "type", "can_id", "data"}.issubset(fields):
        raise ValueError("missing required CSV columns")
    for row in reader:
        try:
            ts = _timestamp(row[time_field], 1 if time_field == "timestamp_us" else 1000)
            kind = row["type"]
            event = row.get("event")
            if event and not (kind or row["can_id"] or row["data"]):
                yield Marker(ts, event)
                continue
            if event or kind not in {"STD", "EXT", "STD_RTR", "EXT_RTR"}:
                raise ValueError("invalid frame type or event row")
            dlc = int(row["dlc"]) if row.get("dlc") else None
            seq = int(row["sequence"]) if row.get("sequence") else None
            yield Frame(ts, int(row["can_id"], 16), kind.startswith("EXT"),
                        bytes.fromhex(row["data"]), dlc, kind.endswith("_RTR"), seq)
        except (ValueError, TypeError, InvalidOperation) as exc:
            raise ValueError(f"CSV line {reader.line_num}: {exc}") from exc


def _read_gvret(reader: csv.DictReader, scale: int) -> Iterator[Frame]:
    bus = None
    for row in reader:
        try:
            if bus is None:
                bus = row["Bus"]
            if row["Bus"] != bus:
                raise ValueError("split multi-bus logs before analysis")
            dlc = int(row["LEN"])
            extended = row["Extended"].lower()
            if not 0 <= dlc <= 8 or extended not in {"true", "false"}:
                raise ValueError("invalid Classic CAN DLC or identifier format")
            payload = bytes(int(row[f"D{index}"], 16) for index in range(1, dlc + 1))
            yield Frame(_timestamp(row["Time Stamp"], scale), int(row["ID"], 16),
                        extended == "true", payload, dlc)
        except (KeyError, TypeError, ValueError, InvalidOperation, AttributeError) as exc:
            raise ValueError(f"GVRET line {reader.line_num}: {exc}") from exc


def read_candump(source: TextIO) -> Iterator[Frame]:
    interface = None
    for number, line in enumerate(source, 1):
        if not line.strip():
            continue
        match = CANDUMP.fullmatch(line.strip())
        if match is None:
            raise ValueError(f"candump line {number}: unsupported format")
        ts, channel, identifier, payload = match.groups()
        if interface is None:
            interface = channel
        if channel != interface:
            raise ValueError("split multi-interface logs before analysis")
        remote = payload.startswith("R")
        try:
            yield Frame(_timestamp(ts, 1_000_000), int(identifier, 16), len(identifier) == 8,
                        b"" if remote else bytes.fromhex(payload),
                        int(payload[1:] or 0) if remote else None, remote)
        except (ValueError, InvalidOperation) as exc:
            raise ValueError(f"candump line {number}: {exc}") from exc


def write_csv(records: Iterable[Record], target: TextIO) -> None:
    writer = csv.writer(target)
    writer.writerow(["timestamp_us", "type", "can_id", "data", "event", "dlc", "sequence"])
    for record in records:
        if isinstance(record, Marker):
            writer.writerow([record.timestamp_us, "", "", "", record.name, "", ""])
            continue
        kind = "EXT" if record.extended else "STD"
        writer.writerow([record.timestamp_us, kind + ("_RTR" if record.remote else ""),
                         f"0x{record.can_id:X}", record.data.hex().upper(), "",
                         record.dlc, record.sequence])


def write_candump(records: Iterable[Record], target: TextIO) -> None:
    for record in records:
        if isinstance(record, Marker):
            continue
        seconds, micros = divmod(record.timestamp_us, 1_000_000)
        width = 8 if record.extended else 3
        data = f"R{record.dlc}" if record.remote else record.data.hex().upper()
        target.write(f"({seconds}.{micros:06d}) can0 {record.can_id:0{width}X}#{data}\n")
