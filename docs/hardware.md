# Hardware and safety

The initial target from the project brief is a Nologo ESP32-C3 SuperMini with a 3.3 V SN65HVD230 board. The proposed wiring is transceiver TXD to GPIO0, RXD to GPIO1, and 500 kbit/s Classic CAN. Confirm board pin labels and electrical levels against the exact hardware. The first bench run should leave the transceiver TXD physically disconnected from GPIO0 and hold TXD at its verified recessive level. Use a separate transmitting and acknowledging node or a bench generator configured for silent listeners.

The proposed Passat J605 connector T32e assignments (32 CAN-H orange/green, 31 CAN-L orange/brown) come from the brief and are unverified. Check the vehicle-specific wiring diagram and verify bus identity, polarity, bitrate and sleep behavior before connecting. The reported removal of the breakout's 120-ohm resistor fits the intended parallel tap. Verify the resulting board and the existing bus termination before any vehicle test.

Treat the current breakout as bench hardware. A vehicle installation needs a qualified automotive interface covering isolation or engineered grounding, ESD and transient protection, short and reverse-polarity protection, automotive power conversion, and behavior with either side unpowered. Measure current draw across awake, asleep and reconnect states against a battery budget. Confirm that the gateway cannot ACK, send error flags, hold the bus dominant, wake the car, or prevent sleep during boot, faults and loss of Wi-Fi or HA.

Sources for controller behavior and transceiver electrical requirements:
- [ESP-IDF 5.5.2 TWAI](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32c3/api-reference/peripherals/twai.html)
- [TI SN65HVD230 datasheet](https://www.ti.com/lit/ds/symlink/sn65hvd230.pdf)
