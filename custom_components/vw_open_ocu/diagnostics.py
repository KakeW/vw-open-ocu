"""Download diagnostic counts without raw frames or broker credentials."""


async def async_get_config_entry_diagnostics(hass, entry):
    gateway, _ = entry.runtime_data
    return {
        "online": gateway.online,
        "gateway_status": {key: value for key, value in gateway.status.items() if key != "session"},
        "sequence_gaps": gateway.sequence_gaps,
        "invalid_messages": gateway.invalid_messages,
        "recording": gateway.recording,
        "capture_records": len(gateway.records),
        "capture_discarded": gateway.capture_discarded,
    }
