"""Run with: python -m unittest discover -s tools -p 'test_*.py'."""

import itertools
import unittest
from unittest.mock import Mock

from usb_telemetry_viewer import detection_statuses, update_detection_labels


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
