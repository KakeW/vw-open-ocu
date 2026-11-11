# VW Open OCU

Receive-only CAN telemetry and diagnostics for the Volkswagen Passat GTE MY2018. The MY2018 e-Golf is a protocol reference. Vehicle state requires validation on the Passat before it can appear in Home Assistant.

- [Architecture](docs/architecture.md)
- [Bench and Home Assistant setup](docs/development.md)
- [Passat preconditioning capture workflow](docs/vehicle-capture.md)
- [Hardware and safety](docs/hardware.md)
- [MQTT protocol](docs/mqtt-protocol.md)
- [CAN and BAP evidence](docs/can-protocol.md)

Run the offline checks with `python3 -m unittest discover -s tests -v`.
