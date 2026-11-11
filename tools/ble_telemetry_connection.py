"""BLE transport for the existing Backseat Minder viewer (optional bleak dependency)."""

import asyncio
from collections import deque
import importlib.util
import struct
import threading

SERVICE_UUID = "8d7c0001-3e91-4d8b-9f2a-5b6c7d8e9010"
TX_UUID = "8d7c0002-3e91-4d8b-9f2a-5b6c7d8e9010"
RX_UUID = "8d7c0003-3e91-4d8b-9f2a-5b6c7d8e9010"
PACKET_LIMITS = {b"BSMP": 512, b"BSMT": 512, b"BSMC": 10000,
                 b"BSMD": 5, b"BSMH": 8000, b"BSMF": 200000}


class BlePacketAssembler:
    """Reassemble interleaved notifications; never emit an interrupted packet."""

    def __init__(self):
        self.partial = {}

    def feed(self, notification):
        if len(notification) <= 16:
            return None
        marker, sequence, offset, total = struct.unpack(">4sIII", notification[:16])
        chunk = notification[16:]
        if marker not in PACKET_LIMITS:
            return None
        if not 0 < total <= PACKET_LIMITS[marker] or offset + len(chunk) > total:
            self.partial.pop(marker, None)
            return None
        if offset == 0:
            self.partial[marker] = (sequence, total, bytearray())
        state = self.partial.get(marker)
        if state is None:
            return None
        old_sequence, old_total, data = state
        if old_sequence != sequence or old_total != total or offset != len(data):
            self.partial.pop(marker, None)
            return None
        data.extend(chunk)
        if len(data) == total:
            del self.partial[marker]
            # Return exactly the packet format consumed by the USB viewer.
            return marker + struct.pack(">I", total) + data
        return None


class BluetoothConnection:
    """Keep asyncio/Bleak off Tk's thread; reconnect without replaying commands."""

    label = "Bluetooth"

    def __init__(self, identifier=""):
        if importlib.util.find_spec("bleak") is None:
            raise RuntimeError("Install Bluetooth support: python -m pip install bleak")
        self.identifier = identifier
        self.generation = 0
        self._lock = threading.Lock()
        self._received = deque()
        self._buffered = 0
        self._connected = False
        self._status = "Scanning for Backseat Minder..."
        self._stop = threading.Event()
        self._loop = self._task = self._client = None
        self._thread = threading.Thread(target=self._thread_main, name="BLE telemetry", daemon=True)
        self._thread.start()

    @property
    def status(self):
        with self._lock:
            return self._status

    def _set_status(self, text):
        with self._lock:
            self._status = text

    def _notification(self, assembler, data):
        packet = assembler.feed(data)
        if packet is None:
            return
        with self._lock:
            if self._buffered + len(packet) > 512000:
                self._received.clear()
                self._buffered = 0
                self.generation += 1
            self._received.append(packet)
            self._buffered += len(packet)

    def _matches(self, device, advertisement):
        if SERVICE_UUID not in [uuid.lower() for uuid in advertisement.service_uuids]:
            return False
        return (not self.identifier or
                self.identifier.casefold() in (device.address.casefold(),
                    (advertisement.local_name or device.name or "").casefold()))

    async def _run(self):
        from bleak import BleakClient, BleakScanner

        while not self._stop.is_set():
            try:
                self._set_status("Bluetooth scanning; waiting for device (it may be asleep)...")
                device = await BleakScanner.find_device_by_filter(self._matches, timeout=8)
                if device is None:
                    continue
                self._set_status(f"Bluetooth connecting to {device.address}; approve pairing if prompted...")
                disconnected = asyncio.Event()
                async with BleakClient(device, pair=True, timeout=20,
                                       disconnected_callback=lambda _: disconnected.set()) as client:
                    assembler = BlePacketAssembler()
                    await client.start_notify(TX_UUID,
                        lambda _, data: self._notification(assembler, data))
                    with self._lock:
                        self._received.clear()
                        self._buffered = 0
                        self.generation += 1
                        self._client = client
                        self._connected = True
                        self._status = f"Bluetooth connected: {device.address}"
                    await disconnected.wait()
            except asyncio.CancelledError:
                raise
            except Exception as error:
                self._set_status(f"Bluetooth reconnecting: {error}")
            finally:
                with self._lock:
                    self._connected = False
                    self._client = None
                    # Keep complete packets for the next GUI poll, especially
                    # the final sleep report delivered just before disconnect.
            if not self._stop.is_set():
                await asyncio.sleep(1)

    def _thread_main(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._task = self._loop.create_task(self._run())
        try:
            if self._stop.is_set():
                self._task.cancel()
            self._loop.run_until_complete(self._task)
        except asyncio.CancelledError:
            pass
        except Exception as error:
            self._set_status(f"Bluetooth unavailable: {error}")
        finally:
            self._loop.run_until_complete(self._loop.shutdown_asyncgens())
            self._loop.close()

    def read_available(self, now):
        with self._lock:
            if not self._connected and not self._received:
                return None
            data = b"".join(self._received)
            self._received.clear()
            self._buffered = 0
            return data

    def write(self, command):
        with self._lock:
            client = self._client
            ready = self._connected
        if not ready or self._stop.is_set() or self._loop is None or self._loop.is_closed():
            raise OSError("Bluetooth disconnected")

        async def send_once():
            if self._client is not client or not client.is_connected:
                raise OSError("Bluetooth connection changed")
            await client.write_gatt_char(RX_UUID, command, response=True)

        future = asyncio.run_coroutine_threadsafe(send_once(), self._loop)
        try:
            future.result(timeout=3)
        except Exception as error:
            future.cancel()
            raise OSError(f"Bluetooth write failed: {error}") from error
        return len(command)

    def close(self):
        self._stop.set()
        if self._loop is not None and self._task is not None and not self._loop.is_closed():
            self._loop.call_soon_threadsafe(self._task.cancel)
        self._thread.join(timeout=3)
