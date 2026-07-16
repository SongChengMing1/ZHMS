import unittest

from tools.ppg_preprocess.common import (
    BASELINE_ALPHA,
    LOWPASS_A1,
    LOWPASS_A2,
    LOWPASS_B0,
    LOWPASS_B1,
    LOWPASS_B2,
    MAX_STEP_DELTA,
    apply_preprocess_chain,
    max_gap_ms,
    normalize_series,
    parse_sample_line,
    SampleRow,
)


class Task3CommonTests(unittest.TestCase):
    def test_preprocess_constants_match_100hz_retune(self) -> None:
        self.assertAlmostEqual(BASELINE_ALPHA, 0.03093, places=5)
        self.assertAlmostEqual(LOWPASS_B0, 0.016581932082772255, places=12)
        self.assertAlmostEqual(LOWPASS_B1, 0.03316386416554451, places=12)
        self.assertAlmostEqual(LOWPASS_B2, 0.016581932082772255, places=12)
        self.assertAlmostEqual(LOWPASS_A1, -1.6041301488876343, places=12)
        self.assertAlmostEqual(LOWPASS_A2, 0.6704578995704651, places=12)
        self.assertEqual(MAX_STEP_DELTA, 12000)

    def test_parse_sample_line_filters_logs(self) -> None:
        self.assertIsNone(parse_sample_line("LOG_INF booting"))
        row = parse_sample_line("123,456,789,321,ok")
        self.assertIsNotNone(row)
        assert row is not None
        self.assertEqual(row.timestamp_ms, 123)
        self.assertEqual(row.red, 456)
        self.assertEqual(row.ir, 789)
        self.assertEqual(row.filtered_ir, 321)
        self.assertEqual(row.status, "ok")

    def test_normalize_series_handles_flat_data(self) -> None:
        self.assertEqual(normalize_series([7, 7, 7]), [0.5, 0.5, 0.5])

    def test_normalize_series_scales_non_flat_data(self) -> None:
        self.assertEqual(normalize_series([10, 20, 30]), [0.0, 0.5, 1.0])

    def test_max_gap_ms_reports_largest_interval(self) -> None:
        rows = [
            SampleRow(100, 1, 2, 3, "ok"),
            SampleRow(120, 4, 5, 6, "ok"),
            SampleRow(160, 7, 8, 9, "ok"),
        ]
        self.assertEqual(max_gap_ms(rows), 40)

    def test_apply_preprocess_chain_keeps_flat_signal_at_zero(self) -> None:
        self.assertEqual(apply_preprocess_chain([100, 100, 100, 100]), [0, 0, 0, 0])

    def test_apply_preprocess_chain_matches_required_median_spike_sequence(self) -> None:
        self.assertEqual(
            apply_preprocess_chain([100000, 100050, 140000, 100060, 100050]),
            [0, 1, 4, 9, 15],
        )

    def test_apply_preprocess_chain_matches_100hz_reference_sequence(self) -> None:
        self.assertEqual(
            apply_preprocess_chain([100000, 101000, 102000, 101500, 100800]),
            [0, 16, 73, 177, 320],
        )

    def test_apply_preprocess_chain_clamp_feedback_matches_firmware(self) -> None:
        self.assertEqual(
            apply_preprocess_chain([100000, 200000, 100000, 200000, 100000]),
            [0, 1607, 5742, 11199, 17087],
        )

    def test_apply_preprocess_chain_matches_float32_firmware_sequence(self) -> None:
        self.assertEqual(
            apply_preprocess_chain([123983, 111474, 121474, 119639, 106699, 89697]),
            [0, -201, -759, -1414, -1979, -2677],
        )

    def test_apply_preprocess_chain_clamps_large_spike(self) -> None:
        outputs = apply_preprocess_chain([100000, 100050, 140000])
        self.assertEqual(outputs[0], 0)
        self.assertLessEqual(outputs[2] - outputs[1], MAX_STEP_DELTA)


if __name__ == "__main__":
    unittest.main()
