"""Gateway connectivity diagnostic."""

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity

from .entity import GatewayEntity


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([Connectivity(entry, "online", "Connectivity")])


class Connectivity(GatewayEntity, BinarySensorEntity):
    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY

    @property
    def is_on(self):
        return self.gateway.online
