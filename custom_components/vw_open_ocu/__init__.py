"""Receive-only CAN gateway integration."""

from datetime import timedelta
import re
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from homeassistant.core import HomeAssistant

DOMAIN = "vw_open_ocu"
GATEWAY_ID = re.compile(r"[a-z0-9][a-z0-9_-]{0,31}\Z")


async def async_setup(hass: "HomeAssistant", config: dict) -> bool:
    from .services import register

    register(hass)
    return True


async def async_setup_entry(hass, entry) -> bool:
    from homeassistant.components import mqtt
    from homeassistant.core import callback
    from homeassistant.exceptions import ConfigEntryNotReady
    from homeassistant.helpers.event import async_track_time_interval
    from homeassistant.helpers.update_coordinator import DataUpdateCoordinator
    import logging

    from .runtime import Gateway

    if not await mqtt.async_wait_for_mqtt_client(hass):
        raise ConfigEntryNotReady("Configure Home Assistant MQTT first")

    gateway = Gateway()
    coordinator = DataUpdateCoordinator(
        hass, logging.getLogger(__name__), name=DOMAIN, config_entry=entry
    )
    entry.runtime_data = (gateway, coordinator)
    prefix = f"{DOMAIN}/{entry.data['gateway_id']}/v1"

    @callback
    def status_message(message):
        if not message.retain and gateway.heartbeat(message.payload):
            coordinator.async_set_updated_data(gateway)

    @callback
    def frame_message(message):
        if not message.retain:
            gateway.ingest(message.payload)

    @callback
    def freshness_tick(now):
        gateway.check_freshness()
        coordinator.async_set_updated_data(gateway)

    @callback
    def mqtt_connection_changed(connected):
        if not connected:
            gateway.broker_disconnected()
            coordinator.async_set_updated_data(gateway)

    entry.async_on_unload(mqtt.async_subscribe_connection_status(hass, mqtt_connection_changed))
    entry.async_on_unload(await mqtt.async_subscribe(
        hass, f"{prefix}/status", status_message, qos=0, encoding=None
    ))
    entry.async_on_unload(await mqtt.async_subscribe(
        hass, f"{prefix}/frames", frame_message, qos=0, encoding=None
    ))
    entry.async_on_unload(async_track_time_interval(hass, freshness_tick, timedelta(seconds=2)))
    await hass.config_entries.async_forward_entry_setups(entry, ["binary_sensor", "sensor"])
    return True


async def async_unload_entry(hass, entry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, ["binary_sensor", "sensor"])
