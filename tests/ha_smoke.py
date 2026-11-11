"""Exercise the real Home Assistant integration against an isolated local broker."""

import asyncio
import logging
from pathlib import Path
import shutil
import sys
from tempfile import TemporaryDirectory

from homeassistant import bootstrap, loader
from homeassistant.config_entries import ConfigEntryState
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry
from paho.mqtt.client import CallbackAPIVersion, Client

from test_gateway import batch, frame, status

DOMAIN = "vw_open_ocu"
GATEWAY_ID = "smoke"
PREFIX = f"{DOMAIN}/{GATEWAY_ID}/v1"


async def wait_until(condition) -> None:
    async with asyncio.timeout(15):
        while not condition():
            await asyncio.sleep(0.05)


async def run(port: int) -> None:
    with TemporaryDirectory() as config:
        source = Path(__file__).resolve().parents[1] / "custom_components" / DOMAIN
        shutil.copytree(source, Path(config) / "custom_components" / DOMAIN)
        (Path(config) / "configuration.yaml").write_text("homeassistant: {}\n")
        hass = HomeAssistant(config)
        loader.async_setup(hass)
        client = Client(CallbackAPIVersion.VERSION2)
        try:
            assert await bootstrap.async_from_config_dict({"homeassistant": {}}, hass)
            await hass.async_start()
            await configure(hass, "mqtt", {
                "broker": "127.0.0.1", "port": port, "protocol": "5",
                "other_settings": {"set_client_cert": False, "set_ca_cert": "off"},
            })
            entry = await configure(hass, DOMAIN, {"gateway_id": GATEWAY_ID})
            gateway, _ = entry.runtime_data

            await asyncio.to_thread(client.connect, "127.0.0.1", port)
            client.loop_start()
            for _ in range(25):
                client.publish(f"{PREFIX}/status", status(), retain=False)
                if gateway.online:
                    break
                await asyncio.sleep(0.2)
            assert gateway.online
            registry = entity_registry.async_get(hass)
            connectivity = registry.async_get_entity_id("binary_sensor", DOMAIN, f"{GATEWAY_ID}_online")
            assert connectivity is not None
            await wait_until(lambda: hass.states.is_state(connectivity, "on"))

            async def action(service: str, response: bool = False, **data):
                return await hass.services.async_call(
                    DOMAIN, service, {"gateway_id": GATEWAY_ID, **data},
                    blocking=True, return_response=response,
                )

            await action("capture_start", max_records=12)
            client.publish(f"{PREFIX}/frames", batch(frame(0), frame(2)), retain=False)
            await wait_until(lambda: len(gateway.records) == 2)
            await action("capture_mark", name="synthetic event")
            await action("capture_stop")
            response = await action("capture_export", response=True, format="csv")
            assert response["record_count"] == 3
            assert "synthetic event" in response["data"]
            assert gateway.sequence_gaps == 1
            metadata = response["metadata"]
            assert metadata["max_records"] == 12
            assert metadata["stop_reason"] == "manual"
            assert metadata["sequence_gaps_between_received_frames"] == 1
            assert metadata["markers"][0]["previous_received_frame"]["sequence"] == 2
            assert await hass.config_entries.async_unload(entry.entry_id)
            await hass.async_block_till_done()
            state = hass.states.get(connectivity)
            assert state is None or state.state == "unavailable"
            print("Home Assistant MQTT, entity, capture/export and unload smoke passed")
        finally:
            client.disconnect()
            await asyncio.to_thread(client.loop_stop)
            await hass.async_stop()


async def configure(hass: HomeAssistant, domain: str, data: dict):
    form = await hass.config_entries.flow.async_init(domain, context={"source": "user"})
    result = await hass.config_entries.flow.async_configure(form["flow_id"], data)
    assert result["type"] == "create_entry", result
    entry = result["result"]
    assert entry.state == ConfigEntryState.LOADED, (entry.state, entry.reason)
    return entry


if __name__ == "__main__":
    logging.basicConfig(level=logging.WARNING)
    asyncio.run(run(int(sys.argv[1])))
