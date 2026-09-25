# Backseat Minder — Codebase Context

## Purpose

This repository contains ESP-IDF firmware for an **ESP32-S3 camera-based back-seat occupancy reminder prototype**. The indicator LED reflects the combined face, mmWave and thermal presence signals.

The project also provides an on-device captive-portal setup flow for collecting and persisting contact-related settings. A primary BOOT press/release can now request a test SMS through the secondary ESP32 in `cell/`. Real sending defaults off for bring-up. There are no automatic sensor-triggered SMS alerts, calls or emergency-services integrations.

## Platform and Build

- Target: `esp32s3`
- Framework: ESP-IDF `5.5.4`
- Project name: `backseat-minder`
- Build system: CMake / `idf.py`
- Main source directory: `main/`
- Application partition: 2 MB factory app (`partitions.csv`)

The root `CMakeLists.txt` also points `EXTRA_COMPONENT_DIRS` at a local ESP-WHO checkout. This means builds expect ESP-WHO to be available at the configured path, in addition to the managed components recorded in `dependencies.lock`.

Typical device workflow from the source comments:

```text
idf.py build
idf.py -p COM4 flash monitor
```

`COM4` is an example local serial port, not a portable project setting.

## Source Map

| File | Role |
| --- | --- |
| `main/app_main.cpp` | Firmware entry point; initializes setup support, camera/face detection, and the main LED loop. |
| `main/boot_button.cpp` | Debounced BOOT press/release event; no sleep or driver-presence toggle. |
| `main/cellular_link.cpp` | UART2 SMS worker, acknowledgement/result logging, busy guard and timeouts. |
| `main/cellular_diagnostics.cpp` | Thread-safe last-eight-event history, sent over native USB as BSMC JSON snapshots. |
| `shared/cellular_protocol.h` | Shared bounded framing and phone/message validation. |
| `cell/src/main.cpp` | Secondary UART command service and TEL0161 modem transactions. |
| `main/setup_mode.h` | Public interface for setup mode. |
| `main/setup_mode.cpp` | Captive portal, Wi-Fi access point, DNS redirection, form parsing, and NVS configuration storage. |
| `main/CMakeLists.txt` | Registers application source files and component dependencies. |
| `dependencies.lock` | Pinned ESP-IDF and Espressif component versions. |
| `partitions.csv` | Flash partition layout. |
| `sdkconfig` | Generated ESP-IDF configuration; treat as platform/build configuration rather than primary application logic. |

## Runtime Behavior

### Normal mode

1. `app_main()` calls `setup_mode_init()`.
2. GPIO 48 is driven low to suppress unintended light from the ESP32-S3 onboard WS2812/NeoPixel.
3. GPIO 2 is configured as the indicator LED output.
4. A FreeRTOS queue is created for camera frames.
5. The ESP-WHO camera pipeline is registered for RGB565/QVGA frames and face detection.
6. The application polls with a 20 ms delay between iterations:
   - checks BOOT press/release and queues `Testing` to the saved primary number;
   - checks whether GPIO 38 is being held, and enters setup mode if so;
   - every three seconds, updates in-memory `temperature_f` and `humidity_percent`
     values from an AM2302/DHT22 sensor on GPIO 1;
   - reads `get_tuned_face_detected()` and the mmWave/thermal presence signals;
   - sets GPIO 2 high when any of those signals indicates presence and low otherwise.

These presence signals drive the LED and USB telemetry; they do not automatically trigger SMS.

### Optional USB development telemetry

The device remains fully standalone. When its native **USB Serial/JTAG** USB-C port (left) is
connected to a host, `usb_telemetry.cpp` sends a telemetry packet every three seconds
after the temperature/humidity sample. Each packet includes uptime, face-detection
status, sensor validity, temperature in Fahrenheit, and relative humidity. Camera-frame
and telemetry packets are multiplexed over the same USB-OTG serial connection. No
external host is needed for normal operation, and a disconnected or slow host does not
block the main loop.

