"""Run with: python -m unittest discover -s tools -p 'test_*.py'."""

import itertools
import unittest
from unittest.mock import Mock

from usb_telemetry_viewer import detection_statuses, update_detection_labels, regression_line
from usb_telemetry_viewer import cellular_view, update_cellular_panel


class CellularStatusTests(unittest.TestCase):
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
        for status in ("SIM_NOT_READY", "NOT_REGISTERED", "RESULT_TIMEOUT", "NO_PHONE"):
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
