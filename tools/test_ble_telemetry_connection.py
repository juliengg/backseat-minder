"""BLE fragmentation, reconnect boundaries, and viewer transport selection."""

import asyncio
import struct
import threading
import unittest
from unittest.mock import AsyncMock, Mock, patch

from ble_telemetry_connection import (
    BlePacketAssembler, BluetoothConnection, SERVICE_UUID, RX_UUID,
)
from usb_telemetry_viewer import PacketReader, main


def fragment(marker, sequence, offset, total, payload):
    return struct.pack(">4sIII", marker, sequence, offset, total) + payload


class BluetoothPacketTests(unittest.TestCase):
    def test_interleaved_camera_and_sensor_use_existing_parser(self):
        assembler = BlePacketAssembler()
        jpeg = b"jpeg" * 5000
        sensor = b'{"temperature_f":72.5}'
        self.assertIsNone(assembler.feed(fragment(b"BSMF", 1, 0, len(jpeg), jpeg[:228])))
        sample = assembler.feed(fragment(b"BSMT", 2, 0, len(sensor), sensor))
        self.assertEqual(PacketReader().feed(sample, 0), [(b"BSMT", sensor)])
        packet = None
        for offset in range(228, len(jpeg), 228):
            packet = assembler.feed(fragment(b"BSMF", 1, offset, len(jpeg), jpeg[offset:offset+228]))
        self.assertEqual(PacketReader().feed(packet, 5), [(b"BSMF", jpeg)])

    def test_loss_duplicate_or_out_of_order_discards_packet(self):
        for seq, offset in ((2, 3), (1, 4), (1, 0)):
            assembler = BlePacketAssembler()
            assembler.feed(fragment(b"BSMH", 1, 0, 10, b"abc"))
            result = assembler.feed(fragment(b"BSMH", seq, offset, 10, b"def"))
            self.assertIsNone(result)
            self.assertIsNone(assembler.feed(fragment(b"BSMH", 1, 6, 10, b"ghij")))
            result = assembler.feed(fragment(b"BSMH", 3, 0, 3, b"new"))
            self.assertEqual(PacketReader().feed(result, 0), [(b"BSMH", b"new")])

    def test_malformed_notifications_are_bounded(self):
        assembler = BlePacketAssembler()
        for bad in (b"short", fragment(b"XXXX", 1, 0, 4, b"test"),
                    fragment(b"BSMF", 1, 0, 200001, b"x"),
                    fragment(b"BSMT", 1, 0, 0, b"x"),
                    fragment(b"BSMD", 1, 4, 5, b"xx")):
            self.assertIsNone(assembler.feed(bad))
        self.assertEqual(assembler.partial, {})

    def test_new_transfer_replaces_interrupted_image(self):
        assembler = BlePacketAssembler()
        assembler.feed(fragment(b"BSMF", 1, 0, 20, b"old"))
        packet = assembler.feed(fragment(b"BSMF", 2, 0, 3, b"new"))
        self.assertEqual(PacketReader().feed(packet, 0), [(b"BSMF", b"new")])

    def test_new_connection_cannot_finish_old_transfer(self):
        old = BlePacketAssembler()
        old.feed(fragment(b"BSMH", 1, 0, 6, b"abc"))
        new = BlePacketAssembler()
        self.assertIsNone(new.feed(fragment(b"BSMH", 1, 3, 6, b"def")))


