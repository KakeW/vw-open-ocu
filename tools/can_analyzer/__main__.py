"""Offline capture statistics, comparison, event windows and BAP extraction."""

import argparse
from collections import defaultdict
from dataclasses import asdict
import json
from pathlib import Path
from typing import Iterable

from custom_components.vw_open_ocu.bap import Assembler
from custom_components.vw_open_ocu.capture import (
    Marker, Record, read_candump, read_csv, write_candump, write_csv,
)
from custom_components.vw_open_ocu.frames import Frame, label


def load(path: Path, gvret_time_unit: str | None = None) -> list[Record]:
    with path.open(newline="", encoding="utf-8-sig") as source:
        return list(read_csv(source, gvret_time_unit) if path.suffix.lower() == ".csv" else read_candump(source))


def summarize(frames: Iterable[Frame]) -> dict:
    groups: dict[str, list[Frame]] = defaultdict(list)
    for frame in frames:
        groups[label(frame.key)].append(frame)
    summary = {}
    for name, group in sorted(groups.items()):
        duration = max(frame.timestamp_us for frame in group) - min(
            frame.timestamp_us for frame in group
        )
        values = [
            {frame.data[index] for frame in group if len(frame.data) > index}
            for index in range(8)
        ]
        summary[name] = {
            "count": len(group),
            "frequency_hz": (len(group) - 1) * 1_000_000 / duration if duration else None,
            "changing_bytes": [index for index, seen in enumerate(values) if len(seen) > 1],
            "byte_values": [sorted(seen) for seen in values],
            "lengths": sorted({len(frame.data) for frame in group}),
        }
    return summary


def compare(baseline: dict, action: dict) -> dict:
    return {
        "new": sorted(action.keys() - baseline.keys()),
        "missing": sorted(baseline.keys() - action.keys()),
        "changed": {
            key: {"baseline": baseline[key], "action": action[key]}
            for key in sorted(action.keys() & baseline.keys()) if action[key] != baseline[key]
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--compare", type=Path, help="Action capture to compare against baseline")
    parser.add_argument("--id", action="append", default=[], help="Filter by STD:123 or EXT:00000123")
    parser.add_argument("--bap-id", action="append", type=lambda value: int(value, 16), default=[])
    parser.add_argument("--event-window-us", type=int)
    parser.add_argument("--gvret-time-unit", choices=["us", "ms"],
                        help="Explicit timestamp unit for GVRET CSV inputs")
    parser.add_argument("--export", type=Path)
    args = parser.parse_args()
    if args.event_window_us is not None and args.event_window_us < 0:
        parser.error("--event-window-us requires a nonnegative integer")

    def selected(records: Iterable[Record]) -> list[Record]:
        return [record for record in records if isinstance(record, Marker)
                or not args.id or label(record.key) in args.id]

    try:
        records = selected(load(args.capture, args.gvret_time_unit))
        frames = [record for record in records if isinstance(record, Frame)]
        summary = summarize(frames)
        result: dict = {"ids": summary}
        if args.compare:
            result["comparison"] = compare(summary, summarize(
                record for record in selected(load(args.compare, args.gvret_time_unit)) if isinstance(record, Frame)
            ))
        if args.event_window_us is not None:
            result["events"] = [
                {**asdict(marker), "ids": summarize(
                    frame for frame in frames
                    if abs(frame.timestamp_us - marker.timestamp_us) <= args.event_window_us
                )}
                for marker in records if isinstance(marker, Marker)
            ]
        if args.bap_id:
            assembler = Assembler(set(args.bap_id))
            result["bap"] = [
                {**asdict(message), "payload": message.payload.hex()}
                for frame in frames if (message := assembler.feed(frame)) is not None
            ]
        if args.export:
            with args.export.open("w", newline="", encoding="utf-8") as target:
                writer = write_csv if args.export.suffix.lower() == ".csv" else write_candump
                writer(records, target)
        print(json.dumps(result, indent=2))
    except (OSError, ValueError) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
