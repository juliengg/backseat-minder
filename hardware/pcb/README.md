# Backseat Minder PCB Carrier Concept

## Goal

Turn the current breadboard prototype into a compact carrier PCB that accepts the existing modules without redesigning every sensor board. The goal is to preserve the proven firmware interface while giving the project a clean, serviceable, production-friendly layout.

This is the lowest-risk, fastest path from a breadboard prototype to a real PCB because it keeps the current module ecosystem intact and only adds a fixture that mounts and routes signals cleanly.

---

## Hardware inventory from the current firmware

The live firmware uses these module paths and signal assignments:

| Module | Interface | Current use | Important notes |
| --- | --- | --- | --- |
| ESP32-S3 board | MCU | Main controller | Mounts as the brains of the carrier board |
| DHT22 / AM2302 | 1-wire GPIO | Temperature + humidity | Data pin on GPIO 1; requires 4.7–10 kΩ pull-up to 3.3 V |
| mmWave C4001 / UART presence sensor | UART1 | Occupancy detection | RX=GPIO 41, TX=GPIO 42 |
| MLX90640 thermal camera | I2C0 | Heat-trace sensing | SDA=GPIO 21, SCL=GPIO 47 |
| USB camera / ESP-WHO camera | CSI / camera bus | Face detection | Keep camera power and signal routing clean, away from noisy UART signals |
| LED | GPIO | Status indicator | GPIO 2 |
| Setup button | GPIO 38 | Config mode | Active-low; board can include pull-up |
| USB OTG | Native USB | Telemetry/debug | Keep D+ / D- routed as a differential pair |

---

## Recommended board approach

### 1) Use a carrier PCB, not a full custom sensor redesign

The safest first PCB is a multi-header carrier board that accepts the same module form factors already used on the breadboard. This gives a practical route:

- keep the current modules and firmware working;
- reduce wiring errors and fragility;
- add a mechanical base that can be mounted in a vehicle, enclosure, or test rig;
- make debugging easier than a conventionally hand-wired breadboard.

### 2) Use 2.54 mm pin headers for module compatibility

All current modules appear to be breadboard-friendly breakout boards. Use the same 2.54 mm male/female arrangement and pin headers on the PCB so each board plugs in directly.

Recommended layout:

- one large header block for the ESP32-S3 board;
- separate 3-pin or 4-pin headers for the mmWave sensor, DHT22, and any other sensor breakout;
- ground and 3.3 V bus rails routed around the board;
- a compact power input section capable of accepting 5 V or 3.3 V source.

### 3) Add a small regulator section

Most breadboard modules are intended for 3.3 V logic. The board should include:

- 5 V input connector (or barrel jack if desired);
- 3.3 V LDO or buck converter with proper decoupling;
- local 100 nF + 10 µF decoupling close to each sensor connector;
- optional reverse-polarity and transient protection for automotive/bench use.

---

## Recommended PCB topology

### Power plan

- Input power to board: 5 V from USB or external supply
- Regulated rail: 3.3 V to all logic modules
- Ground plane: continuous, low-impedance plane across the board
- Place decoupling capacitors close to each sensor connector and at the ESP32-S3 power pins

### Signal grouping

- Digital logic / GPIO: LED, setup button, DHT22 data
- UART: mmWave sensor on UART1
- I2C: MLX90640 thermal sensor
- High-speed camera / CSI routing: keep away from UART and 5 V switching noise
- USB differential pair: route as short, tightly coupled pair

### Mechanical placement

- Keep the camera and thermal sensor aimed outward, with the board center aligned to the module viewing direction
- Place mmWave sensor away from the USB and camera routing, ideally with a clear area in front of its antenna
- Keep the ESP32-S3 board near the middle of the carrier to avoid long GPIO traces
- Reserve mounting holes for enclosure screws and a standoff pattern

---

## Proposed carrier-board connector map

The exact footprint depends on the final ESP32-S3 board variant, but this is the pin plan to freeze before routing.

