"""Run with: python -m unittest discover -s tools -p 'test_*.py'."""

import itertools
import unittest
from unittest.mock import Mock

from usb_telemetry_viewer import detection_statuses, update_detection_labels, regression_line


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
