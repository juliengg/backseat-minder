# Backseat Minder — Codebase Context

## Purpose

This repository contains ESP-IDF firmware for an **ESP32-S3 camera-based back-seat occupancy reminder prototype**. Its implemented safety signal is face detection: when the camera AI pipeline detects a face, the device turns on an indicator LED.

The project also provides an on-device captive-portal setup flow for collecting and persisting contact-related settings. It does **not** currently send messages, place calls, contact emergency services, or make an automated safety decision beyond setting the LED state.

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
| `main/setup_mode.h` | Public interface for setup mode. |
| `main/setup_mode.cpp` | Captive portal, Wi-Fi access point, DNS redirection, form parsing, and NVS configuration storage. |
| `main/CMakeLists.txt` | Registers the two application source files. |
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
6. Every 100 ms, the application:
   - checks whether GPIO 0 is being held, and enters setup mode if so;
   - every three seconds, updates in-memory `temperature_f` and `humidity_percent`
     values from an AM2302/DHT22 sensor on GPIO 1;
   - reads `get_face_detected()` from the face-detection component;
   - sets GPIO 2 high when a face is detected and low otherwise.

Face detection is therefore the only active occupancy-related behavior in the checked-in application code.

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
the USB-OTG connection as a COM port after the firmware is flashed.

For a development camera preview, install Pillow in addition to `pyserial`, then run:

```text
python -m pip install pillow
python tools/usb_telemetry_viewer.py --port COM5
```

The preview is JPEG-compressed and limited to one frame per second to avoid making USB
debugging alter normal face-detection behavior. It uses the detected-frame output, so
face boxes may be visible in the preview.

### Setup mode

Holding the button on GPIO 0 enters a blocking setup session:

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

These fields are currently **stored only**. No application logic consumes them after setup mode saves them.

## Hardware Assumptions

- ESP32-S3 device with a supported camera, likely aligned with ESP-WHO ESP32-S3-EYE support.
- GPIO 0: active-low setup button, using the internal pull-up.
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
- The portal logs its complete submitted form body and saved phone/contact values to the serial log. These are personally sensitive values and should be removed or redacted before production use.
- Form values are inserted into HTML without HTML escaping. Phone-style values are the intended input, but untrusted input could still affect the rendered page on a later setup visit.
- The emergency-alert wording in the UI is aspirational. There is no emergency-services integration in the current code.
- Face detection alone is not a reliable determination of a child, passenger, vehicle state, or emergency. Any real safety product needs additional sensors, failure handling, user testing, privacy design, and appropriate regulatory/legal review.

## Current Gaps / Likely Next Work

- Define the actual alert policy (when to alert, how long to wait, how to cancel, and failure behavior).
- Add a communications mechanism (for example, a companion phone app, cellular modem, or cloud service) if notifications are required.
- Read and use saved contact and alert settings in the runtime logic.
- Add secure provisioning/access control and avoid logging personal data.
- Validate camera/face-detection accuracy and recovery behavior under real vehicle lighting, motion, heat, and network conditions.
- Add tests or hardware-in-the-loop verification; none are present in this repository.

## Guidance for Future LLM Work

- Prefer changing `main/app_main.cpp` for normal detection-loop behavior and `main/setup_mode.cpp` for provisioning/configuration behavior.
- Keep ESP-IDF lifecycle constraints in mind: the setup code intentionally keeps `esp_netif` and the default event loop initialized across setup sessions, while tearing down only the Wi-Fi driver and AP netif.
- `enter_setup_mode()` blocks until the form is submitted and then restarts the device. Treat it as a mode transition, not a non-blocking helper.
- Avoid assuming that stored settings result in notifications; verify runtime consumers before describing a feature as implemented.
- Avoid editing generated `build/` output and generally avoid manual edits to `sdkconfig` unless an ESP-IDF configuration change is intended.
