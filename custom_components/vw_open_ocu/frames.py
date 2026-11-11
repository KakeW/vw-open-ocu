"""Classic CAN frames shared by Home Assistant and the offline tools."""

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class Frame:
    timestamp_us: int
    can_id: int
    extended: bool
    data: bytes
    dlc: int | None = None
    remote: bool = False
    sequence: int | None = None

    def __post_init__(self) -> None:
        dlc = len(self.data) if self.dlc is None else self.dlc
        object.__setattr__(self, "dlc", dlc)
        limit = 0x1FFFFFFF if self.extended else 0x7FF
        if self.timestamp_us < 0 or not 0 <= self.can_id <= limit:
            raise ValueError("invalid CAN identifier or timestamp")
        if not 0 <= dlc <= 8 or len(self.data) != (0 if self.remote else dlc):
            raise ValueError("invalid Classic CAN DLC or payload")

    @property
    def key(self) -> tuple[bool, int]:
        return self.extended, self.can_id


def label(key: tuple[bool, int]) -> str:
    extended, identifier = key
    return f"{'EXT' if extended else 'STD'}:{identifier:0{8 if extended else 3}X}"
