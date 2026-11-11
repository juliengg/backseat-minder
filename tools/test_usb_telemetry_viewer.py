"""Run with: python -m unittest discover -s tools -p 'test_*.py'."""

import struct
import itertools
import unittest
from unittest.mock import Mock, patch
import serial

from usb_telemetry_viewer import detection_statuses, update_detection_labels, regression_line
from usb_telemetry_viewer import cellular_view, update_cellular_panel
from usb_telemetry_viewer import SmsControl
from usb_telemetry_viewer import UsbConnection, driver_view, DriverDisplay
from usb_telemetry_viewer import main, PacketReader
from usb_telemetry_viewer import main, device_forecast_line


class DriverTelemetryTests(unittest.TestCase):
    def test_viewer_opens_while_device_is_already_asleep(self):
        device = Mock(is_open=False)
        with patch("usb_telemetry_viewer.serial.Serial", return_value=device), \
                patch("usb_telemetry_viewer.show_viewer", return_value=0) as show, \
                patch("sys.argv", ["viewer", "--port", "COM4"]), patch("builtins.print"):
            self.assertEqual(main(), 0)
        self.assertEqual(device.port, "COM4")
        self.assertFalse(device.dtr)
        self.assertFalse(device.rts)
        show.assert_called_once_with(device)
        device.open.assert_not_called()
        device.close.assert_called_once()

    def test_driver_flag_rssi_and_power(self):
        sample = {"driver_present": True, "beacon_available": True,
                  "beacon_rssi_valid": True, "beacon_rssi_dbm": -65,
                  "deep_sleep_pending": True}
        text, color = driver_view(sample)
        self.assertIn("YES", text)
        self.assertIn("-65 dBm", text)
        self.assertIn("Deep sleep", text)
        self.assertEqual(color, "green")
        sample["driver_present"] = False
        sample["beacon_rssi_valid"] = False
        sample["deep_sleep_pending"] = False
        text, color = driver_view(sample)
        self.assertIn("NO", text)
        self.assertIn("no fresh", text)
        self.assertNotIn("-65", text)
        self.assertIn("Active", text)

    def test_legacy_and_failed_ble_are_unknown(self):
        for sample in ({}, {"driver_present": False, "beacon_available": False},
                       {"driver_present": "false"}):
            self.assertIn("unknown", driver_view(sample)[0])

    def test_initial_and_wake_probes_never_report_an_unconfirmed_absence(self):
        for previous in (False, True):
            text, color = driver_view({"driver_present": previous,
                "driver_checking": True, "beacon_available": True})
            self.assertIn("checking", text)
            self.assertNotIn("NO", text)
            self.assertEqual(color, "gray")
            if previous:
                self.assertIn("YES (last confirmed)", text)

    def test_legacy_keepawake_field_is_ignored(self):
        text, _ = driver_view({"driver_present": True, "beacon_available": True,
                               "telemetry_active": True})
        self.assertIn("Active", text)
        self.assertNotIn("telemetry", text)

    def test_sleep_display_survives_usb_gap_then_wake_probe_then_away(self):
        display = DriverDisplay()
        display.observe({"driver_present": True, "beacon_available": True,
            "driver_checking": False, "deep_sleep_pending": True,
            "power_state": "sleep_pending"}, 0)
        # No USB packets during real deep sleep: keep the last confirmed state.
        for now in (1, 5, 20):
            text, _ = display.view(now)
            self.assertIn("YES", text)
            self.assertIn("Deep sleep", text)
            self.assertNotIn("unknown", text)
        self.assertIn("Last report", display.view(20)[0])
        display.observe({"driver_present": True, "driver_checking": True,
            "beacon_available": True, "power_state": "beacon_probe"}, 21)
        self.assertIn("YES", display.view(21)[0])
        self.assertIn("Deep sleep", display.view(21)[0])
        display.observe({"driver_present": False, "driver_checking": False,
            "beacon_available": True, "power_state": "active"}, 26)
        self.assertIn("NO", display.view(26)[0])
        self.assertIn("Active", display.view(26)[0])
        self.assertNotIn("Deep sleep", display.view(26)[0])

    def test_viewer_started_during_sleep_probe_uses_retained_presence(self):
        display = DriverDisplay()
        display.observe({"driver_present": True, "driver_checking": True,
            "beacon_available": True, "power_state": "beacon_probe"}, 0)
        text, _ = display.view(0)
        self.assertIn("YES", text)
        self.assertIn("Deep sleep", text)

    def test_actual_active_override_is_not_mislabeled_as_sleep(self):
        text, _ = driver_view({"driver_present": True, "beacon_available": True,
                               "power_state": "active"})
        self.assertIn("YES", text)
        self.assertIn("Active", text)
        self.assertNotIn("Deep sleep", text)

    def test_reconnect_backoff_and_data_without_resending_commands(self):
        device = Mock(is_open=True, in_waiting=3)
        device.read.side_effect = serial.SerialException("asleep")
        device.close.side_effect = lambda: setattr(device, "is_open", False)
        connection = UsbConnection(device)
        self.assertIsNone(connection.read_available(0))
        device.open.assert_not_called()
        self.assertIsNone(connection.read_available(0.4))
        device.open.assert_not_called()
        device.open.side_effect = serial.SerialException("not enumerated")
        self.assertIsNone(connection.read_available(0.5))
        self.assertEqual(device.open.call_count, 1)
        device.open.side_effect = lambda: setattr(device, "is_open", True)
        device.read.side_effect = None
        device.read.return_value = b"BSM"
        self.assertEqual(connection.read_available(1), b"BSM")
        self.assertEqual(connection.generation, 1)
        device.write.assert_not_called()

    def test_viewing_is_passive_and_never_writes_keepawake_commands(self):
        device = Mock(is_open=True, in_waiting=0)
        connection = UsbConnection(device)
        self.assertEqual(connection.read_available(0), b"")
        self.assertEqual(connection.read_available(0.9), b"")
        for now in (1, 2, 4):
            self.assertEqual(connection.read_available(now), b"")
        device.write.assert_not_called()


