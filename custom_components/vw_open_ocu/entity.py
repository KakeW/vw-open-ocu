"""Shared diagnostics device identity."""

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity import EntityCategory
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import DOMAIN


class GatewayEntity(CoordinatorEntity):
    _attr_has_entity_name = True
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, entry, key: str, name: str) -> None:
        self.gateway, coordinator = entry.runtime_data
        super().__init__(coordinator)
        gateway_id = entry.data["gateway_id"]
        self._attr_unique_id = f"{gateway_id}_{key}"
        self._attr_name = name
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, gateway_id)}, name=f"VW Open OCU ({gateway_id})"
        )
