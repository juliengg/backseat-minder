# Telemetry viewer

Install dependencies in the Python environment used to run the viewer:

```powershell
python -m pip install -r tools/requirements-telemetry.txt
```

For Bluetooth, enable the computer's Bluetooth adapter and power the primary
board with firmware containing the BLE telemetry service. Run:

```powershell
python tools/usb_telemetry_viewer.py --bluetooth
```

The board advertises as **Backseat Minder**. The app pairs using an encrypted,
bonded BLE connection; approve an operating system pairing prompt if shown.
To select one of several boards, supply its Bluetooth address (or advertised
name): `--bluetooth AA:BB:CC:DD:EE:FF` (replace with your board's address).
Use the address discovered for your board;
its BLE address can differ from the Wi-Fi address printed by the flasher.

The existing USB command still works:

```powershell
python tools/usb_telemetry_viewer.py --port COM4
```

Both transports feed the same dashboard: sensor charts, detections, driver RSSI,
visible and thermal images, cellular event history, and the Send SMS button.
Commands are submitted once and never replayed after a disconnect. Bluetooth
images refresh more slowly than USB. Old image frames are replaced rather than
building an unbounded backlog; sensor and driver packets take priority.

Driver detection continues to scan for the fob while telemetry is connected.
The viewer does not keep the board awake. During driver-triggered deep sleep,
Bluetooth disconnects and the dashboard retains the last driver report; the app
automatically scans and reconnects when the board wakes. A short beacon-only
wake can finish before the computer reconnects. Sensors and cameras are only
available while the board is in active monitoring mode.

If Windows has a stale bond after erasing the board's NVS, remove **Backseat
Minder** from Windows Bluetooth settings and reconnect to pair again.

## Protocol

Service: `8d7c0001-3e91-4d8b-9f2a-5b6c7d8e9010`.
Notification TX characteristic: `8d7c0002-3e91-4d8b-9f2a-5b6c7d8e9010`.
Command RX characteristic: `8d7c0003-3e91-4d8b-9f2a-5b6c7d8e9010`.
Subscription and commands require link encryption. RX accepts exactly
`SEND_SMS\n` as a single write with response, using the saved recipient/body.

Each notification is a 16-byte big-endian header (`4sIII`: packet magic,
sequence, payload offset, total payload length), followed by payload bytes.
Packets of different types may interleave. The receiver checks sequence, length,
and contiguous offsets before delivering a complete packet to the same parser
used by USB. A disconnect discards incomplete packets and queued commands.