class SmsControlTests(unittest.TestCase):
    def snapshot(self, count=0, busy=False, uptime=9000):
        return {"uptime_ms": uptime, "sms_control": True,
                "sms_busy": busy, "sms_requests": count, "events": []}

    def control(self):
        device = Mock()
        device.write.return_value = len(b"SEND_SMS\n")
        control = SmsControl(device)
        control.observe(self.snapshot(), 0)
        return control, device

    def test_no_send_before_supported_fresh_telemetry(self):
        control, device = SmsControl(Mock()), Mock()
        control.device = device
        control.send(0)
        control.observe({"uptime_ms": 0, "events": []}, 0)
        control.send(1)
        device.write.assert_not_called()
        self.assertIn("Update primary", control.refresh(1)[1])
        control.observe(self.snapshot(), 2)
        control.send(13)
        device.write.assert_not_called()

    def test_single_click_and_duplicate_clicks_until_ack(self):
        control, device = self.control()
        control.send(1)
        control.send(2)
        device.write.assert_called_once_with(b"SEND_SMS\n")
        # Buffered telemetry from before the click must not re-enable the button.
        control.observe(self.snapshot(), 3)
        self.assertFalse(control.refresh(3)[0])
        control.observe(self.snapshot(count=1, busy=True), 4)
        self.assertFalse(control.refresh(4)[0])
        control.observe(self.snapshot(count=1, busy=False), 5)
        self.assertTrue(control.refresh(5)[0])
        control.send(6)
        self.assertEqual(device.write.call_count, 2)

    def test_busy_and_stale_disable_send(self):
        control, device = self.control()
        control.observe(self.snapshot(busy=True), 1)
        control.send(2)
        control.observe(self.snapshot(busy=False), 3)
        control.send(14)
        device.write.assert_not_called()

    def test_failed_or_partial_write_never_retries(self):
        for outcome in (3, serial.SerialTimeoutException("timeout"), OSError("disconnected")):
            control, device = self.control()
            if isinstance(outcome, Exception):
                device.write.side_effect = outcome
            else:
                device.write.return_value = outcome
            control.send(1)
            self.assertIn("write failed", control.refresh(2)[1])
            control.send(2)
            control.observe(self.snapshot(), 12)
            self.assertIn("outcome unknown", control.refresh(12)[1])
            device.write.assert_called_once()

    def test_restart_and_no_confirmation_are_not_success(self):
        control, device = self.control()
        control.send(1)
        control.observe(self.snapshot(uptime=0), 2)
        self.assertIn("restarted", control.refresh(2)[1])
        control.send(3)
        control.observe(self.snapshot(uptime=1000), 14)
        self.assertIn("No command confirmation", control.refresh(14)[1])
        self.assertEqual(device.write.call_count, 2)

    def test_malformed_capability_does_not_enable_send(self):
        for field, value in (("sms_control", 1), ("sms_busy", "false"),
                             ("sms_requests", -1), ("sms_requests", True)):
            control, device = self.control()
            sample = self.snapshot()
            sample[field] = value
            control.observe(sample, 1)
            control.send(2)
            device.write.assert_not_called()


