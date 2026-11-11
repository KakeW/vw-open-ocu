"""Timestamp and loss counters."""

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity

from .entity import GatewayEntity


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([
        LastHeartbeat(entry, "last_heartbeat", "Last gateway update"),
        Counter(entry, "queue_depth", "Queue depth"),
        Counter(entry, "queue_dropped", "Queue drops"),
        Counter(entry, "transport_dropped", "Transport drops"),
        Counter(entry, "driver_missed", "Controller receive misses"),
        Counter(entry, "driver_overrun", "Controller receive overruns"),
        Counter(entry, "bus_errors", "CAN bus errors"),
        Counter(entry, "sequence_gaps", "Observed sequence gaps"),
    ])


class LastHeartbeat(GatewayEntity, SensorEntity):
    _attr_device_class = SensorDeviceClass.TIMESTAMP

    @property
    def native_value(self):
        return self.gateway.last_heartbeat


class Counter(GatewayEntity, SensorEntity):
    def __init__(self, entry, key: str, name: str) -> None:
        super().__init__(entry, key, name)
        self.key = key

    @property
    def available(self):
        return super().available and self.gateway.online

    @property
    def native_value(self):
        if self.key == "sequence_gaps":
            return self.gateway.sequence_gaps
        return self.gateway.status.get(self.key)
