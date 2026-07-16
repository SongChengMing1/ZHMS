import unittest

from tools.health_metrics.common import (
    HealthMetricsConfig,
    HealthMetricsEngine,
    stress_level_from_rmssd,
)

BASELINE_NN = [800, 810, 790, 805, 800]
WARMUP_NN = [800, 810, 790, 805]
SHIFT_NN = [700, 710, 700, 705]
# Within the coarse BPM range, but outside the approved ~800ms baseline rhythm window.
REJECTED_OUTLIER_NN = 1300
OUTLIER_PERIOD_MS = 1300
RECOVERY_ACCEPTED_NN = [800, 810, 790, 805, 800]


def feed_nn_sequence(
    engine: HealthMetricsEngine, start_ms: int, nn_values: list[int], repeats: int = 1
):
    timestamp_ms = start_ms
    snapshot = engine.snapshot()

    for _ in range(repeats):
        for nn_ms in nn_values:
            timestamp_ms += nn_ms
            snapshot = engine.ingest_nn(timestamp_ms, nn_ms)

    return snapshot, timestamp_ms


class Task5CommonTests(unittest.TestCase):
    def test_config_rejects_non_positive_bpm_accepted_window(self) -> None:
        with self.assertRaises(ValueError):
            HealthMetricsConfig(bpm_accepted_window=0)

    def test_config_rejects_non_positive_hrv_reference_sample_window(self) -> None:
        with self.assertRaises(ValueError):
            HealthMetricsConfig(hrv_reference_sample_window=0)

    def test_config_rejects_invalid_hrv_window_ordering(self) -> None:
        with self.assertRaises(ValueError):
            HealthMetricsConfig(hrv_reference_window_ms=60001, hrv_stable_window_ms=60000)

    def test_bpm_waits_for_five_accepted_samples_before_first_value(self) -> None:
        engine = HealthMetricsEngine()

        snapshot, timestamp_ms = feed_nn_sequence(engine, 0, WARMUP_NN)
        self.assertFalse(snapshot.bpm_valid)

        snapshot = engine.ingest_nn(timestamp_ms + 800, 800)
        self.assertTrue(snapshot.bpm_valid)
        self.assertEqual(snapshot.bpm, 75)

    def test_cold_start_rejected_outlier_does_not_advance_five_sample_count(
        self,
    ) -> None:
        engine = HealthMetricsEngine()

        snapshot, timestamp_ms = feed_nn_sequence(engine, 0, WARMUP_NN)

        timestamp_ms += OUTLIER_PERIOD_MS
        snapshot = engine.ingest_nn(timestamp_ms, REJECTED_OUTLIER_NN)
        self.assertFalse(snapshot.bpm_valid)

        timestamp_ms += 800
        snapshot = engine.ingest_nn(timestamp_ms, 800)
        self.assertTrue(snapshot.bpm_valid)
        self.assertEqual(snapshot.bpm, 75)

    def test_rejected_outlier_does_not_change_display_bpm_before_refresh(self) -> None:
        engine = HealthMetricsEngine()

        snapshot, timestamp_ms = feed_nn_sequence(engine, 0, BASELINE_NN)
        timestamp_ms += OUTLIER_PERIOD_MS
        snapshot = engine.ingest_nn(timestamp_ms, REJECTED_OUTLIER_NN)
        timestamp_ms += OUTLIER_PERIOD_MS
        snapshot = engine.ingest_nn(timestamp_ms, REJECTED_OUTLIER_NN)

        self.assertTrue(snapshot.bpm_valid)
        self.assertEqual(snapshot.bpm, 75)

        snapshot = engine.advance(timestamp_ms + 2300)
        self.assertTrue(snapshot.bpm_valid)
        self.assertEqual(snapshot.bpm, 75)

    def test_accepted_shift_does_not_change_display_bpm_before_refresh(self) -> None:
        engine = HealthMetricsEngine()

        snapshot, baseline_timestamp_ms = feed_nn_sequence(engine, 0, BASELINE_NN)
        snapshot, _ = feed_nn_sequence(engine, baseline_timestamp_ms, SHIFT_NN)

        self.assertTrue(snapshot.bpm_valid)
        self.assertEqual(snapshot.bpm, 75)

        snapshot = engine.advance(baseline_timestamp_ms + 4900)
        self.assertTrue(snapshot.bpm_valid)
        self.assertEqual(snapshot.bpm, 75)

    def test_display_bpm_only_refreshes_every_five_seconds_with_step_limit(
        self,
    ) -> None:
        engine = HealthMetricsEngine()

        _, baseline_timestamp_ms = feed_nn_sequence(engine, 0, BASELINE_NN)
        feed_nn_sequence(engine, baseline_timestamp_ms, SHIFT_NN)

        after_refresh = engine.advance(baseline_timestamp_ms + 5000)
        self.assertTrue(after_refresh.bpm_valid)
        self.assertEqual(after_refresh.bpm, 78)

    def test_rejected_outlier_does_not_refresh_the_ten_second_timeout(self) -> None:
        engine = HealthMetricsEngine()

        snapshot, timestamp_ms = feed_nn_sequence(engine, 0, BASELINE_NN)
        for _ in range(8):
            timestamp_ms += OUTLIER_PERIOD_MS
            snapshot = engine.ingest_nn(timestamp_ms, REJECTED_OUTLIER_NN)

        self.assertFalse(snapshot.bpm_valid)

    def test_bpm_invalidates_at_exact_ten_second_gap(self) -> None:
        engine = HealthMetricsEngine()

        snapshot, timestamp_ms = feed_nn_sequence(engine, 0, BASELINE_NN)
        snapshot = engine.advance(timestamp_ms + 10000)

        self.assertFalse(snapshot.bpm_valid)
        self.assertEqual(snapshot.bpm_mode, "invalid")

    def test_invalid_bpm_requires_five_new_samples_to_recover(self) -> None:
        engine = HealthMetricsEngine()

        snapshot, timestamp_ms = feed_nn_sequence(engine, 0, BASELINE_NN)
        timestamp_ms += 10000
        snapshot = engine.advance(timestamp_ms)
        self.assertFalse(snapshot.bpm_valid)

        timestamp_ms += 800
        snapshot = engine.ingest_nn(timestamp_ms, RECOVERY_ACCEPTED_NN[0])
        self.assertFalse(snapshot.bpm_valid)

        timestamp_ms += 810
        snapshot = engine.ingest_nn(timestamp_ms, RECOVERY_ACCEPTED_NN[1])
        self.assertFalse(snapshot.bpm_valid)

        timestamp_ms += 790
        snapshot = engine.ingest_nn(timestamp_ms, RECOVERY_ACCEPTED_NN[2])
        self.assertFalse(snapshot.bpm_valid)

        timestamp_ms += 805
        snapshot = engine.ingest_nn(timestamp_ms, RECOVERY_ACCEPTED_NN[3])
        self.assertFalse(snapshot.bpm_valid)

        timestamp_ms += 800
        snapshot = engine.ingest_nn(timestamp_ms, RECOVERY_ACCEPTED_NN[4])
        self.assertTrue(snapshot.bpm_valid)
        self.assertEqual(snapshot.bpm, 75)

    def test_first_post_gap_beat_does_not_keep_old_bpm_valid(self) -> None:
        engine = HealthMetricsEngine()

        snapshot, timestamp_ms = feed_nn_sequence(engine, 0, BASELINE_NN)
        timestamp_ms += 10801
        snapshot = engine.ingest_nn(timestamp_ms, 800)

        self.assertFalse(snapshot.bpm_valid)
        self.assertEqual(snapshot.bpm_mode, "invalid")

        timestamp_ms += 810
        snapshot = engine.ingest_nn(timestamp_ms, 810)
        self.assertFalse(snapshot.bpm_valid)

        timestamp_ms += 790
        snapshot = engine.ingest_nn(timestamp_ms, 790)
        self.assertFalse(snapshot.bpm_valid)

        timestamp_ms += 805
        snapshot = engine.ingest_nn(timestamp_ms, 805)
        self.assertFalse(snapshot.bpm_valid)

        timestamp_ms += 800
        snapshot = engine.ingest_nn(timestamp_ms, 800)
        self.assertTrue(snapshot.bpm_valid)
        self.assertEqual(snapshot.bpm, 75)

    def test_ingest_nn_rejects_backward_timestamp(self) -> None:
        engine = HealthMetricsEngine()

        engine.ingest_nn(800, 800)

        with self.assertRaises(ValueError):
            engine.ingest_nn(799, 800)

    def test_reference_hrv_becomes_valid_after_30s_window(self) -> None:
        engine = HealthMetricsEngine()

        snapshot, _ = feed_nn_sequence(engine, 0, [800, 820, 790, 810], repeats=11)

        self.assertTrue(snapshot.hrv_valid)
        self.assertEqual(snapshot.hrv_mode, "reference")
        self.assertGreater(snapshot.hrv_rmssd, 0.0)
        self.assertGreater(snapshot.hrv_sdnn, 0.0)
        self.assertTrue(snapshot.nn_hrv_valid)
        self.assertIsNone(snapshot.stress_level)

    def test_stable_hrv_enables_stress_level_after_60s_window(self) -> None:
        engine = HealthMetricsEngine()

        snapshot, _ = feed_nn_sequence(engine, 0, [800, 805, 795, 800], repeats=20)

        self.assertTrue(snapshot.hrv_valid)
        self.assertEqual(snapshot.hrv_mode, "stable")
        self.assertGreater(snapshot.hrv_rmssd, 0.0)
        self.assertGreater(snapshot.hrv_sdnn, 0.0)
        self.assertEqual(snapshot.stress_level, "高")

    def test_first_post_gap_hrv_sample_uses_trimmed_history(self) -> None:
        engine = HealthMetricsEngine()

        snapshot, timestamp_ms = feed_nn_sequence(engine, 0, [800, 820, 790, 810], repeats=11)
        self.assertTrue(snapshot.hrv_valid)

        timestamp_ms += 61000
        snapshot = engine.ingest_nn(timestamp_ms, 1200)

        self.assertTrue(snapshot.nn_hrv_valid)

    def test_advance_rejects_backward_timestamp(self) -> None:
        engine = HealthMetricsEngine()

        engine.advance(1000)

        with self.assertRaises(ValueError):
            engine.advance(999)

    def test_stress_level_thresholds_follow_rmssd_cutoffs(self) -> None:
        self.assertEqual(stress_level_from_rmssd(None), None)
        self.assertEqual(stress_level_from_rmssd(24.9), "高")
        self.assertEqual(stress_level_from_rmssd(25.0), "中")
        self.assertEqual(stress_level_from_rmssd(50.0), "中")
        self.assertEqual(stress_level_from_rmssd(50.1), "低")


if __name__ == "__main__":
    unittest.main()