class CellularStatusTests(unittest.TestCase):
    def test_incoming_sender_and_multiline_unicode_body(self):
        sample = self.snapshot("SMS_RECEIVED")
        sample["events"][0].update(sender="+15551234567", message='OK\nERROR|"😀')
        text, color, history = cellular_view(sample)
        self.assertEqual(text, "Received SMS from +15551234567")
        self.assertEqual(color, "green")
        self.assertIn('    OK\n    ERROR|"😀', history)
        label, widget = Mock(), Mock()
        update_cellular_panel(label, widget, sample)
        update_cellular_panel(label, widget, sample)
        self.assertEqual(widget.insert.call_args_list[0], widget.insert.call_args_list[1])

    def test_incoming_invalid_fields_rejected(self):
        for fields in ({}, {"sender": "+", "message": "x"},
                       {"sender": "123\nRESULT|OK", "message": "x"},
                       {"sender": "123", "message": "x" * 481},
                       {"sender": "123", "message": "bad\x00body"}):
            sample = self.snapshot("SMS_RECEIVED")
            sample["events"][0].update(fields)
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                cellular_view(sample)

    def snapshot(self, *statuses):
        return {"uptime_ms": 9000, "events": [
            {"uptime_ms": i * 1000, "status": status}
            for i, status in enumerate(statuses)]}

    def test_ack_and_final_result_both_visible(self):
        text, color, history = cellular_view(self.snapshot("WAITING_ACK", "ACCEPTED", "OK"))
        self.assertEqual(color, "green")
        self.assertEqual("SMS submitted successfully", text)
        self.assertIn("Acknowledged", history)
        self.assertIn("00:02", history)

    def test_dry_run_is_not_reported_as_sent(self):
        text, color, _ = cellular_view(self.snapshot("DRY_RUN"))
        self.assertIn("no SMS sent", text)
        self.assertNotEqual(color, "green")

    def test_sim_network_and_timeout_messages(self):
        for status in ("SIM_NOT_READY", "NOT_REGISTERED", "RESULT_TIMEOUT", "NO_PHONE", "NO_NAME", "INVALID_NAME"):
            self.assertEqual(cellular_view(self.snapshot(status))[1], "red")
        self.assertIn("missing, locked", cellular_view(self.snapshot("SIM_NOT_READY"))[0])

    def test_repeated_snapshot_replaces_history_and_reboot_clears_it(self):
        label, history = Mock(), Mock()
        sample = self.snapshot("ACCEPTED", "DRY_RUN")
        update_cellular_panel(label, history, sample)
        update_cellular_panel(label, history, sample)
        self.assertEqual(history.delete.call_count, 2)
        self.assertEqual(history.insert.call_args_list[0], history.insert.call_args_list[1])
        update_cellular_panel(label, history, self.snapshot("READY"))
        self.assertNotIn("Dry run", history.insert.call_args.args[1])

    def test_malformed_payloads_do_not_update_widgets(self):
        bad = [None, {}, {"events": "bad"}, self.snapshot("BAD\nTOKEN"),
               self.snapshot("X" * 48), self.snapshot(*(["OK"] * 9)),
               {"uptime_ms": 0, "events": [{"uptime_ms": 1, "status": "OK"}]}]
        for sample in bad:
            label, history = Mock(), Mock()
            with self.subTest(sample=sample), self.assertRaises(ValueError):
                update_cellular_panel(label, history, sample)
            label.configure.assert_not_called()

    def test_unknown_error_is_readable(self):
        text, color, _ = cellular_view(self.snapshot("FUTURE_ERROR"))
        self.assertIn("FUTURE_ERROR", text)
        self.assertEqual(color, "red")

    def test_horn_result_is_distinct_from_sms_success(self):
        text, color, history = cellular_view(self.snapshot("HORN_ACCEPTED", "HORN_OK"))
        self.assertEqual(text, "Horn: CAN sequence submitted")
        self.assertEqual(color, "green")
        self.assertNotIn("SMS submitted", history)

    def test_horn_failures_are_visible(self):
        for status in ("HORN_CAN_UNAVAILABLE", "HORN_CAN_SEND_FAILED", "HORN_CAN_OFF_FAILED",
                       "HORN_RESULT_TIMEOUT", "HORN_UART_WRITE_FAILED"):
            self.assertEqual(cellular_view(self.snapshot(status))[1], "red")


class RegressionTests(unittest.TestCase):
    def test_uses_last_ten_valid_samples_and_actual_times(self):
        times = [-100, 0, 1, 3, 6, 10, 15, 21, 28, 36, 45, 46]
        values = [9999] + [2 * x + 7 for x in times[1:-1]] + [float("nan")]
        xs, ys = regression_line(times, values)
        self.assertEqual(xs, [0, 45])
        self.assertAlmostEqual(ys[0], 7)
        self.assertAlmostEqual(ys[1], 97)

    def test_waits_for_ten_readings(self):
        self.assertEqual(regression_line(range(9), range(9)), ([], []))

    def test_identical_timestamps_do_not_divide_by_zero(self):
        self.assertEqual(regression_line([1] * 10, range(10)), ([], []))

    def test_flat_readings(self):
        self.assertEqual(regression_line(range(10), [50] * 10), ([0, 9], [50, 50]))


