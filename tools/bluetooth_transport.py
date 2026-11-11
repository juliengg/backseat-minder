"""BLE transport for the existing BSM telemetry packet stream.

Bleak is imported only when BluetoothSerial is constructed. No BLE dependency is
needed for USB mode. Notifications contain bounded, identified fragments so a
congested camera frame cannot corrupt the following sensor packet.
"""
import asyncio
import concurrent.futures
import struct
import threading
import time

SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e'
TX_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'
RX_UUID = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'
FRAGMENT = struct.Struct('<2sHII')
LIMITS = {b'BSMT':1024,b'BSMC':10000,b'BSMP':512,b'BSMF':200000,b'BSMH':8000,b'BSMD':5}


class FragmentAssembler:
    def __init__(self):
        self.reset()

    def reset(self):
        self.frame_id = None
        self.total = 0
        self.buffer = bytearray()

    def feed(self, notification):
        if len(notification) <= FRAGMENT.size:
            self.reset()
            return None
        magic, frame_id, offset, total = FRAGMENT.unpack_from(notification)
        body = notification[FRAGMENT.size:]
        if magic != b'\xb7M' or not 8 < total <= 200008 or offset + len(body) > total:
            self.reset()
            return None
        if offset == 0:
            self.frame_id, self.total, self.buffer = frame_id, total, bytearray()
        if frame_id != self.frame_id or total != self.total or offset != len(self.buffer):
            self.reset()
            return None
        self.buffer.extend(body)
        if len(self.buffer) != total:
            return None
        packet = bytes(self.buffer)
        self.reset()
        maximum = LIMITS.get(packet[:4])
        size = struct.unpack_from('>I',packet,4)[0]
        if maximum is None or not 0 < size <= maximum or size + 8 != len(packet):
            return None
        return packet


class BluetoothSerial:
    """Serial-like adapter backed by a reconnecting, paired Bleak GATT client."""
    is_bluetooth = True

    def __init__(self, address=None, name='Backseat Minder', *, scanner=None, client_factory=None):
        if scanner is None or client_factory is None:
            from bleak import BleakClient, BleakScanner
            scanner = scanner or BleakScanner
            client_factory = client_factory or BleakClient
        self.address, self.name = address, name
        self.scanner, self.client_factory = scanner, client_factory
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = self._loop = self._task = self._client = None
        self._open = False
        self._session = 0
        self._rx = bytearray()
        self._assembler = FragmentAssembler()
        self.status = 'Searching for Bluetooth telemetry device...'

    @property
    def is_open(self):
        with self._lock:
            return self._open

    @property
    def session_id(self):
        with self._lock:
            return self._session

    @property
    def in_waiting(self):
        with self._lock:
            return len(self._rx)

    def open(self):
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._worker, name='bsm-bluetooth', daemon=True)
        self._thread.start()

    def read(self, size):
        with self._lock:
            result = bytes(self._rx[:size])
            del self._rx[:size]
            return result

    def drain(self):
        # Keep stream generation and bytes coherent across disconnect/overflow.
        with self._lock:
            if not self._open:
                return None
            result=(self._session,bytes(self._rx))
            self._rx.clear()
            return result

    def _notify(self, _characteristic, data):
        with self._lock:
            if not self._open:
                return
            packet = self._assembler.feed(data)
            if packet is not None:
                if len(self._rx)+len(packet)>512000:
                    self._rx.clear()
                    self._session += 1  # Tell the viewer to discard its partial packet.
                self._rx.extend(packet)

    def _disconnected(self, _client=None):
        with self._lock:
            self._open = False
            self._rx.clear()
            self._assembler.reset()
        self.status = 'Bluetooth disconnected (sleep, off, or out of range); reconnecting...'

    async def _run(self):
        self._loop = asyncio.get_running_loop()
        self._task = asyncio.current_task()
        try:
            while not self._stop.is_set():
                try:
                    self.status = 'Searching for Backseat Minder Bluetooth telemetry...'
                    if self.address:
                        device = await self.scanner.find_device_by_address(self.address,timeout=8)
                    else:
                        device = await self.scanner.find_device_by_filter(
                            lambda d,a: SERVICE_UUID in [u.lower() for u in a.service_uuids]
                            and (a.local_name==self.name or d.name==self.name),
                            timeout=8,service_uuids=[SERVICE_UUID])
                    if device is None:
                        self.status = 'Bluetooth device not found; waiting for its next wake cycle...'
                        await asyncio.sleep(.5)
                        continue
                    self.status = 'Connecting/pairing Bluetooth; approve the OS prompt if shown...'
                    async with self.client_factory(device, disconnected_callback=self._disconnected,
                                                   pair=True,timeout=30,services=[SERVICE_UUID],
                                                   winrt={"use_cached_services": False}) as client:
                        self._client = client
                        # Open before notify registration: an immediate first packet is valid.
                        with self._lock:
                            self._rx.clear(); self._assembler.reset(); self._session+=1; self._open=True
                        await client.start_notify(TX_UUID,self._notify)
                        self.status = 'Bluetooth connected (encrypted telemetry)'
                        while client.is_connected and not self._stop.is_set():
                            await asyncio.sleep(.05)
                except asyncio.CancelledError:
                    raise
                except Exception as error:
                    self.status = f'Bluetooth: {error}; reconnecting...'
                finally:
                    self._client = None
                    # Retain the concrete connection error for the viewer.
                    status = self.status
                    self._disconnected()
                    if 'Bluetooth:' in status:
                        self.status = status
                if not self._stop.is_set():
                    await asyncio.sleep(.5)
        finally:
            self._disconnected()
            self._loop=self._task=None

    def _worker(self):
        try:
            asyncio.run(self._run())
        except asyncio.CancelledError:
            pass
        except Exception as error:
            self.status=f'Bluetooth worker stopped: {error}'

    def write(self, data):
        # Match USB's deliberately narrow control surface; no arbitrary recipient/AT.
        if data != b'SEND_SMS\n':
            raise OSError('Unsupported Bluetooth command')
        loop, client = self._loop, self._client
        if not self.is_open or loop is None or client is None:
            raise OSError('Bluetooth disconnected; command not submitted')
        async def submit():
            await client.write_gatt_char(RX_UUID,data,response=True)
        future=asyncio.run_coroutine_threadsafe(submit(),loop)
        try:
            future.result(timeout=1)
        except (Exception,concurrent.futures.TimeoutError) as error:
            future.cancel()
            raise OSError('Bluetooth write failed; command outcome unknown; no retry') from error
        return len(data)

    def close(self):
        self._stop.set()
        loop,task=self._loop,self._task
        if loop is not None and task is not None:
            try: loop.call_soon_threadsafe(task.cancel)
            except RuntimeError: pass
        if self._thread is not None:
            self._thread.join(timeout=1)
        self._disconnected()


class BluetoothConnection:
    """Viewer adapter: one generation per subscription, not per scan attempt."""
    def __init__(self, device):
        self.device=device
        self.generation=0
        self._session=None
        device.open()

    def read_available(self, now):
        received=self.device.drain()
        if received is None:
            return None
        session,data=received
        if session!=self._session:
            self._session=session;self.generation+=1
        return data
