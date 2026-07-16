import unittest

from tools.ppg_peak_detect.common import (
    PeakDetector,
    PeakDetectorConfig,
    SampleRow,
    parse_sample_line,
    run_detector,
)


class Task4CommonTests(unittest.TestCase):
    def test_detector_uses_expected_default_constants(self) -> None:
        config = PeakDetectorConfig()
        self.assertEqual(config.min_peak_distance_ms, 360)
        self.assertEqual(config.min_ibi_ms, 400)
        self.assertEqual(config.max_ibi_ms, 1500)
        self.assertEqual(config.median_window, 3)
        self.assertAlmostEqual(config.threshold_fraction, 0.45, places=6)
        self.assertAlmostEqual(config.level_decay, 0.875, places=6)
        self.assertAlmostEqual(config.level_gain, 0.125, places=6)
        self.assertAlmostEqual(config.ibi_deviation_limit, 0.25, places=6)
        self.assertAlmostEqual(config.startup_ibi_deviation_limit, 0.30, places=6)
        self.assertEqual(config.min_peak_amplitude, 110)

    def test_detector_rejects_startup_ibi_that_jumps_too_far_from_reference(self) -> None:
        detector = PeakDetector()
        rows = [
            SampleRow(0, 0, 0, 0, "ok"),
            SampleRow(100, 0, 0, 20, "ok"),
            SampleRow(200, 0, 0, 120, "ok"),
            SampleRow(300, 0, 0, 30, "ok"),
            SampleRow(400, 0, 0, 0, "ok"),
            SampleRow(900, 0, 0, 20, "ok"),
            SampleRow(1000, 0, 0, 130, "ok"),
            SampleRow(1100, 0, 0, 40, "ok"),
            SampleRow(1200, 0, 0, 0, "ok"),
            SampleRow(1400, 0, 0, 20, "ok"),
            SampleRow(1500, 0, 0, 125, "ok"),
            SampleRow(1600, 0, 0, 35, "ok"),
            SampleRow(1700, 0, 0, 0, "ok"),
            SampleRow(2200, 0, 0, 20, "ok"),
            SampleRow(2300, 0, 0, 128, "ok"),
            SampleRow(2400, 0, 0, 40, "ok"),
            SampleRow(2500, 0, 0, 0, "ok"),
        ]

        outputs = [detector.push(row) for row in rows]
        # Lock the exact startup peak sequence the host regression depends on.
        observed_peaks = [
            (out.peak_timestamp_ms, out.ibi_ms, out.ibi_valid)
            for out in outputs
            if out.peak_detected
        ]

        self.assertEqual(
            observed_peaks,
            [
                (200, 0, False),
                (1000, 800, True),
                (1500, 500, False),
                (2300, 800, True),
            ],
        )

    def test_detector_accepts_startup_ibi_on_inclusive_30_percent_boundary(self) -> None:
        detector = PeakDetector()
        rows = [
            SampleRow(0, 0, 0, 0, "ok"),
            SampleRow(100, 0, 0, 20, "ok"),
            SampleRow(200, 0, 0, 120, "ok"),
            SampleRow(300, 0, 0, 30, "ok"),
            SampleRow(400, 0, 0, 0, "ok"),
            SampleRow(900, 0, 0, 20, "ok"),
            SampleRow(1000, 0, 0, 130, "ok"),
            SampleRow(1100, 0, 0, 40, "ok"),
            SampleRow(1200, 0, 0, 0, "ok"),
            SampleRow(1940, 0, 0, 20, "ok"),
            SampleRow(2040, 0, 0, 130, "ok"),
            SampleRow(2140, 0, 0, 40, "ok"),
        ]

        outputs = [detector.push(row) for row in rows]
        observed_peaks = [
            (out.peak_timestamp_ms, out.ibi_ms, out.ibi_valid)
            for out in outputs
            if out.peak_detected
        ]

        self.assertEqual(
            observed_peaks,
            [
                (200, 0, False),
                (1000, 800, True),
                (2040, 1040, True),
            ],
        )

    def test_detector_rejects_third_startup_ibi_that_only_matches_one_reference(
        self,
    ) -> None:
        detector = PeakDetector()
        rows = [
            SampleRow(0, 0, 0, 0, "ok"),
            SampleRow(100, 0, 0, 20, "ok"),
            SampleRow(200, 0, 0, 120, "ok"),
            SampleRow(300, 0, 0, 30, "ok"),
            SampleRow(400, 0, 0, 0, "ok"),
            SampleRow(900, 0, 0, 20, "ok"),
            SampleRow(1000, 0, 0, 130, "ok"),
            SampleRow(1100, 0, 0, 40, "ok"),
            SampleRow(1200, 0, 0, 0, "ok"),
            SampleRow(1860, 0, 0, 20, "ok"),
            SampleRow(1960, 0, 0, 128, "ok"),
            SampleRow(2060, 0, 0, 40, "ok"),
            SampleRow(2160, 0, 0, 0, "ok"),
            SampleRow(2420, 0, 0, 20, "ok"),
            SampleRow(2520, 0, 0, 128, "ok"),
            SampleRow(2620, 0, 0, 40, "ok"),
        ]

        outputs = [detector.push(row) for row in rows]
        observed_peaks = [
            (out.peak_timestamp_ms, out.ibi_ms, out.ibi_valid)
            for out in outputs
            if out.peak_detected
        ]

        self.assertEqual(
            observed_peaks,
            [
                (200, 0, False),
                (1000, 800, True),
                (1960, 960, True),
                (2520, 560, False),
            ],
        )

    def test_detector_rejects_low_amplitude_peak_candidates(self) -> None:
        detector = PeakDetector()
        rows = [
            SampleRow(0, 0, 0, 0, "ok"),
            SampleRow(100, 0, 0, 20, "ok"),
            SampleRow(200, 0, 0, 95, "ok"),
            SampleRow(300, 0, 0, 30, "ok"),
            SampleRow(400, 0, 0, 0, "ok"),
        ]

        outputs = [detector.push(row) for row in rows]
        self.assertFalse(any(out.peak_detected for out in outputs))
        self.assertFalse(any(out.ibi_valid for out in outputs))
        self.assertFalse(outputs[-1].signal_valid)

    def test_detector_rejects_invalid_median_window_at_config_construction(self) -> None:
        with self.assertRaises(ValueError):
            PeakDetectorConfig(median_window=0)

    def test_parse_sample_line_accepts_board_export_row(self) -> None:
        row = parse_sample_line(
            "100,12,34,56,78,1,0,1,0,ok"
        )

        self.assertEqual(
            row,
            SampleRow(timestamp_ms=100, red=12, ir=34, filtered_ir=56, status="ok"),
        )

    def test_parse_sample_line_accepts_board_export_row_with_reset_diagnostics(self) -> None:
        row = parse_sample_line(
            "100,12,34,56,78,1,0,1,0,1,3,ok"
        )

        self.assertEqual(
            row,
            SampleRow(timestamp_ms=100, red=12, ir=34, filtered_ir=56, status="ok"),
        )

    def test_parse_sample_line_accepts_board_export_row_with_debug_state(self) -> None:
        row = parse_sample_line(
            "100,12,34,56,78,1,0,1,0,1,3,8.000,0.000,0,0,ok"
        )

        self.assertEqual(
            row,
            SampleRow(timestamp_ms=100, red=12, ir=34, filtered_ir=56, status="ok"),
        )

    def test_parse_sample_line_accepts_board_export_row_with_finger_debug_columns(self) -> None:
        row = parse_sample_line(
            "100,12,34,56,78,1,0,1,0,1,3,8.000,0.000,0,1,77,123,ok"
        )

        self.assertEqual(
            row,
            SampleRow(timestamp_ms=100, red=12, ir=34, filtered_ir=56, status="ok"),
        )

    def test_detector_finds_regular_peaks_and_valid_ibis(self) -> None:
        detector = PeakDetector()
        rows = [
            SampleRow(0, 0, 0, 0, "ok"),
            SampleRow(100, 0, 0, 20, "ok"),
            SampleRow(200, 0, 0, 120, "ok"),
            SampleRow(300, 0, 0, 30, "ok"),
            SampleRow(400, 0, 0, 0, "ok"),
            SampleRow(900, 0, 0, 20, "ok"),
            SampleRow(1000, 0, 0, 130, "ok"),
            SampleRow(1100, 0, 0, 40, "ok"),
            SampleRow(1200, 0, 0, 0, "ok"),
        ]

        outputs = [detector.push(row) for row in rows]
        peaks = [out for out in outputs if out.peak_detected]
        ibis = [out.ibi_ms for out in outputs if out.ibi_valid]

        self.assertEqual([peak.peak_timestamp_ms for peak in peaks], [200, 1000])
        self.assertEqual(ibis, [800])

    def test_detector_rejects_second_peak_inside_minimum_distance(self) -> None:
        detector = PeakDetector()
        rows = [
            SampleRow(0, 0, 0, 0, "ok"),
            SampleRow(100, 0, 0, 20, "ok"),
            SampleRow(200, 0, 0, 120, "ok"),
            SampleRow(300, 0, 0, 30, "ok"),
            SampleRow(420, 0, 0, 40, "ok"),
            SampleRow(500, 0, 0, 130, "ok"),
            SampleRow(600, 0, 0, 40, "ok"),
        ]

        outputs = [detector.push(row) for row in rows]
        peaks = [out.peak_timestamp_ms for out in outputs if out.peak_detected]

        self.assertEqual(peaks, [200])

    def test_detector_rejects_ibi_outside_physiologic_range(self) -> None:
        detector = PeakDetector()
        rows = [
            SampleRow(0, 0, 0, 0, "ok"),
            SampleRow(100, 0, 0, 20, "ok"),
            SampleRow(200, 0, 0, 120, "ok"),
            SampleRow(300, 0, 0, 30, "ok"),
            SampleRow(450, 0, 0, 20, "ok"),
            SampleRow(500, 0, 0, 130, "ok"),
            SampleRow(560, 0, 0, 40, "ok"),
        ]

        outputs = [detector.push(row) for row in rows]
        self.assertFalse(any(out.ibi_valid for out in outputs))

    def test_run_detector_resets_after_timestamp_gap(self) -> None:
        rows = [
            SampleRow(0, 0, 0, 0, "ok"),
            SampleRow(100, 0, 0, 20, "ok"),
            SampleRow(200, 0, 0, 120, "ok"),
            SampleRow(300, 0, 0, 30, "ok"),
            SampleRow(400, 0, 0, 0, "ok"),
            SampleRow(600, 0, 0, 0, "ok"),
            SampleRow(700, 0, 0, 20, "ok"),
            SampleRow(800, 0, 0, 130, "ok"),
            SampleRow(900, 0, 0, 40, "ok"),
            SampleRow(1000, 0, 0, 0, "ok"),
        ]

        outputs = run_detector(rows)
        peaks = [out for out in outputs if out.peak_detected]

        self.assertEqual([peak.peak_timestamp_ms for peak in peaks], [200, 800])
        self.assertEqual(peaks[1].ibi_ms, 0)
        self.assertFalse(peaks[1].ibi_valid)

    def test_detector_clears_signal_valid_after_flatline_without_new_candidates(self) -> None:
        detector = PeakDetector()
        rows = [
            SampleRow(0, 0, 0, 0, "ok"),
            SampleRow(100, 0, 0, 20, "ok"),
            SampleRow(200, 0, 0, 120, "ok"),
            SampleRow(300, 0, 0, 30, "ok"),
            SampleRow(400, 0, 0, 0, "ok"),
            SampleRow(900, 0, 0, 20, "ok"),
            SampleRow(1000, 0, 0, 130, "ok"),
            SampleRow(1100, 0, 0, 40, "ok"),
            SampleRow(1200, 0, 0, 0, "ok"),
        ]
        rows.extend(
            SampleRow(timestamp, 0, 0, 0, "ok")
            for timestamp in range(1300, 3201, 100)
        )

        outputs = [detector.push(row) for row in rows]
        self.assertTrue(any(out.signal_valid for out in outputs))
        self.assertFalse(outputs[-1].signal_valid)

    def test_detector_marks_signal_invalid_when_only_noise_candidates_arrive(self) -> None:
        detector = PeakDetector()
        rows = [
            SampleRow(0, 0, 0, 0, "ok"),
            SampleRow(100, 0, 0, 1, "ok"),
            SampleRow(200, 0, 0, 2, "ok"),
            SampleRow(300, 0, 0, 1, "ok"),
            SampleRow(400, 0, 0, 0, "ok"),
            SampleRow(500, 0, 0, 1, "ok"),
            SampleRow(600, 0, 0, 2, "ok"),
            SampleRow(700, 0, 0, 1, "ok"),
        ]

        outputs = [detector.push(row) for row in rows]
        self.assertFalse(outputs[-1].signal_valid)


if __name__ == "__main__":
    unittest.main()