Use `tools/usb_telemetry_viewer.py` on a development laptop to display serial output.
It mirrors `idf.py monitor` output by default; use `--json-only` to display only the
formatted telemetry records. It requires `pyserial` (`python -m pip install pyserial`)
and a port name, for example:

```text
python tools/usb_telemetry_viewer.py --port COM7
```

On the Freenove ESP32-S3 WROOM board, use the connector labeled **USB-OTG**. It is
wired directly to the ESP32-S3's USB D- (GPIO 19) and D+ (GPIO 20) lines. Do not use
the **USB-UART** connector for this dedicated telemetry stream; that connector is
instead attached to UART0 through the board's USB-to-UART chip. Windows should expose
the USB-OTG connection as a COM port after the firmware is flashed (COM3).

For a development camera preview, install Pillow in addition to `pyserial`, then run:

```text
python -m pip install pillow
python tools/usb_telemetry_viewer.py --port COM5
```

The preview is JPEG-compressed and limited to one frame per second to avoid making USB
debugging alter normal face-detection behavior. It uses the detected-frame output, so
face boxes may be visible in the preview.

### Manual cellular SMS test

The firmware remains in normal active mode. BOOT (GPIO0) produces one event after
both press and release have been stable for 50 ms; a startup-held button is ignored
until released. Driver-presence toggling and deep sleep have been removed.

The primary reads and normalizes only `bsm_cfg/phone`, then queues
`SEND_SMS|number|Testing` through UART2 TX39/RX40 at 115200 8N1. A worker logs the
secondary acknowledgement and final result without blocking monitoring. Another
press while active logs `BUSY`. There are no automatic retries; after a 3-second
ACK timeout the link stays busy until a result or the 110-second overall deadline.

The secondary is an ESP32-WROOM-32 development board (`esp32dev` in PlatformIO).
Its UART2 RX26/TX27 connects to primary TX39/RX40 respectively. It owns modem UART1
RX18/TX19, validates commands, checks modem/SIM/network
readiness, and reports a bounded result. It sends the exact requested message and
stores no recipient. Real sending defaults off (`BSM_SEND_REAL_SMS=0`); a successful
dry-run check returns `RESULT|ERROR|DRY_RUN`. `RESULT|OK` means modem acceptance,
not proof of handset delivery.

See [cell/README.md](cell/README.md) for wiring, enablement, build commands, timeout
policy and bench acceptance checks. Keep the primary's microSD slot empty: GPIO39/40
share its bus. GPIO38 setup, UART1 mmWave, camera, other sensors and USB retain their
existing behavior. During the blocking setup session BOOT is not polled.

The Python USB viewer displays a Cellular SMS panel with the latest summary and
recent timestamped events. `usb_telemetry_send()` sends a `BSMC` JSON snapshot with
each sensor sample (about every three seconds), preserving short-lived ACK/results
without making the SMS worker wait on USB. No recipient or SMS content is included.
The panel marks telemetry stale after ten seconds without a valid cellular packet.

### Setup mode

Holding the external setup button on GPIO 38 enters a blocking setup session:

1. The device starts an **open** Wi-Fi access point:
   - SSID: `Backseat Minder`
   - Password: none
   - IP address/gateway: `192.168.4.1`
2. A lightweight UDP DNS server answers DNS queries with `192.168.4.1`, creating captive-portal behavior.
3. An HTTP server serves the setup page and redirects common Android, iOS/macOS, Windows, and Firefox connectivity-check URLs to the portal.
4. The portal accepts settings, saves them to NVS, displays a confirmation page, then ends setup.
5. The firmware tears down the Wi-Fi/AP resources and reboots.

While waiting for portal confirmation, GPIO 2 blinks every 200 ms.

## Persistent Configuration

Settings are stored in ESP32 nonvolatile storage (NVS):