| Net | Signal | GPIO / rail | Connected to | Notes |
| --- | --- | --- | --- | --- |
| 3V3 | Power | 3.3 V rail | ESP32-S3, DHT22, sensors | Main logic rail |
| GND | Ground | GND | All modules | Ground plane required |
| 5V_IN | Power | 5 V input | Board input | Optionally regulated down to 3.3 V |
| GPIO38 | Setup button | GPIO 38 | Button net | Active-low input; keep microSD slot empty |
| GPIO2 | Status LED | GPIO 2 | LED anode/cathode | Add current-limiting resistor |
| GPIO1 | DHT22 data | GPIO 1 | DHT22 data | 4.7–10 kΩ pull-up to 3.3 V |
| GPIO41 | mmWave RX | UART1 RX | Sensor TX | Keep away from camera signals |
| GPIO42 | mmWave TX | UART1 TX | Sensor RX | Keep away from camera signals |
| GPIO21 | MLX SDA | I2C0 SDA | Thermal camera | Keep trace short |
| GPIO47 | MLX SCL | I2C0 SCL | Thermal camera | Keep trace short |
| GPIO19 | USB D- | USB OTG D- | USB-C / USB connector | Differential pair |
| GPIO20 | USB D+ | USB OTG D+ | USB-C / USB connector | Differential pair |
| GPIO48 | NeoPixel | GPIO 48 | Optional onboard RGB LED | Pulled low in firmware |

---

## PCB design rules

- 2-layer board is sufficient for a first revision
- Use 0.2–0.3 mm trace width for logic signals; 0.5+ mm for power routing
- Use continuous ground pour underneath digital sections where possible
- Keep I2C traces short and routed parallel, with a ground reference nearby
- Add 100 nF decoupling at each sensor connector
- Add 10 µF bulk capacitance near the regulator output
- If you need a USB-C connector, use the USB 2.0 routing and 5.1 kΩ pull-downs on CC pins as required

---

## Wiring checklist for the prototype board

Before you order the board, confirm all of the following:

1. The ESP32-S3 module pinout matches the chosen board footprint.
2. DHT22 data uses a proper pull-up and not a floating line.
3. The mmWave UART pins are not shared with the USB or boot pins.
4. MLX90640 I2C pins are not used by other peripherals.
5. The camera module connection is valid for the board variant in use.
6. The board has enough clearance for connectors and enclosure mounting.

---

## Suggested build sequence

1. Create a 2-layer carrier board with all current module connectors and a central ESP32-S3 socket.
2. Confirm the carrier board works on a bench with all current modules plugged in.
3. Move the sensor and indicator modules to their final mechanical positions.
4. Only then consider combining some sensors into a single custom PCB if a second revision is needed.

This path gives the lowest technical risk and makes the hardware easier to debug than a fully custom machining or single-board redesign.

---

## Next step

The next useful artifact is a KiCad-ready pin mapping and a board netlist. That can be generated directly from this plan and then turned into a schematic using either:

- a breadboard-compatible header layout if you want a quick prototype; or
- a more compact custom board if you want a permanent chassis design.

---

## Pin inventory for this project

The project does not currently keep a single formal pin registry file, but the following mappings are the source of truth in the firmware and are the ones to use when designing the PCB.

### Currently assigned pins

