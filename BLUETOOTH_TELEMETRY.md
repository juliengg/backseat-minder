# Bluetooth telemetry viewer

The primary ESP32-S3 advertises a BLE service named **Backseat Minder** while its
existing Bluetooth host is awake. The viewer supports either native USB or BLE:

```powershell
python -m pip install -r tools/requirements-bluetooth.txt
python tools/usb_telemetry_viewer.py --bluetooth
```

Turn on the computer's Bluetooth radio. The first connection requests encrypted
pairing; approve an OS pairing prompt if one appears. The firmware uses BLE Secure
Connections bonding with no display/input (Just Works). This encrypts the link;
it does not provide a physical PIN or protection against an active MITM during
initial pairing. Use trusted nearby devices for pairing. Do not remove bonds from
one side only; if pairing fails after changing firmware/PCs, remove the obsolete
OS pairing and reconnect. Bond keys are saved in ESP32 NVS.

For a specific unit, provide its BLE address (or CoreBluetooth UUID on macOS):

```powershell
python tools/usb_telemetry_viewer.py --bluetooth --bluetooth-address 3C:DC:75:69:25:16
```

The optional `--bluetooth-name` selects another advertised name. `--port` and
`--bluetooth` are mutually exclusive. USB mode is retained and does not import
Bleak; its dependencies are in `tools/requirements-viewer.txt`:

```powershell
python tools/usb_telemetry_viewer.py --port COM15
```

Use the primary's **native USB** connector for USB telemetry; its CH343 UART
connector supplies console logs, not the binary viewer stream. BLE does not need
a USB data connection; the device still needs power. Only the primary needs the
BLE firmware update. The secondary modem/CAN firmware is unchanged.

## Data and rates

BLE carries the same BSM packet types as USB: sensor readings/validity, occupancy
flags, beacon state/RSSI, alert elapsed time, device-computed slope and seven-minute
forecast, cellular summary/history and received SMS, JPEG camera frames, thermal
CSV frames, and heat-trace bounds. The existing dashboard Send SMS button sends
the same narrow `SEND_SMS` request over the encrypted BLE RX characteristic; no
phone number, message override, AT command, call, or horn command is accepted.
No SMS/call/horn action is triggered by merely connecting the viewer.

Camera previews are best effort, capped at one attempt per two seconds; thermal
frames and heat bounds are capped at one per second. Camera frames over 40 KB or
connections with negotiated MTU below 100 skip camera transmission. Normal Windows
connections negotiate the requested 247-byte MTU. Link congestion can lower rates
further; USB rates remain unchanged. The dashboard displays the ESP32 forecast,
not a new viewer-side temperature calculation.

Firmware keeps one latest pending packet per type and sends state/status ahead
of previews. Optional packet copies prefer PSRAM and are bounded. A separate low
priority sender task fragments packets and handles congestion with deadlines;
alert/sensor code never waits for a Bluetooth viewer. Notifications are best effort,
not persistent logging: disconnected/overloaded viewers do not receive a backlog.

## Beacon scanning, setup and sleep

The service shares the existing Bluedroid host and single GAP callback with passive
beacon scanning. It does not create a second controller or replace the beacon's
callback. Before Wi-Fi setup or deep sleep, queued telemetry is discarded and
sending is quiesced before the shared host is deinitialized.

**A connected viewer does not keep the primary awake.** Existing driver-present
and cancelled-incident sleep policies remain active. BLE disconnects during deep
sleep and Wi-Fi setup; the viewer scans/reconnects automatically. Beacon-only wake
probes produce beacon/power packets, not new temperature or camera samples. Full
streams are produced only while the primary's monitoring/sensor tasks are active.
Pairing/discovery may take multiple wake cycles. Keep the beacon/alert workflow
in its intended state; do not create a real alert solely to test previews.

Only one BLE viewer can connect at a time. USB and one BLE viewer can receive
simultaneously. The cellular log contains sender/message data, so pair only the
computer intended to view those logs.

## Protocol and tests

The BLE service uses Nordic UART UUIDs: service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`,
TX notify `...0003...`, RX write `...0002...`. Subscribing to TX and writing RX
require encryption. Each notification adds a 12-byte little-endian fragment header:
`<2sHII` = magic `B7 4D`, frame ID, byte offset, complete BSM packet length.
Payload is the corresponding fragment of the existing 8-byte BSM header and body.
The client validates lengths/types and exposes only fully reassembled packets.
Aborted frames, dropped fragments, reconnects, and receiver overflow cannot join
old fragments to a new stream; the dashboard's parser is reset by stream generation.

Python tests use fake BLE clients/scanners and cover all packet types, small/large
MTUs, lost/reordered fragments, aborted image recovery, reconnection, buffer limits,
CLI mode selection and no retry on unknown command outcomes. Existing USB viewer,
beacon and sleep-policy tests remain in use. Build with `tools/Build-Primary.ps1`.
The BLE firmware also needs `CONFIG_BT_GATTS_ENABLE=y`, set in this checkout.

Official API references: [Espressif GATT example](https://github.com/espressif/esp-idf/tree/v5.5.4/examples/bluetooth/bluedroid/ble/gatt_server_service_table),
[Bleak client and pairing](https://bleak.readthedocs.io/en/stable/api/client.html).

## Hardware verification

On November 11, 2026, the primary ESP32-S3 was built with ESP-IDF 5.5.4 and
flashed without erasing saved settings. A paired Windows client received all six
packet types over BLE: 26 beacon/status, six sensor, six cellular summary,
15 thermal, 15 heat-trace, and six JPEG packets in a 58-second test, across two
subscriptions that delivered data. The nearby beacon was recognized at -46 dBm.
Console logs confirmed normal sensor startup, intentional BLE disconnection and
deep sleep; the client automatically reconnected. No SMS, call or horn test
command was sent. The 42 Python viewer/transport tests passed.

The current image fits the 2 MB application partition with about 48 KB free;
further firmware growth may require revisiting the partition layout.
