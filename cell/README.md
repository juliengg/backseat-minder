# Cellular SMS service

The primary ESP-IDF firmware handles sensors, camera, setup and the BOOT button.
This PlatformIO/Arduino firmware runs on an ESP32-WROOM-32 development board and owns the
TEL0161/SIM7600G modem. It receives the recipient and message with every command;
there is no hardcoded recipient, secondary NVS storage, or location lookup.

The primary sends `ALERT: {name}'s Backseat Minder device has detected an unattended passenger in their vehicle.` after a debounced BOOT press **and release**, using the required name saved in setup. The `Testing` commands below remain examples for bench testing the secondary directly.
Monitoring stays active. The secondary's BOOT button is only used for flashing.

## Wiring

Both inter-board UARTs use **UART2, 115200 baud, 8N1**, with 3.3 V logic:

| Primary ESP32-S3 | Secondary ESP32-WROOM-32 |
| --- | --- |
| GPIO39 TX | GPIO26 RX |
| GPIO40 RX | GPIO27 TX |
| GND | GND |

Keep the **primary's microSD slot empty** and leave its SD-card support disabled. The Freenove
board connects SD CLK to GPIO39 and SD D0 to GPIO40. These header pins are unused
by the current camera/sensor firmware; GPIO38 remains the setup button. UART1
GPIO41/42 remains assigned to mmWave on the primary, and UART0 remains the console.
Do not attach an external GPIO JTAG probe to the repurposed GPIO39-42 pins.