- Namespace: `bsm_cfg`
- Primary phone number: `phone`
- Emergency contact 1: `ec1`
- Emergency contact 2: `ec2`
- Emergency contact 3: `ec3`
- Emergency-alert toggle: `emerg_alerts`

Each phone/contact field is capped at 31 characters plus a null terminator in memory. The setup page reloads saved values when it is reopened.

The primary `phone` field is read for each manual SMS request through `setup_mode_get_phone_number()`. The helper strips common phone formatting and rejects missing or malformed values. Emergency contacts and the emergency-alert toggle remain stored only.

## Hardware Assumptions

- Freenove ESP32-S3 WROOM with the custom camera pin map in `sdkconfig`.
- GPIO 0: active-low onboard BOOT button for a manual test SMS press/release.
- GPIO 39/40: cellular UART2 TX/RX; microSD slot must remain empty.
- GPIO 38: active-low external setup button, using the internal pull-up.
- GPIO 2: external/status LED output used for face-detection status and setup-mode blinking.
- GPIO 1: AM2302/DHT22 single-wire temperature and relative-humidity sensor data pin.
- GPIO 48: onboard WS2812/NeoPixel data pin, explicitly held low.

GPIO 4 cannot be used for the AM2302/DHT22 because the camera uses it as its SIOD control
line. The AM2302/DHT22 needs its data line pulled high (normally with an external 4.7–10 kOhm
resistor from data to the sensor supply). The firmware reads it every three seconds,
converts temperature to Fahrenheit, and logs successful readings. The values are not yet
used to change alert or face-detection behavior.

Pin assignments are hard-coded in the source. Confirm the board wiring before changing hardware or reusing the firmware on another ESP32-S3 board.

## Dependencies

Key dependencies locked in `dependencies.lock` include:

- `espressif/esp32-camera`
- `espressif/esp32_s3_eye_noglib`
- `espressif/mdns`
- ESP-IDF

The camera and face-detection entry points (`register_camera`, `register_human_face_detection`, and `get_face_detected`) come from ESP-WHO-related components, not from code implemented in this repository.

## Security and Product Notes

- Setup Wi-Fi is intentionally open. Anyone within range while setup mode is active can view and submit the configuration form.
- Setup logs no longer print the submitted form body or saved phone/contact values. Cellular logs redact the recipient.
- Form values are inserted into HTML without HTML escaping. Phone-style values are the intended input, but untrusted input could still affect the rendered page on a later setup visit.
- The emergency-alert wording in the UI is aspirational. There is no emergency-services integration in the current code.
- Face detection alone is not a reliable determination of a child, passenger, vehicle state, or emergency. Any real safety product needs additional sensors, failure handling, user testing, privacy design, and appropriate regulatory/legal review.

## Current Gaps / Likely Next Work

- Define the actual alert policy (when to alert, how long to wait, how to cancel, and failure behavior).
- Complete cellular bench verification and define any future automatic alert policy.
- Define runtime handling for emergency contacts and alert settings; only the primary phone is currently used.
- Add secure provisioning/access control and avoid logging personal data.
- Validate camera/face-detection accuracy and recovery behavior under real vehicle lighting, motion, heat, and network conditions.
- Extend the cellular host tests in `tests/` with hardware-in-the-loop verification.

## Guidance for Future LLM Work

- Prefer changing `main/app_main.cpp` for normal detection-loop behavior and `main/setup_mode.cpp` for provisioning/configuration behavior.
- Keep ESP-IDF lifecycle constraints in mind: the setup code intentionally keeps `esp_netif` and the default event loop initialized across setup sessions, while tearing down only the Wi-Fi driver and AP netif.
- `enter_setup_mode()` blocks until the form is submitted and then restarts the device. Treat it as a mode transition, not a non-blocking helper.
- Avoid assuming that stored settings result in notifications; verify runtime consumers before describing a feature as implemented.
- Avoid editing generated `build/` output and generally avoid manual edits to `sdkconfig` unless an ESP-IDF configuration change is intended.
