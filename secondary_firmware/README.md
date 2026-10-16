# Secondary SMS and CAN services

The primary ESP-IDF firmware handles sensors, camera, setup and dashboard commands.
This PlatformIO/Arduino firmware runs on an ESP32-WROOM-32 development board and owns the
TEL0161/SIM7600G modem and MCP2515 CAN controller. It receives the recipient and message with every SMS command;
there is no hardcoded recipient, secondary NVS storage, or location lookup.

The primary sends `Backseat Minder test for {name}. This checks delivery of passenger alerts. No action is needed.` when **Send SMS** is clicked in the Python telemetry dashboard, using the name and phone number saved in setup. The `Testing` commands below remain examples for bench testing the secondary directly.
Monitoring stays active. The primary BOOT button now requests a horn chirp; the
secondary BOOT button is only used for flashing.

## Code organization

`src/main.cpp` only initializes services and runs their polling functions.
Implementations live in `src/services/`:

- `sms_service.cpp/.h`: modem initialization, bounded SMS sending and incoming SMS reception.
- `received_sms.h`: bounded GSM-7/UCS2 SMS-DELIVER PDU decoder.
- `can_horn.cpp/.h`: MCP2515 initialization and the tested Q50 horn sequence.
- `primary_link.cpp/.h`: shared UART framing, command dispatch and BUSY replies.

`include/config.h` owns board pin assignments. `../shared/cellular_protocol.h`
defines the commands, request type and timeouts shared with the primary.

## Horn control

Flash **both boards** with these updates. Press and release the **primary** BOOT
button once to request one chirp. Its existing debounce requires stable press and
release for 50 ms, ignores a button held at startup, and does not repeat while held.
It queues only a short UART transaction; primary camera/sensor work keeps running.
There is no automatic horn trigger at startup or from detection.

The primary sends `HORN_CHIRP\n` over the same UART2 link as SMS. Replies are
`HORN_ACCEPTED\n`, then `HORN_RESULT|OK\n` or `HORN_RESULT|ERROR|reason\n`.
The typed replies keep horn results separate from existing SMS `ACCEPTED`/`RESULT`
responses. The primary waits up to 500 ms for acknowledgement and 3 seconds for
a final result, with no automatic retry.