Pin assignments were checked against `sdkconfig`, `hardware/pcb/README.md`, the
[Freenove board pinout](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board/blob/main/ESP32S3_Pinout.png),
and [Freenove's SD wiring](https://docs.freenove.com/projects/fnk0086/en/latest/fnk0086/codes/tutorial/4_Read_and_Write_the_SDcard.html).

| TEL0161 UART | Secondary ESP32-WROOM-32 / supply |
| --- | --- |
| T / TX | GPIO18 RX (UART1) |
| R / RX | GPIO19 TX (UART1) |
| - / GND | Common GND |
| + / VCC (UART connector) | Secondary 3V3, for the UART logic level |

Power off before wiring. Use GPIO numbers printed on the board, not header position
numbers. The secondary UART pins are explicitly routed in software: use GPIO26/27
for the primary link and GPIO18/19 for the modem, even if the board labels another
pair RX/TX. GPIO1/3 remain available to its onboard USB-to-UART bridge.

Power each ESP32 development board through its own USB connector for bench testing.
Connect both ESP32 grounds, TEL0161 GND, and the modem supply negative together.
The inter-board connection needs only TX, RX and GND; do not tie the boards' 5 V or
3.3 V outputs together when independently USB-powered.

Use a separate stable 5 V supply at the TEL0161 **Power IN terminal**: supply positive
to Power IN positive, supply negative to Power IN negative/common ground. This is
separate from the UART connector's `+` pin. The
[TEL0161 schematic](https://dfimg.dfrobot.com/wiki/21783/TEL0161_sim7600g-4g-communication-module_schematics_V1.0.pdf)
shows UART VCC supplying the TX/RX pull-ups, so use the secondary's 3.3 V rail there
to match ESP32 logic. Do not apply 5 V to that UART VCC in this ESP32 wiring, and
do not try to run the modem from the ESP32 3.3 V rail alone. This corrects the earlier
README table that conflated UART VCC with the separate modem power input.

DFRobot specifies an external 5-12 V supply for UART operation. Keep the LTE MAIN
antenna attached during network testing. These connections apply to the TEL0161
carrier board, not a bare SIM7600 module. See the
[TEL0161 specifications](https://wiki.dfrobot.com/tel0161) and
[ESP32-WROOM development board pinout](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/hw-reference/esp32/get-started-devkitc.html).

## Build and flash

From an ESP-IDF 5.5.4 terminal at the repository root:

```text
idf.py build
idf.py -p PRIMARY_PORT flash monitor
```

From a PlatformIO terminal at the repository root:

```text
pio run -d cell
pio run -d cell -t upload --upload-port SECONDARY_PORT
pio device monitor -d cell --port SECONDARY_PORT
```

Alternatively open `cell/` in VS Code with PlatformIO and use Build, Upload and
Monitor. Keep the adjacent `shared/` directory: both projects compile the same
protocol definitions. Use the secondary's **USB-UART** connector for the existing
`Serial` diagnostics at 115200 baud. Neither firmware is flashed by the host tests.

`include/config.h` defaults `BSM_SEND_REAL_SMS` to **0**. Dry-run mode performs
modem/SIM/network/SMS-mode checks but never issues `AT+CMGS` or submits a message.
It reports `RESULT|ERROR|DRY_RUN`, so it cannot be mistaken for real SMS success.
After verifying the hardware, explicitly select the real-SMS environment:

```text
pio run -d cell -e esp32_wroom_32_real_sms
pio run -d cell -e esp32_wroom_32_real_sms -t upload --upload-port SECONDARY_PORT
```

That environment sets `BSM_SEND_REAL_SMS=1`; the default environment remains a dry
run. Both use Espressif32 platform 7.1.3, the version used for build verification.
The default dry-run environment is `esp32_wroom_32`, using `board = esp32dev`.
The old `freenove_esp32_s3` environment names and S3 firmware binaries must not be
used for this secondary board. Primary ESP-IDF build/flash commands are unchanged.

Local verification tools, when installed by the coding agent, are under the
ignored `.venv-cell/` and `.pio-core/` directories. To use that PlatformIO copy in
PowerShell, set `$env:PLATFORMIO_CORE_DIR = "$PWD/.pio-core"` and replace `pio` above
with `./.venv-cell/Scripts/python.exe -m platformio`.

## Protocol and failure policy

The Python viewer (`python tools/usb_telemetry_viewer.py --port PRIMARY_USB_PORT`)
shows a **Cellular SMS** panel over the primary's existing native USB connection.
Flash the updated primary firmware and restart the viewer. Summaries update with
the sensor telemetry, approximately every three seconds, and retain the last eight
events so acknowledgements and fast final results remain visible. The panel covers
missing configuration, acknowledgement, SIM/network/modem errors, dry-run, success,
BUSY and timeouts. It marks telemetry stale after ten seconds without an update.
Detailed AT responses remain on the secondary serial monitor; phone numbers and
SMS contents are not included in viewer diagnostics. Older primary firmware still
shows cameras/sensors but leaves this panel waiting for cellular telemetry.

```text
SEND_SMS|+15551234567|Testing\n
ACCEPTED\n
RESULT|OK\n
```

Errors are `RESULT|ERROR|<reason>\n`. Both LF and CRLF input are accepted. Blank
lines are ignored. Frames have at most 256 bytes before LF, including optional CR;
oversized/invalid frames are discarded through the next LF before parsing resumes.
The receiver accepts only `SEND_SMS`; arbitrary AT passthrough is unavailable.

Numbers contain 7-15 digits and an optional leading `+` (international numbers
cannot begin with zero). The primary reads only `bsm_cfg/phone`, strips spaces,
hyphens, parentheses and periods from the stored value, and validates the result.
It preserves the saved portal value and skips sending if it is absent or invalid.
Use international format with country code for reliable routing. Logs redact the
recipient and no longer print submitted setup contact fields.

Messages must be nonempty printable GSM-compatible ASCII, without `|`, backtick,
CR, LF, Ctrl-Z, Escape or UTF-8. The limit is 160 GSM septets; `^{}[]\~` each consume
two septets. Messages are sent exactly as received, without a location suffix.

- The primary queues the request to a FreeRTOS worker; camera/sensor processing
  does not wait for the modem. A second trigger while active logs `BUSY`.
- The secondary continues servicing UART during all modem waits and rejects
  additional commands with `RESULT|ERROR|BUSY`.
- An acknowledgement timeout is logged at 3 seconds. Because the ACK might be
  lost after submission, the primary remains busy until a result or the full
  110-second deadline. A disconnected secondary cannot block monitoring.
- Secondary modem work is bounded by a 100-second transaction deadline, including
  modem boot wait, readiness checks and submission. SIM/network errors are returned
  explicitly; unsupported or unavailable SMS service returns a modem error.
- A timeout after Ctrl-Z returns `SMS_OUTCOME_UNKNOWN` after draining late modem
  bytes for the remaining transaction window. Check the recipient before another
  deliberate press, since the SMS may already have been sent. There are **no
  automatic SMS retries**. Later manual presses start new requests.
- `RESULT|OK` means the modem returned both `+CMGS` and final `OK`; it is not a
  handset delivery receipt. The protocol has no sequence IDs; use one primary and
  one outstanding request, and avoid resetting either board during a transaction.

## Bench acceptance checks

1. Build and flash both boards with dry-run enabled. Power the modem separately,
   wire the crossed UARTs and grounds, and leave the primary's microSD slot empty.
2. With no saved phone, press/release primary BOOT: verify the configuration warning
   and no secondary command. GPIO38 should still open setup and save a phone.
3. Save an international phone number and reboot through setup. Press/release BOOT:
   primary logs `ACCEPTED`; secondary logs `SEND_SMS|<redacted>|Testing`, modem
   diagnostics and `DRY_RUN` if ready. Hold and bounce BOOT to check one event per
   valid press/release. Camera, thermal, mmWave, DHT, LED and USB should keep working.
4. Press BOOT again while busy: it must not queue another SMS. Disconnect the
   secondary: expect an ACK warning and final timeout, with monitoring responsive.
5. With the primary disconnected, use a 3.3 V USB-UART adapter on secondary RX26/TX27
   to inject unknown commands, missing fields, bad numbers, empty/oversized messages,
   control bytes and >256-byte lines. Expect bounded errors and recovery on the next
   valid line; test a second command during a modem wait for `BUSY`.
6. Test missing SIM, no network and powered-off modem. Confirm an error without a
   reboot loop and recovery on a later deliberate request after correcting the fault.
7. Enable real SMS, rebuild/flash, and press/release primary BOOT. Verify `RESULT|OK`
   and receipt of exactly `Testing` at the saved number.

## Automated checks

Run `python tests/run_cellular_tests.py` from the repository root with a host C++
compiler on PATH (`CXX` can select it). On this Windows workspace, the installed
project-local compiler can be used with:

```text
./.venv-cell/Scripts/python.exe tests/run_cellular_tests.py --zig
```

Tests compile the actual protocol, primary transport, secondary service and BOOT
implementation with fake UART/modem/GPIO interfaces. They cover dry-run and
real-submit paths, frame validation/recovery, copied async requests, BUSY handling,
SIM/network faults, disconnected-UART/ACK/result timeouts, no retries and debounce.
They do not verify physical wiring, power, RF service, SMS delivery or sensor output.

Build verification passed for ESP-IDF 5.5.4 and both PlatformIO environments. The
host suite passes for both secondary variants and the primary transport, including
bounded diagnostic history and JSON validation. Viewer tests cover status summaries,
acknowledgement/result history, dry-run wording and malformed telemetry.
Bench acceptance remains pending; no boards were flashed during implementation.
