# Secondary board pin reference

Board: **ESP32-WROOM-32 development board**, configured as `esp32dev` in
[platformio.ini](platformio.ini). These are **GPIO numbers**, not header positions.
This reference applies to the secondary board; the primary ESP32-S3 has a different
pin map. Check the actual secondary board's header labels before wiring.

## Current assignments

Directions below are relative to the secondary ESP32. Application assignments come
from [include/config.h](include/config.h) and [src/services/](src/services/).

| Secondary pin | Direction | Function | Connection / notes |
| --- | --- | --- | --- |
| GPIO18 | Input | UART1 modem RX | TEL0161 T / TX |
| GPIO19 | Output | UART1 modem TX | TEL0161 R / RX |
| GPIO26 | Input | UART2 primary-link RX | Primary ESP32-S3 GPIO39 TX |
| GPIO27 | Output | UART2 primary-link TX | Primary ESP32-S3 GPIO40 RX |
| GPIO1 | Output | UART0 TX / `Serial` diagnostics | Onboard USB-to-UART bridge; keep available for flashing and logs |
| GPIO3 | Input | UART0 RX | Onboard USB-to-UART bridge; keep available for flashing |
| GPIO0 | Input | BOOT / bootloader selection | Onboard BOOT button; no secondary application-button function |
| EN | Input | Reset / enable | Onboard reset and programming circuitry; not a GPIO |
| GPIO6-11 | Internal flash bus | Module flash | Reserved; do not connect peripherals |

All three UARTs use **115200 baud**; the two external links use **8N1 and 3.3 V
logic**. Cross TX to RX and connect common ground.

## MCP2515 CAN assignments

These GPIO assignments are confirmed by the user and configured by
[can_horn.cpp](src/services/can_horn.cpp). Avoid assigning other peripherals to
them without updating the firmware and wiring.

The plan assumes the pictured module contains an **MCP2515 controller, TJA1050
transceiver and 8 MHz crystal**. All five signal connections below require suitable
3.3 V / 5 V level translation for that module.

| Secondary pin | Direction | MCP2515 module pin | Function |
| --- | --- | --- | --- |
| GPIO25 | Output | SCK | SPI clock through J9 A2/B2 |
| GPIO23 | Output | MOSI / SI | SPI data to controller through J9 A3/B3 |
| GPIO22 | Input | MISO / SO | SPI data from controller through J9 A4/B4 |
| GPIO21 | Output | CS | Active-low chip select through J9 A5/B5 |
| GPIO32 | Input | INT | J9 A1/B1; active-low interrupt; current TX service uses polling |

Use explicit SPI routing because the default ESP32 SPI pins GPIO18/19 conflict
with the modem. The service uses this explicit routing:

```cpp
SPI.begin(25, 22, 23, 21); // SCK, MISO, MOSI, CS
```

J9 is the SparkFun TXS0108E eight-channel breakout. Connect VA and OE to 3.3 V,
VB to 5 V, and both sides to common ground. The breakout defaults OE low, so it
must be driven high to enable translation. SparkFun supports SPI use but warns
that even six-inch jumper wires can cause ringing; keep signal connections short.
See the [SparkFun hookup guide](https://learn.sparkfun.com/tutorials/level-shifter---8-channel-txs0108e-hookup-guide).

The pinned `autowp-mcp2515` 1.3.1 driver uses this SPI bus at **1 MHz**, with
**8 MHz** oscillator and **500 kbps** CAN settings for the user-tested 2015 Infiniti Q50.

## Power and ground connections

| Connection | Supply / destination | Notes |
| --- | --- | --- |
| Secondary 3V3 | TEL0161 UART connector + / VCC | UART logic supply only |
| TEL0161 Power IN | Separate stable 5 V supply | Modem main power; do not power the modem from the ESP32 3V3 rail |
| MCP2515 module VCC | Regulated 5 V supply | Never connect directly to vehicle 12 V |
| Common GND | Both ESP32 boards, TEL0161 GND, supply negatives and CAN module GND | Include the vehicle interface ground when connected |

For bench use, power each ESP32 through its own USB connector. Do not tie the
boards' 5 V or 3.3 V outputs together when independently USB-powered. See the
[cellular wiring guide](README.md#wiring) for the modem power details.

The pictured TJA1050 requires 4.75-5.25 V. ESP32 GPIOs are not 5 V tolerant, and
the MCP2515's SPI input HIGH minimum is 0.7 times its supply voltage (3.5 V at a
5 V supply). Use a translator suitable for push-pull SPI: three channels from
ESP32 to module (SCK, MOSI, CS), and two from module to ESP32 (MISO, INT).
Powering the whole pictured module at 3.3 V does not meet the TJA1050 specification.

## Vehicle CAN connections and horn scope

| Module connection | Destination |
| --- | --- |
| CAN_H screw terminal | CAN High on the intended compatible vehicle bus |
| CAN_L screw terminal | CAN Low on the same bus |
| GND | Vehicle interface ground / common reference |

Remove the module's **120-ohm termination jumper** when adding it as a branch to
an already terminated vehicle bus. Termination belongs at the two bus endpoints;
a separate bench bus needs its own correct endpoint termination.

There is no separate horn GPIO. A debounced **primary BOOT press/release** sends
`HORN_CHIRP` over UART2; the secondary transmits the supplied, user-tested **2015
Infiniti Q50** sequence on standard CAN ID **0x358**, DLC **8**:

- ON at 0, 20, 120 and 220 ms: `42 02 00 00 00 00 00 00`.
- OFF at 240 ms: `02 02 00 00 00 00 00 00`.

The service uses normal CAN mode, matching the tested program. Initialization sends
no horn frames. This vehicle-specific sequence is not a general horn protocol.
Keep **J1 open** and remove the extra **120-ohm bench resistor** for the vehicle
connection, as specified in the tested program. The MCP2515 supports Classical CAN,
not CAN FD. See [horn operation and error handling](README.md#horn-control).

## Constraints for future pin allocation

- Preserve the current UART and CAN assignments above.
- GPIO0, 2, 5, 12 and 15 are boot-strapping pins; check startup requirements before
  attaching circuitry. GPIO0 already serves BOOT.
- GPIO34-39 are input-only and have no internal pull-up/pull-down resistors; only
  some are exposed by WROOM development boards.
- Board-specific LEDs and other onboard circuitry depend on the exact development
  board. An unlisted GPIO is not automatically verified free on the physical board.
- Update this reference when wiring or firmware assignments change. Keep current
  application pin constants in `include/config.h` consistent with this document.

## Hardware references

- [Espressif ESP32-DevKitC header pinout](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html)
- [ESP32 datasheet](https://documentation.espressif.com/esp32_datasheet_en.html)
- [Espressif Arduino SPI routing](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/spi.html)
- [Microchip MCP2515 datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/APID/ProductDocuments/DataSheets/MCP2515-Family-Data-Sheet-DS20001801K.pdf)
- [NXP TJA1050 datasheet](https://www.nxp.com/docs/en/data-sheet/TJA1050.pdf)
- [TI CAN termination reference](https://www.ti.com/tool/TIDA-01238)