The secondary uses the user-tested 2015 Infiniti Q50 sequence: standard ID `0x358`,
DLC 8, 500 kbps CAN, 8 MHz MCP oscillator and 1 MHz SPI. ON payload
`42 02 00 00 00 00 00 00` is submitted at 0, 20, 120 and 220 ms; OFF payload
`02 02 00 00 00 00 00 00` at 240 ms. Timing is relative to sequence start and is
best effort while servicing UART input. No CAN frame is sent at initialization.
Use the [planned GPIO wiring](PINOUT.md#mcp2515-can-assignments), not the original
standalone sketch's pin assignments. Keep module J1 open and remove the extra
120-ohm bench resistor on the vehicle connection, as in the tested sketch.

SMS and horn share **one transaction slot** on each board. A command received while
either action is active is rejected as BUSY, never queued for later. UART input is
serviced during modem waits and the 240 ms chirp. The dashboard's SMS button is
disabled while either action is active. `HORN_*` events appear in its existing history.
`sms_busy` is retained for compatibility but now means either UART action is busy.

MCP initialization failure returns `CAN_UNAVAILABLE` without preventing SMS.
The OFF frame is attempted even after an ON send error. Any reported send error
resets the MCP to clear pending frames and disables CAN until the secondary is
restarted (or `can_horn::begin()` is explicitly called again). `CAN_SEND_FAILED`
means at least one ON submission failed; `CAN_OFF_FAILED` means OFF submission
failed. `HORN_RESULT|OK` means the library accepted all five submissions, not that
the vehicle action was observed. Detailed results and TEC/REC/EFLG are on the
secondary serial monitor. The existing SMS dry-run option applies only to SMS;
**horn commands transmit real CAN frames in both build environments**.

## Dashboard SMS control

Build and flash the updated **primary** firmware, then launch
`python tools/usb_telemetry_viewer.py --port PRIMARY_USB_PORT`. Click **Send SMS**
in the Cellular SMS panel. Its existing dry-run/real-SMS setting still determines whether a text is
actually submitted.

The viewer sends `SEND_SMS\n` over the primary's native USB connection. The primary
accepts only that exact command (LF or CRLF), reads the saved contact details and
uses the existing asynchronous cellular service. USB commands cannot override the
recipient or message. The physical BOOT button no longer triggers an SMS; GPIO38
still opens setup.

The button remains disabled until compatible primary firmware supplies fresh
cellular telemetry, while a command awaits confirmation, and while an SMS or horn is busy.
The existing Cellular SMS history shows configuration errors, acknowledgement and
final results. Missing confirmation or a failed USB write is an unknown outcome,
never a success or an automatic retry. Check the recipient before retrying manually.
Cellular snapshots include `sms_control`, `sms_busy` and `sms_requests`; the request
counter confirms USB receipt even when successive requests produce identical errors.

## Incoming SMS in the Python log

Build and flash **both boards**, then restart the updated Python viewer. After the
modem's boot wait, the secondary configures incoming SMS automatically, including
in the default dry-run build. Wait for **Incoming SMS reception ready** in the
Cellular SMS history, then text the SIM's phone number from your phone. The same
log shows **Received SMS from +number**, followed by the message body. Device uptime
is shown beside the event; camera/sensor telemetry continues while waiting.

The secondary enables stored-message notifications (`AT+CNMI=2,1,0,0,0`), queues
`+CMTI` notifications even during outgoing SMS, and reads each notified SM/ME slot
using `AT+CMGR` in PDU mode. GSM-7 (including extension characters), UCS2/UTF-16
text, emoji and line breaks are decoded to UTF-8. Long concatenated SMS parts are
shown as individual entries. Binary SMS and alphanumeric sender IDs are unsupported
and produce a receive error. This implementation displays new notifications; it
does not import the modem's pre-existing inbox.

Received text is display-only and cannot trigger SMS, horn or setup commands.
The UART event is `RECEIVED_SMS|sender|hex-encoded-UTF8\n`; receive setup/errors use
`SMS_RX_STATUS|RX_READY`, `RX_ERROR`, or `RX_QUEUE_FULL`. The primary listens while
idle and during outgoing requests, then includes `SMS_RECEIVED` events with
`sender` and `message` in its JSON-escaped `BSMC` telemetry (up to 10,000 bytes).
The existing eight-event history is kept in primary RAM, replaces older events,
and clears when the primary restarts. It is not a persistent SMS archive.

After forwarding a decoded SMS to the primary, the secondary deletes that specific
modem slot to keep storage available. Undecodable SMS stays in storage. Reading
and deleting use bounded background commands; a send requested while one owns the
modem returns BUSY. Setup retries after failure without sending a text. Protocol
details follow the [SIMCom AT command manual](https://simcom.ee/documents/SIM7600E/SIM7500_SIM7600%20Series_AT%20Command%20Manual%20_V1.10.pdf).

Bench check: send `Hello`, then a multiline reply containing `OK`, `ERROR`, a pipe,
quotes and an emoji. Confirm the sender/body appear once, and repeat while an
outgoing SMS is in progress. Actual carrier delivery still requires hardware testing.

## Wiring

See the [secondary board pin reference](PINOUT.md) for all current assignments,
board-reserved pins, power connections and MCP2515 CAN assignments.

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

### Standalone MCP2515 bench diagnostic

Build/upload environment `esp32_wroom_32_mcp_diagnostic` to the secondary only.
This temporarily replaces its application: no primary UART commands, horn service,
or modem/SMS service runs. No external CAN frames are intentionally transmitted;
tests use configuration mode and internal loopback only. The CL2000 should not
see the internal test frames. A CAN bus, termination, and acknowledgement are not
needed for these tests.

```text
pio run -d secondary_firmware -e esp32_wroom_32_mcp_diagnostic
pio run -d secondary_firmware -e esp32_wroom_32_mcp_diagnostic -t upload --upload-port COM5
pio device monitor -d secondary_firmware --port COM5 --baud 115200
```

At startup (or on serial character `r`) it tests SPI at 100 kHz and 1 MHz.
Each speed runs 300 timing-register write/read pairs plus configuration-mode
checks. If those pass, it checks 16 internally looped-back frames for matching
ID, length, and data. It requests configuration mode when finished.

- Register failures: investigate power, ground, SPI routing, level translation,
  connections, and the module before diagnosing the external CAN bus.
- Only the slower speed passes: investigate SPI signal integrity/translation.
- Register test passes but loopback fails: investigate controller/oscillator and
  module hardware. Loopback does not verify the actual crystal frequency.
- Both pass: focus subsequent checks on transceiver power, CAN-H/L wiring,
  termination, actual crystal frequency/bitrate, and logger configuration.

Restore normal operation by uploading the previously used application environment
(`esp32_wroom_32_real_sms` for the real-SMS firmware, or `esp32_wroom_32` for dry-run).
The diagnostic does not test INT wiring or the external CAN transceiver.

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
The receiver accepts `SEND_SMS|number|message` and the exact `HORN_CHIRP` command;
arbitrary AT passthrough and arbitrary CAN frames are unavailable.

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
2. With no saved phone, click **Send SMS** in the dashboard: verify the configuration warning
   and no secondary command. GPIO38 should still open setup and save a phone.
3. Save a name and international phone number and reboot through setup. Click **Send SMS**:
   primary logs `ACCEPTED`; secondary logs a redacted `SEND_SMS` command, modem
   diagnostics and `DRY_RUN` if ready. Check that BOOT requests a horn chirp without sending an SMS.
   Camera, thermal, mmWave, DHT, LED and USB should keep working.
4. Check that **Send SMS** is disabled while busy and repeated clicks do not queue
   another SMS. Disconnect the
   secondary: expect an ACK warning and final timeout, with monitoring responsive.
5. With the primary disconnected, use a 3.3 V USB-UART adapter on secondary RX26/TX27
   to inject unknown commands, missing fields, bad numbers, empty/oversized messages,
   control bytes and >256-byte lines. Expect bounded errors and recovery on the next
   valid line; test a second command during a modem wait for `BUSY`.
6. Test missing SIM, no network and powered-off modem. Confirm an error without a
   reboot loop and recovery on a later deliberate request after correcting the fault.
7. Enable real SMS, rebuild/flash, and click **Send SMS**. Verify `RESULT|OK`
   and receipt of the named test message at the saved number.

## Automated checks

Run `python tests/run_cellular_tests.py` from the repository root with a host C++
compiler on PATH (`CXX` can select it). On this Windows workspace, the installed
project-local compiler can be used with:

```text
./.venv-cell/Scripts/python.exe tests/run_cellular_tests.py --zig
```

Tests compile the actual USB command receiver, protocol, primary transport,
secondary services and BOOT debounce helper with fake USB/UART/modem/GPIO/SPI/MCP interfaces. They cover dry-run and
real-submit paths, frame validation/recovery, copied async requests, BUSY handling,
SIM/network faults, disconnected-UART/ACK/result timeouts, no retries and debounce.
They also cover bounded USB polling, fragmented commands, malformed input recovery
and command receipt telemetry. Viewer tests cover button eligibility, duplicate
clicks, command confirmation, stale telemetry and USB write failures. They do not
verify physical wiring, power, RF service, SMS delivery or sensor output. Horn
coverage checks GPIO routing, exact CAN ID/DLC/payload/timing, no startup frames,
BUSY in both directions, initialization/send failures, mandatory OFF attempt,
typed UART responses and timeout/no-retry behavior. Both PlatformIO environments
and the primary ESP-IDF firmware build successfully. The integrated horn has not
been flashed or tested on a vehicle by this implementation session.

Build verification passed for ESP-IDF 5.5.4 and both PlatformIO environments. The
host suite passes for both secondary variants and the primary transport, including
bounded diagnostic history and JSON validation. Viewer tests cover status summaries,
acknowledgement/result history, dry-run wording and malformed telemetry.
Bench acceptance remains pending; no boards were flashed during implementation.
