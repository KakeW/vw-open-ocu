"""Gateway freshness, sequence accounting and bounded local capture."""

from datetime import UTC, datetime
from io import StringIO
from time import monotonic

from .bap import Assembler
from .capture import Marker, Record, write_candump, write_csv
from .wire import decode_batch, decode_status

STALE_SECONDS = 16
CAPTURE_LIMIT = 10_000


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
        self._last_capture_frame_us: int | None = None
        # Channel membership is empty until confirmed Passat observations exist.
        self.bap = Assembler(set())

    @property
    def online(self) -> bool:
        return (self._heartbeat_monotonic is not None
                and monotonic() - self._heartbeat_monotonic < STALE_SECONDS)

    def heartbeat(self, payload: bytes) -> bool:
        try:
            status = decode_status(payload)
        except (ValueError, RecursionError):
            self.invalid_messages += 1
            return False
        if status["session"] != self.status.get("session"):
            self.last_sequence = None
            self.sequence_gaps = 0
            self._last_can_us = None
            self.bap.pending.clear()
            if self.capture_session is not None and self.recording:
                self.recording = False
        self.status = status
        self.last_heartbeat = datetime.now(UTC)
        self._heartbeat_monotonic = monotonic()
        return True

    def ingest(self, payload: bytes) -> None:
        try:
            session, frames = decode_batch(payload)
        except ValueError:
            self.invalid_messages += 1
            return
        if not self.online or session != self.status.get("session"):
            return
        for frame in frames:
            sequence = frame.sequence
            assert sequence is not None
            if self.last_sequence is not None:
                if sequence <= self.last_sequence or frame.timestamp_us < self._last_can_us:
                    continue
                if sequence > self.last_sequence + 1:
                    self.sequence_gaps += sequence - self.last_sequence - 1
                    self.bap.pending.clear()
            self.last_sequence = sequence
            self._last_can_us = frame.timestamp_us
            self.bap.feed(frame)
            if self.recording:
                self._last_capture_frame_us = frame.timestamp_us
                self._append(frame)

    def start_capture(self) -> None:
        if self.recording:
            raise ValueError("Stop the active capture before starting another")
        if not self.online or not self.status["capture_mode"]:
            raise ValueError("Capture requires a live gateway with capture mode enabled")
        self.records.clear()
        self._last_capture_frame_us = None
        self.capture_discarded = 0
        self.capture_session = self.status["session"]
        self.recording = True

    def mark(self, name: str) -> None:
        if not self.recording or not self.online or self._last_capture_frame_us is None:
            raise ValueError("A live recording with at least one observed frame is required")
        if len(self.records) >= CAPTURE_LIMIT:
            raise ValueError("Capture is full")
        self._append(Marker(self._last_capture_frame_us, name))

    def _append(self, record: Record) -> None:
        if len(self.records) < CAPTURE_LIMIT:
            self.records.append(record)
        else:
            self.capture_discarded += 1

    def export(self, format_name: str) -> str:
        if self.recording:
            raise ValueError("Stop the capture before exporting")
        output = StringIO()
        writer = write_csv if format_name == "csv" else write_candump
        writer(self.records, output)
        return output.getvalue()
