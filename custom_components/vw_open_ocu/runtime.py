"""Gateway freshness, sequence accounting and bounded local capture."""

from datetime import UTC, datetime
from io import StringIO
from time import monotonic

from .bap import Assembler
from .capture import Marker, Record, write_candump, write_csv
from .frames import Frame
from .wire import decode_batch, decode_status

STALE_SECONDS = 16
CAPTURE_LIMIT = 10_000
MAX_CAPTURE_LIMIT = 100_000
LOSS_COUNTERS = (
    "queue_dropped", "transport_dropped", "driver_missed", "driver_overrun",
    "unsupported_dlc", "bus_errors",
)


def _position(frame: Frame | None) -> dict | None:
    if frame is None:
        return None
    return {"timestamp_us": frame.timestamp_us, "sequence": frame.sequence}


class Gateway:
    def __init__(self) -> None:
        self.status: dict = {}
        self.last_heartbeat: datetime | None = None
        self._heartbeat_monotonic: float | None = None
        self.last_sequence: int | None = None
        self.sequence_gaps = 0
        self.invalid_messages = 0
        self.recording = False
        self.records: list[Record] = []
        self.capture_discarded = 0
        self.capture_session: str | None = None
        self._last_can_us: int | None = None
        self._record_limit = CAPTURE_LIMIT
        self._capture_started_at: str | None = None
        self._capture_stopped_at: str | None = None
        self._stop_reason: str | None = None
        self._start_heartbeat: dict | None = None
        self._end_heartbeat: dict | None = None
        self._first_frame: Frame | None = None
        self._last_frame: Frame | None = None
        self._capture_gaps = 0
        self._capture_invalid = 0
        self._capture_out_of_order = 0
        self._capture_other_session = 0
        self._markers: list[dict] = []
        self._pending_markers: list[dict] = []
        # Channel membership is empty until confirmed Passat observations exist.
        self.bap = Assembler(set())

    @property
    def online(self) -> bool:
        return (self._heartbeat_monotonic is not None
                and monotonic() - self._heartbeat_monotonic < STALE_SECONDS)

    def _heartbeat_snapshot(self) -> dict | None:
        if self.last_heartbeat is None:
            return None
        return {"received_at_utc": self.last_heartbeat.isoformat(), "status": self.status.copy()}

    def _invalid_message(self) -> None:
        self.invalid_messages += 1
        if self.recording:
            self._capture_invalid += 1

    def check_freshness(self) -> None:
        if self.recording and not self.online:
            self.stop_capture("heartbeat_timeout")

    def broker_disconnected(self) -> None:
        self._heartbeat_monotonic = None
        self.bap.pending.clear()
        self.stop_capture("ha_mqtt_disconnected")

    def heartbeat(self, payload: bytes) -> bool:
        self.check_freshness()
        try:
            status = decode_status(payload)
        except (ValueError, RecursionError):
            self._invalid_message()
            return False
        previous = self.status
        if status["session"] != previous.get("session"):
            self.stop_capture("gateway_reboot")
            self.last_sequence = None
            self.sequence_gaps = 0
            self._last_can_us = None
            self.bap.pending.clear()
        self.status = status
        self.last_heartbeat = datetime.now(UTC)
        self._heartbeat_monotonic = monotonic()
        if status.get("connection_epoch") != previous.get("connection_epoch"):
            self.stop_capture("gateway_connection_changed")
            self.bap.pending.clear()
        return True

    def ingest(self, payload: bytes) -> None:
        self.check_freshness()
        try:
            session, frames = decode_batch(payload)
        except ValueError:
            self._invalid_message()
            return
        if not self.online or session != self.status.get("session"):
            if self.recording:
                self._capture_other_session += 1
            return
        for frame in frames:
            sequence = frame.sequence
            assert sequence is not None
            if self.last_sequence is not None:
                if sequence <= self.last_sequence or frame.timestamp_us < self._last_can_us:
                    if self.recording:
                        self._capture_out_of_order += 1
                    continue
                if sequence > self.last_sequence + 1:
                    self.sequence_gaps += sequence - self.last_sequence - 1
                    self.bap.pending.clear()
            self.last_sequence = sequence
            self._last_can_us = frame.timestamp_us
            self.bap.feed(frame)
            if self.recording:
                self._record_frame(frame)

    def _record_frame(self, frame: Frame) -> None:
        if self._last_frame is not None:
            self._capture_gaps += frame.sequence - self._last_frame.sequence - 1
        else:
            self._first_frame = frame
        self._last_frame = frame
        if self._pending_markers:
            for marker in self._pending_markers:
                marker["next_received_frame"] = _position(frame)
            self._pending_markers.clear()
        if len(self.records) < self._record_limit:
            self.records.append(frame)
        else:
            self.capture_discarded += 1

    def start_capture(self, max_records: int | None = None) -> None:
        self.check_freshness()
        if self.recording:
            raise ValueError("Stop the active capture before starting another")
        if not self.online or not self.status["capture_mode"]:
            raise ValueError("Capture requires a live gateway with capture mode enabled")
        limit = CAPTURE_LIMIT if max_records is None else max_records
        if not 1 <= limit <= MAX_CAPTURE_LIMIT:
            raise ValueError("Capture record limit is out of range")
        self.records.clear()
        self.capture_discarded = 0
        self.capture_session = self.status["session"]
        self._record_limit = limit
        self._capture_started_at = datetime.now(UTC).isoformat()
        self._capture_stopped_at = None
        self._stop_reason = None
        self._start_heartbeat = self._heartbeat_snapshot()
        self._end_heartbeat = None
        self._first_frame = None
        self._last_frame = None
        self._capture_gaps = 0
        self._capture_invalid = 0
        self._capture_out_of_order = 0
        self._capture_other_session = 0
        self._markers.clear()
        self._pending_markers.clear()
        self.recording = True

    def stop_capture(self, reason: str = "manual") -> None:
        if self.recording:
            self.recording = False
            self._capture_stopped_at = datetime.now(UTC).isoformat()
            self._stop_reason = reason
            self._end_heartbeat = self._heartbeat_snapshot()
            self._pending_markers.clear()

    def mark(self, name: str) -> None:
        self.check_freshness()
        if not self.recording or self._last_frame is None:
            raise ValueError("A live recording with at least one observed frame is required")
        if len(self.records) >= self._record_limit:
            raise ValueError("Capture is full")
        self.records.append(Marker(self._last_frame.timestamp_us, name))
        marker = {
            "name": name,
            "requested_at_utc": datetime.now(UTC).isoformat(),
            "previous_received_frame": _position(self._last_frame),
            "next_received_frame": None,
        }
        self._markers.append(marker)
        self._pending_markers.append(marker)

    def capture_metadata(self) -> dict:
        start = self._start_heartbeat
        end = self._end_heartbeat
        deltas = None
        if start and end and start["received_at_utc"] != end["received_at_utc"]:
            before, after = start["status"], end["status"]
            if before["session"] == after["session"]:
                deltas = {key: after[key] - before[key] if after[key] >= before[key] else None
                          for key in LOSS_COUNTERS}
        watermark = end["status"].get("last_published_sequence") if end else None
        publish_ahead = None
        if self._last_frame is not None and watermark is not None:
            publish_ahead = max(0, watermark - self._last_frame.sequence)
        return {
            "session": self.capture_session,
            "started_at_utc": self._capture_started_at,
            "stopped_at_utc": self._capture_stopped_at,
            "stop_reason": self._stop_reason,
            "max_records": self._record_limit,
            "first_received_frame": _position(self._first_frame),
            "last_received_frame": _position(self._last_frame),
            "sequence_gaps_between_received_frames": self._capture_gaps,
            "invalid_messages": self._capture_invalid,
            "duplicate_or_out_of_order_frames": self._capture_out_of_order,
            "other_session_batches": self._capture_other_session,
            "start_heartbeat": start,
            "end_heartbeat": end,
            "heartbeat_counter_deltas": deltas,
            "end_heartbeat_publish_ahead_by": publish_ahead,
            "markers": self._markers.copy(),
        }

    def export(self, format_name: str) -> str:
        self.check_freshness()
        if self.recording:
            raise ValueError("Stop the capture before exporting")
        output = StringIO()
        writer = write_csv if format_name == "csv" else write_candump
        writer(self.records, output)
        return output.getvalue()