class DetectionStatusTests(unittest.TestCase):
    def test_all_sensor_combinations_and_colors(self):
        for face, radar, thermal in itertools.product((False, True), repeat=3):
            expected_presence = any((face, radar, thermal))
            for legacy in (False, True):
                with self.subTest(face=face, radar=radar, thermal=thermal,
                                  legacy=legacy):
                    sample = {
                        "face_detected": face,
                        "mmwave_person_detected" if legacy else
                        "mmwave_presence_detected": radar,
                        "heat_trace_detected": thermal,
                    }
                    if not legacy:
                        sample["human_presence_detected"] = expected_presence
                    expected = {
                        "Human presence": expected_presence,
                        "Face detected": face,
                        "mmWave radar presence": radar,
                        "Thermal heat detected": thermal,
                    }
                    self.assertEqual(detection_statuses(sample), expected)
                    labels = {name: Mock() for name in expected}
                    update_detection_labels(labels, sample)
                    for name, detected in expected.items():
                        labels[name].configure.assert_called_once_with(
                            text="✓" if detected else "✗",
                            fg="green" if detected else "red")

    def test_new_radar_field_takes_precedence_over_legacy_field(self):
        statuses = detection_statuses({
            "mmwave_presence_detected": False,
            "mmwave_person_detected": True,
        })
        self.assertFalse(statuses["mmWave radar presence"])
        self.assertFalse(statuses["Human presence"])

    def test_labels_clear_after_positive_sample(self):
        labels = {name: Mock() for name in detection_statuses({})}
        update_detection_labels(labels, {"face_detected": True})
        update_detection_labels(labels, {"face_detected": False})
        labels["Human presence"].configure.assert_called_with(
            text="✗", fg="red")


if __name__ == "__main__":
    unittest.main()


class StreamRecoveryTests(unittest.TestCase):
    def packet(self, marker, payload):
        return marker + struct.pack(">I", len(payload)) + payload

    def test_partial_transfer_is_discarded_after_gap(self):
        reader = PacketReader()
        self.assertEqual(reader.feed(b"BSMF" + struct.pack(">I", 90000) + b"partial", 0), [])
        thermal = b",".join([b"25.00"] * 768)
        self.assertEqual(reader.feed(self.packet(b"BSMH", thermal), 1.1),
                         [(b"BSMH", thermal)])

    def test_fragmented_and_multiplexed_packets_survive_normal_reads(self):
        reader = PacketReader()
        wire = self.packet(b"BSMF", b"jpeg") + self.packet(b"BSMH", b"25,26")
        packets = []
        for i, byte in enumerate(wire):
            packets.extend(reader.feed(bytes([byte]), i * 0.01))
        self.assertEqual(packets, [(b"BSMF", b"jpeg"), (b"BSMH", b"25,26")])

    def test_idle_poll_discards_partial_frame_before_new_data(self):
        reader = PacketReader()
        reader.feed(b"BSMH" + struct.pack(">I", 5000) + b"25", 0)
        self.assertEqual(reader.feed(b"", 1.1), [])
        self.assertEqual(reader.feed(self.packet(b"BSMH", b"26"), 1.2),
                         [(b"BSMH", b"26")])

    def test_silent_dead_handle_is_reopened_without_commands(self):
        device = Mock(is_open=True, in_waiting=0)
        device.close.side_effect = lambda: setattr(device, "is_open", False)
        device.open.side_effect = lambda: setattr(device, "is_open", True)
        connection = UsbConnection(device)
        self.assertEqual(connection.read_available(0), b"")
        self.assertIsNone(connection.read_available(5))
        self.assertIsNone(connection.read_available(5.2))
        device.in_waiting = 4
        device.read.return_value = b"BSMH"
        self.assertEqual(connection.read_available(5.5), b"BSMH")
        self.assertEqual(connection.generation, 1)
        device.write.assert_not_called()
class DeviceForecastTests(unittest.TestCase):
    def test_uses_device_forecast_without_recomputing(self):
        sample = {"alert_state":"active", "forecast_valid":True,
                  "temperature_humidity_valid":True, "forecast_7min_f":117.25,
                  "slope_f_per_min":-99}
        self.assertEqual(device_forecast_line(sample,180,90),([180,600],[90,117.25]))
        for key,value in (("forecast_valid",False),("alert_state","suppressed"),
                          ("temperature_humidity_valid",False),("forecast_7min_f",float("nan"))):
            changed=dict(sample);changed[key]=value
            self.assertEqual(device_forecast_line(changed,180,90),([],[]))