class BluetoothTransportTests(unittest.TestCase):
    def connection(self):
        # Exercise real buffers without starting a radio or an OS pairing prompt.
        with patch("ble_telemetry_connection.importlib.util.find_spec", return_value=True), \
                patch("ble_telemetry_connection.threading.Thread"):
            return BluetoothConnection()

    def test_discovery_requires_service_and_optional_address(self):
        connection = self.connection()
        device = Mock(address="AA:BB", name="Backseat Minder")
        adv = Mock(service_uuids=[SERVICE_UUID], local_name="Backseat Minder")
        self.assertTrue(connection._matches(device, adv))
        connection.identifier = "aa:bb"
        self.assertTrue(connection._matches(device, adv))
        connection.identifier = "CC:DD"
        self.assertFalse(connection._matches(device, adv))
        adv.service_uuids = []
        connection.identifier = ""
        self.assertFalse(connection._matches(device, adv))

    def test_complete_packets_and_bounded_backlog(self):
        connection = self.connection()
        connection._connected = True
        payload = b"x" * 180000
        for seq in range(4):
            connection._notification(BlePacketAssembler(), fragment(b"BSMF", seq, 0, len(payload), payload))
        self.assertEqual(connection.generation, 1)
        packets = PacketReader().feed(connection.read_available(0), 0)
        self.assertEqual(packets, [(b"BSMF", payload), (b"BSMF", payload)])
        self.assertEqual(connection.read_available(0), b"")
        connection._connected = False
        self.assertIsNone(connection.read_available(0))

    def test_disconnected_command_fails_without_queueing(self):
        connection = self.connection()
        with self.assertRaises(OSError):
            connection.write(b"SEND_SMS\n")

    def test_completed_sleep_report_survives_disconnect_until_gui_poll(self):
        connection = self.connection()
        report = b'{"deep_sleep_pending":true}'
        connection._notification(BlePacketAssembler(), fragment(b"BSMP", 1, 0, len(report), report))
        connection._connected = False
        self.assertEqual(PacketReader().feed(connection.read_available(0), 0), [(b"BSMP", report)])
        self.assertIsNone(connection.read_available(0))

    def test_command_uses_response_once_and_does_not_retry_failure(self):
        connection = self.connection()
        loop = asyncio.new_event_loop()
        thread = threading.Thread(target=loop.run_forever)
        thread.start()
        try:
            connection._loop = loop
            connection._connected = True
            connection._client = Mock(is_connected=True)
            connection._client.write_gatt_char = AsyncMock()
            self.assertEqual(connection.write(b"SEND_SMS\n"), 9)
            connection._client.write_gatt_char.assert_awaited_once_with(RX_UUID, b"SEND_SMS\n", response=True)
            connection._client.write_gatt_char = AsyncMock(side_effect=OSError("link lost"))
            with self.assertRaises(OSError):
                connection.write(b"SEND_SMS\n")
            connection._client.write_gatt_char.assert_awaited_once()
        finally:
            loop.call_soon_threadsafe(loop.stop)
            thread.join()
            loop.close()

    def test_cli_bluetooth_default_and_address(self):
        for args, identifier in ((["--bluetooth"], ""), (["--bluetooth", "AA:BB"], "AA:BB")):
            device = Mock()
            with patch("sys.argv", ["viewer"] + args), patch("builtins.print"), \
                    patch("usb_telemetry_viewer.BluetoothConnection", return_value=device) as ctor, \
                    patch("usb_telemetry_viewer.serial.Serial") as serial, \
                    patch("usb_telemetry_viewer.show_viewer", return_value=0) as show:
                self.assertEqual(main(), 0)
            ctor.assert_called_once_with(identifier)
            serial.assert_not_called()
            show.assert_called_once_with(device)
            device.close.assert_called_once()

    def test_missing_bleak_reports_install_command(self):
        with patch("ble_telemetry_connection.importlib.util.find_spec", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "pip install bleak"):
                BluetoothConnection()


class BluetoothReconnectTests(unittest.IsolatedAsyncioTestCase):
    async def test_reconnect_starts_new_assembler_and_clears_old_completed_backlog(self):
        with patch("ble_telemetry_connection.importlib.util.find_spec", return_value=True), \
                patch("ble_telemetry_connection.threading.Thread"):
            connection = BluetoothConnection()
        attempts = []

        class Client:
            is_connected = True

            def __init__(self, device, **kwargs):
                self.disconnected = kwargs["disconnected_callback"]
                self.attempt = len(attempts)
                attempts.append(self)

            async def __aenter__(self):
                return self

            async def __aexit__(self, *args):
                return False

            async def start_notify(self, uuid, notify):
                # Deliver after subscription, matching real OS callbacks.
                async def receive():
                    await asyncio.sleep(0)
                    if self.attempt == 0:
                        notify(None, fragment(b"BSMF", 1, 0, 6, b"old"))
                        notify(None, fragment(b"BSMT", 2, 0, 3, b"old"))
                    else:
                        notify(None, fragment(b"BSMF", 1, 3, 6, b"end"))
                        notify(None, fragment(b"BSMT", 3, 0, 3, b"new"))
                        connection._stop.set()
                    self.disconnected(self)
                asyncio.create_task(receive())

        with patch("bleak.BleakScanner.find_device_by_filter", new=AsyncMock(return_value=Mock(address="AA:BB"))), \
                patch("bleak.BleakClient", new=Client):
            await asyncio.wait_for(connection._run(), 4)
        self.assertEqual(connection.generation, 2)
        self.assertFalse(connection._connected)
        self.assertEqual(PacketReader().feed(connection.read_available(0), 0), [(b"BSMT", b"new")])
        self.assertIsNone(connection.read_available(0))


if __name__ == "__main__":
    unittest.main()