| Purpose | GPIO / pin | Source | Notes |
| --- | --- | --- | --- |
| Setup button | GPIO 38 | [main/setup_mode.cpp](../../main/setup_mode.cpp) | Active-low input |
| Status LED | GPIO 2 | [main/app_main.cpp](../../main/app_main.cpp) | Used for detection state |
| DHT22 / AM2302 data | GPIO 1 | [main/temp_humidity_sensor.cpp](../../main/temp_humidity_sensor.cpp) | Single-wire sensor data |
| mmWave RX | GPIO 41 | [main/mmwave_sensor.cpp](../../main/mmwave_sensor.cpp) | UART1 RX |
| mmWave TX | GPIO 42 | [main/mmwave_sensor.cpp](../../main/mmwave_sensor.cpp) | UART1 TX |
| MLX90640 SDA | GPIO 21 | [main/thermal_camera.cpp](../../main/thermal_camera.cpp) and [main/thermal_camera.h](../../main/thermal_camera.h) | I2C0 SDA |
| MLX90640 SCL | GPIO 47 | [main/thermal_camera.cpp](../../main/thermal_camera.cpp) and [main/thermal_camera.h](../../main/thermal_camera.h) | I2C0 SCL |
| USB D- | GPIO 19 | [main/usb_telemetry.cpp](../../main/usb_telemetry.cpp) | USB OTG data negative |
| USB D+ | GPIO 20 | [main/usb_telemetry.cpp](../../main/usb_telemetry.cpp) | USB OTG data positive |
| Onboard NeoPixel | GPIO 48 | [main/app_main.cpp](../../main/app_main.cpp) | Held low in firmware to suppress stray light |

### Camera-reserved pins

These pins are not available for general-purpose use on the Freenove ESP32-S3 WROOM because the onboard camera occupies them and the project code already treats them as camera-related:

- GPIO 4
- GPIO 5
- GPIO 6
- GPIO 7
- GPIO 8
- GPIO 9
- GPIO 10
- GPIO 11
- GPIO 12
- GPIO 13
- GPIO 14
- GPIO 15
- GPIO 16
- GPIO 17
- GPIO 18

The project source explicitly notes GPIO 4 is used by the camera in [main/temp_humidity_sensor.cpp](../../main/temp_humidity_sensor.cpp), and the board pinout confirms the camera-side GPIOs are reserved on the Freenove board.

### Free GPIOs for later use

GPIO 39 and 40 are unused by the current app, but share the onboard microSD bus. GPIO 38 is now assigned to the setup button and also shares that bus. Keep the microSD slot empty and do not enable SD-card support while these pins are repurposed.

GPIO 43/44 are UART0 TX/RX, used by the configured serial console and onboard USB-to-UART bridge. GPIO 45/46 are boot-strapping pins, so they are not unrestricted expansion pins.

### External setup button wiring

Connect a normally-open momentary button between **GPIO 38** (the header pin labeled IO38) and **GND**. Firmware enables the internal pull-up: released is HIGH, pressed is LOW. An optional 10 kOhm pull-up from GPIO 38 to 3.3 V can be fitted on the carrier. Do not connect the button input to 5 V.

GPIO 38 is unused by this application's peripherals and is not a boot-strapping pin. Its board connection is microSD CMD, so the SD slot must remain empty. The existing 50 ms debounce and setup portal behavior are preserved. Hold the external button until setup starts; the onboard BOOT button remains for bootloader entry and no longer requests setup mode.

References: [Freenove board pinout](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board/blob/main/ESP32S3_Pinout.png), [Freenove microSD pin assignments](https://docs.freenove.com/projects/fnk0086/en/latest/fnk0086/codes/tutorial/4_Read_and_Write_the_SDcard.html), and [Espressif GPIO restrictions](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32s3/api-reference/peripherals/gpio.html).

### GPIOs to avoid unless you intentionally redesign the board

- GPIO 0, 1, 2, 3, 4, 19, 20, 21, 38, 41, 42, 43, 44, 45, 46, 47, 48
- all camera-reserved pins listed above
- any pin tied to the board boot/USB/camera functions

This keeps the first PCB revision simple and avoids conflicts with the current firmware.

---

## Practical recommendation

For this project, I would build a first revision as a modular carrier board with:

- ESP32-S3 breakout mounted in the center;
- 2.54 mm headers for all current modules;
- dedicated 3.3 V and GND bus rails;
- a USB connector for debug/telemetry;
- an onboard LED and setup button footprint;
- a compact sensor placement strategy for the camera and mmWave module.

This keeps the project faithful to the software while making the hardware stable enough for real use.
