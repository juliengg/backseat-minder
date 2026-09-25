# Cellular ESP32 Integration Guide

## Goal

Split cellular SMS handling across two ESP32 boards:

- **Primary ESP32-S3:** existing Backseat Minder camera, sensor, setup portal, and decision logic.
- **Secondary ESP32-WROOM-32 development board:** separate PlatformIO/Arduino firmware that communicates with the TEL0161/SIM7600G modem and sends SMS messages.

For the first prototype, pressing the primary board's BOOT button sends a `Testing` SMS through the secondary board. The primary sends the saved phone number with every SMS command. The secondary does **not** need to store the phone number in NVS yet.

## Existing Projects

The primary firmware is the ESP-IDF project in the repository root. Important files include:

- `main/app_main.cpp`: application initialization and main loop.
- `main/setup_mode.cpp`: captive portal and NVS configuration storage.
- `main/driver_presence.cpp`: current BOOT-button driver-presence/deep-sleep behavior.
- `main/mmwave_sensor.cpp`: existing UART1 use.

The secondary firmware is the existing PlatformIO project in `cell/`:

- `cell/platformio.ini`: PlatformIO ESP32-WROOM-32 Arduino build configuration (`board = esp32dev`).
- `cell/src/main.cpp`: current modem setup and BOOT-triggered SMS implementation.
- `cell/include/config.h`: current modem pins and hardcoded destination/message settings.
- `cell/README.md`: TEL0161 wiring and power requirements.

## Required First-Version Behavior

### Primary board

1. Keep the existing camera, thermal camera, mmWave sensor, temperature/humidity sensor, USB telemetry, and setup portal working.
2. Remove the current driver-presence toggle/deep-sleep behavior for now.
3. Configure the primary BOOT button as a simple debounced press event.
4. On a valid BOOT press/release, send a UART command to the secondary:

   ```text
   SEND_SMS|<saved phone number>|Testing\n
   ```

   Example:

   ```text
   SEND_SMS|+15129622126|Testing\n
   ```

5. Read the saved primary phone number from the existing NVS configuration (`bsm_cfg`, key `phone`). Do not duplicate phone-number storage on the secondary yet.
6. Log the secondary's acknowledgement and final result when available.
7. If no phone number is configured, do not send a command. Log a clear warning instead.
8. Keep the existing setup-mode button on GPIO 38 and the current sensor/camera behavior unchanged.

### Secondary board

1. Keep all TEL0161/SIM7600G modem communication on the secondary.
2. Replace the current BOOT-button trigger with a UART command listener from the primary.
3. Parse the `SEND_SMS|number|message` command.
4. Validate the command, number, and message length before touching the modem.
5. Send an acknowledgement after accepting a valid command:

   ```text
   ACCEPTED\n
   ```

6. Run the existing modem readiness checks and SMS transaction.
7. Report the result to the primary:

   ```text
   RESULT|OK\n
   RESULT|ERROR|<short reason>\n
   ```

8. Continue printing useful modem diagnostics to the secondary's USB serial monitor.
9. Do not forward arbitrary AT commands received over the inter-board UART.

## UART Protocol

Use a newline-terminated, human-readable protocol for the first prototype. UART settings should be:

```text
115200 baud, 8 data bits, no parity, 1 stop bit
```

Wiring is crossed:

```text
Primary TX -> Secondary RX
Primary RX <- Secondary TX
Primary GND -> Secondary GND
```

Implemented pin assignments (GPIO numbers, not header positions):

| Primary ESP32-S3 | Secondary ESP32-WROOM-32 |
| --- | --- |
| GPIO39 TX (UART2) | GPIO26 RX (UART2) |
| GPIO40 RX (UART2) | GPIO27 TX (UART2) |
| GND | GND |

Secondary GPIO18 RX connects to TEL0161 T/TX; secondary GPIO19 TX connects to
TEL0161 R/RX. Connect the UART connector's `+`/VCC to secondary 3V3 for its logic
level, and modem GND/supply negative to the common ground. Power the modem itself
through its separate Power IN terminal from a suitable 5 V supply, not from the
ESP32 3.3 V rail alone. Keep the
primary's microSD slot empty because it shares GPIO39/40. See `cell/README.md`
for the complete wiring and the `esp32_wroom_32` / `esp32_wroom_32_real_sms` builds.

Protocol rules:

- Every frame ends with `\n`.
- The receiver accumulates bytes until newline.
- Reject frames exceeding a fixed maximum length, for example 256 bytes.
- Ignore blank lines.
- Use `|` as the field separator.
- The first implementation may reject phone numbers or messages containing `|` rather than attempting unsafe parsing.
- Accept only the known command `SEND_SMS`.
- The primary should wait for `ACCEPTED` and then `RESULT`, with a timeout.
- The primary may retry only with an explicit policy; avoid blindly retrying after an uncertain modem result because that could send duplicate SMS messages.

The protocol is intentionally simple for hardware bring-up. A later version can add sequence IDs, checksums, escaping, and a formal state machine.

## Phone Number Handling

The setup portal already saves the primary number in NVS:

```text
Namespace: bsm_cfg
Key: phone
Maximum in-memory length: 31 characters plus null terminator
```

Add a small public API in the setup/configuration layer to retrieve the current saved phone number. Do not expose NVS internals directly in `app_main.cpp` if a clean helper can be added.

The number should be sent with each SMS command because this is the simplest stateless design:

```text
SEND_SMS|+15551234567|Testing
```

The secondary does not need NVS storage for the number in this phase. If independent operation is needed later, add a separate `SET_NUMBER` command and optional secondary-side NVS storage.

## Pin and Hardware Constraints

Do not assume any unused GPIO is available without checking the board pinout and current firmware.

Known primary-board usage includes:

- GPIO 0: BOOT button.
- GPIO 1: AM2302/DHT22 data.
- GPIO 2: status LED.
- GPIO 21 and GPIO 47: thermal-camera I2C.
- GPIO 38: setup button.
- GPIO 41/42: existing mmWave UART1.
- GPIO 48: onboard NeoPixel data.
- Camera-related GPIOs: controlled by the camera configuration and ESP-WHO components.
- GPIO 19/20: native USB D-/D+.

The inter-board UART must use two genuinely free primary GPIOs and must not interfere with the camera, sensors, USB, BOOT, setup button, or board strapping functions. Prefer a hardware UART with the ESP-IDF UART driver and explicit `uart_set_pin()` configuration. Do not reuse UART1 because the mmWave sensor already owns it.

The TEL0161 must have its own suitable power supply. Do not power it from the ESP32 3.3 V output. Keep the modem antenna attached during network testing. Confirm UART voltage compatibility and connect grounds between the secondary ESP32 and the modem.

## Suggested Implementation Order

### 1. Secondary UART command mode

- Add a second `HardwareSerial` or use the existing Arduino serial abstraction for the inter-board UART.
- Keep USB `Serial` for debugging.
- Remove or disable the secondary BOOT trigger.
- Add line buffering and command validation.
- Initially support only `SEND_SMS|number|Testing` or a general ASCII message field.

### 2. Primary UART transport

- Add a small primary-side module, preferably `main/cellular_link.cpp` and `main/cellular_link.h`.
- Initialize the selected UART and GPIO pins once during startup.
- Add a function such as `cellular_link_send_sms(number, message)`.
- Send the framed command and wait for responses without blocking the main loop indefinitely.

### 3. Primary BOOT event

- Replace the call to `driver_presence_init()` and the deep-sleep branch with a simple BOOT-button event implementation, or refactor `driver_presence.cpp` so it reports a debounced press without changing power state.
- Preserve press/release debouncing.
- On the event, retrieve the saved phone number and call the cellular-link function.
- Leave the device in the no-driver-present/normal active mode.

### 4. Saved number API

- Add a function that loads only the `phone` NVS value into a caller-provided buffer.
- Avoid logging the complete phone number in production-oriented logs.
- Treat an empty or malformed value as unavailable.

### 5. End-to-end test

- Flash the secondary firmware and verify it can communicate with the modem independently.
- Wire the primary and secondary UARTs with crossed TX/RX and common ground.
- Verify that pressing BOOT produces `SEND_SMS|...|Testing` on the secondary debug monitor.
- First run with real SMS sending disabled if possible.
- Enable real sending only after UART parsing and modem readiness checks work.
- Confirm acknowledgement, successful SMS delivery, malformed-command handling, and timeout behavior.

## Error Handling Requirements

The implementation should handle these cases without crashing or permanently blocking:

- Secondary is powered off or disconnected.
- UART wiring is reversed or missing a common ground.
- The primary has no saved phone number.
- The secondary receives a malformed or oversized frame.
- The modem has no SIM, network registration, SMS service, or adequate power.
- The modem takes longer than expected to respond.
- A button is held down or bounces.
- A second BOOT press occurs while an SMS is already being sent.

For the first version, reject a second trigger while a send is active and report `BUSY`. Keep the primary main loop responsive and use bounded timeouts for inter-board communication.

## Out of Scope for This First Pass

- Automatic SMS alerts based on face, mmWave, thermal, or temperature detection.
- Emergency-services behavior.
- Secondary NVS storage of the phone number.
- Secure authentication or encryption between boards.
- Deep-sleep power management.
- SMS delivery receipts or complex modem retry policy.
- Full binary framing or CRC.

## Acceptance Criteria

The first implementation is complete when:

1. Both projects build successfully with their intended toolchains.
2. The primary boots and remains in normal active mode; BOOT no longer enters driver-presence deep sleep.
3. The setup portal can save a phone number as before.
4. A BOOT press on the primary sends the saved number and `Testing` message over UART.
5. The secondary acknowledges, communicates with the TEL0161, and reports success or a bounded error.
6. The modem can send the SMS to the saved number.
7. Missing configuration, disconnected UART, malformed commands, and modem failure produce readable logs without reboot loops.
8. Existing camera, sensor, setup-mode, LED, and USB telemetry behavior remains intact except for the intentionally removed driver-presence/deep-sleep behavior.
