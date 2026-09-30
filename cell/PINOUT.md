# Secondary board pin reference

Board: **ESP32-WROOM-32 development board**, configured as `esp32dev` in
[platformio.ini](platformio.ini). These are **GPIO numbers**, not header positions.
This reference applies to the secondary board; the primary ESP32-S3 has a different
pin map. Check the actual secondary board's header labels before wiring.

## Current assignments

Directions below are relative to the secondary ESP32. Application assignments come
from [include/config.h](include/config.h) and [src/main.cpp](src/main.cpp).

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

## Proposed MCP2515 CAN assignments

These pins are **reserved by this wiring plan for future CAN integration**. The
current firmware does not initialize them or contain a CAN driver. Avoid assigning
new peripherals to them without updating this plan.

The plan assumes the pictured module contains an **MCP2515 controller, TJA1050
transceiver and 8 MHz crystal**. All five signal connections below require suitable
3.3 V / 5 V level translation for that module.

| Secondary pin | Direction | MCP2515 module pin | Function |
| --- | --- | --- | --- |
| GPIO25 | Output | SCK | SPI clock |
| GPIO23 | Output | MOSI / SI | SPI data to controller |
| GPIO22 | Input | MISO / SO | SPI data from controller |
| GPIO21 | Output | CS | Active-low chip select |
| GPIO32 | Input | INT | Active-low interrupt; connect even if initial firmware uses polling |

Use explicit SPI routing because the default ESP32 SPI pins GPIO18/19 conflict
with the modem. Future Arduino initialization for this assignment is:

```cpp
SPI.begin(25, 22, 23, 21); // SCK, MISO, MOSI, CS
```

The selected CAN library must use this configured SPI bus. Configure its oscillator
setting for **8 MHz** and select the CAN bitrate separately to match the target bus.

## Power and ground connections

| Connection | Supply / destination | Notes |
| --- | --- | --- |
| Secondary 3V3 | TEL0161 UART connector + / VCC | UART logic supply only |
| TEL0161 Power IN | Separate stable 5 V supply | Modem main power; do not power the modem from the ESP32 3V3 rail |
| MCP2515 module VCC | Regulated 5 V supply | Planned; never connect directly to vehicle 12 V |
| Common GND | Both ESP32 boards, TEL0161 GND, supply negatives and planned CAN module GND | Include the vehicle interface ground when connected |

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

There is no horn-control output on this module and no separate ESP32 horn GPIO
assigned. Horn control over CAN requires vehicle-specific messages and access to
the appropriate bus/controller. Vehicle year, make, model, bitrate and message
details remain to be established. The MCP2515 supports Classical CAN, not CAN FD.
Begin vehicle bring-up in listen-only mode before enabling transmission.

## Constraints for future pin allocation

- Preserve the current assignments and proposed CAN reservations above.
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
