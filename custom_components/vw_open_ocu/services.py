"""Admin actions for recording and exporting local gateway captures."""

import voluptuous as vol

from homeassistant.core import ServiceCall, SupportsResponse
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.service import async_register_admin_service

from . import DOMAIN
from .runtime import Gateway


def register(hass) -> None:
    def gateway(call: ServiceCall) -> Gateway:
        for entry in hass.config_entries.async_loaded_entries(DOMAIN):
            if entry.data["gateway_id"] == call.data["gateway_id"]:
                return entry.runtime_data[0]
        raise HomeAssistantError("Gateway entry is not loaded")

    async def start(call: ServiceCall) -> None:
        try:
            gateway(call).start_capture()
        except ValueError as exc:
            raise HomeAssistantError(str(exc)) from exc

    async def stop(call: ServiceCall) -> None:
        gateway(call).recording = False

    async def mark(call: ServiceCall) -> None:
        try:
            gateway(call).mark(call.data["name"])
        except ValueError as exc:
            raise HomeAssistantError(str(exc)) from exc

    async def export(call: ServiceCall) -> dict:
        state = gateway(call)
        try:
            return {
                "data": state.export(call.data["format"]),
                "discarded": state.capture_discarded,
                "record_count": len(state.records),
            }
        except ValueError as exc:
            raise HomeAssistantError(str(exc)) from exc

    required = {vol.Required("gateway_id"): str}
    async_register_admin_service(hass, DOMAIN, "capture_start", start, vol.Schema(required))
    async_register_admin_service(hass, DOMAIN, "capture_stop", stop, vol.Schema(required))
    async_register_admin_service(
        hass, DOMAIN, "capture_mark", mark,
        vol.Schema({**required, vol.Required("name"): vol.All(str, vol.Length(min=1, max=64))}),
    )
    async_register_admin_service(
        hass, DOMAIN, "capture_export", export,
        vol.Schema({**required, vol.Optional("format", default="csv"): vol.In(["csv", "candump"])}),
        SupportsResponse.ONLY,
    )
