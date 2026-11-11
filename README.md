# VW Open OCU

![VW Open OCU icon](custom_components/vw_open_ocu/brand/icon.png)

Receive-only CAN telemetry and diagnostics for the Volkswagen Passat GTE MY2018. The MY2018 e-Golf is a protocol reference. Vehicle state requires validation on the Passat before it can appear in Home Assistant.

VW Open OCU is an independent community project. The repository's [MIT license](LICENSE) covers the project source, documentation, synthetic fixtures, and original icon. External reference material is linked in the [protocol notes](docs/can-protocol.md).

## Install the Home Assistant integration

Once the repository is public, open **Settings → Marketplace**, add `KakeW/vw-open-ocu` as a custom repository of type **Integration**, and install **VW Open OCU**. Set up Home Assistant's MQTT integration first, then add **VW Open OCU** under **Settings → Devices & services** and enter your gateway ID. The minimum tested Home Assistant version is 2026.10.0. See the [manual installation and firmware setup](docs/development.md) for the file-copy option.

## Development

- [ESPHome Device Builder setup](docs/esphome.md)
- [Private source and remote OTA](docs/remote-ota.md)
- [Architecture](docs/architecture.md)
- [Bench and Home Assistant setup](docs/development.md)
- [Passat preconditioning capture workflow](docs/vehicle-capture.md)
- [Hardware and safety](docs/hardware.md)
- [MQTT protocol](docs/mqtt-protocol.md)
- [CAN and BAP evidence](docs/can-protocol.md)

Run the offline checks with `python3 -m unittest discover -s tests -v`.
