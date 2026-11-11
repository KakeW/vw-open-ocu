# Hardware and safety

The initial target from the project brief is a Nologo ESP32-C3 SuperMini with a 3.3 V SN65HVD230 board. The proposed wiring is transceiver TXD to GPIO0, RXD to GPIO1, and 500 kbit/s Classic CAN. Confirm board pin labels and electrical levels against the exact hardware. The first bench run should leave the transceiver TXD physically disconnected from GPIO0 and hold TXD at its verified recessive level. Use a separate transmitting and acknowledging node or a bench generator configured for silent listeners.

## Passat GTE MY2018 convenience CAN access

| Control unit | Connector | CAN High | CAN Low | Evidence |
| --- | --- | --- | --- | --- |
| J255 Climatronic | T20d (C), 20-pin | Cavity 5, B506 | Cavity 6, B507 | Wiring-diagram-supported. Physical validation pending. |
| J605 rear lid control module | T32e | Cavity 32, orange/green per project brief | Cavity 31, orange/brown per project brief | Previously identified candidate. Physical validation pending. |

The J255 assignments come from the supplied VW Climatronic J255 connector assignment, edition 12.2018. The supplied information identifies B506 and B507 as the same convenience CAN network references at J605. This supports J255 as an alternative development access point. Confirm applicability to the vehicle's build before wiring.

Identify the connector by its designation, keying and latch. Establish whether the diagram shows the mating face or wire-entry face and whether it depicts the harness or control-unit half. Opposite views can mirror the cavity layout. Use the molded cavity numbers and a verified connector view for cavities 5 and 6. Wire colors at J605 do not establish wire colors at J255. Stop if the cavity numbers, connector view or network assignment are ambiguous.

Use a verified pass-through breakout with all original circuits connected end to end and a short parallel branch for the receive-only interface. Keep J255, J605, the OCU and the other original control modules connected during recording. Verify the interface adds no termination. The project brief reports that the bench breakout's 120-ohm resistor was removed. Verify the actual hardware, bus identity, polarity, bitrate and termination before capture. See the [vehicle capture workflow](vehicle-capture.md).

Treat the current breakout as bench hardware. A vehicle installation needs a qualified automotive interface covering isolation or engineered grounding, ESD and transient protection, short and reverse-polarity protection, automotive power conversion, and behavior with either side unpowered. Measure current draw across awake, asleep and reconnect states against a battery budget. Confirm that the gateway cannot ACK, send error flags, hold the bus dominant, wake the car, or prevent sleep during boot, faults and loss of Wi-Fi or HA.

Sources for controller behavior and transceiver electrical requirements:
- [ESP-IDF 5.5.2 TWAI](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32c3/api-reference/peripherals/twai.html)
- [TI SN65HVD230 datasheet](https://www.ti.com/lit/ds/symlink/sn65hvd230.pdf)
