from __future__ import annotations

from dataclasses import dataclass
from math import sqrt
from statistics import median
from typing import Iterable

BPM_ACCEPTED_WINDOW = 5
BPM_ACCEPTED_DEVIATION_LIMIT = 0.18
BPM_DISPLAY_REFRESH_MS = 5000
BPM_DISPLAY_STEP_LIMIT = 3
BPM_INVALIDATE_TIMEOUT_MS = 10000
BPM_MIN_NEW_ACCEPTED_PER_REFRESH = 2
HRV_REFERENCE_WINDOW_MS = 30000
HRV_STABLE_WINDOW_MS = 60000
HRV_HOLD_MS = 15000
HRV_REFERENCE_DEVIATION_LIMIT = 0.30
HRV_STABLE_DEVIATION_LIMIT = 0.25
HRV_REFERENCE_SAMPLE_WINDOW = 7
STRESS_HIGH_THRESHOLD_MS = 25
STRESS_LOW_THRESHOLD_MS = 50

_BPM_COARSE_MIN_MS = 300
_BPM_COARSE_MAX_MS = 2000
_HRV_MIN_MS = 400
_HRV_MAX_MS = 1500
_HRV_REFERENCE_MIN_SAMPLES = 2
_HRV_STABLE_MIN_SAMPLES = 2


@dataclass(frozen=True)
class HealthMetricsConfig:
    bpm_accepted_window: int = BPM_ACCEPTED_WINDOW
    bpm_accepted_deviation_limit: float = BPM_ACCEPTED_DEVIATION_LIMIT
    bpm_display_refresh_ms: int = BPM_DISPLAY_REFRESH_MS
    bpm_display_step_limit: int = BPM_DISPLAY_STEP_LIMIT
    bpm_invalidate_timeout_ms: int = BPM_INVALIDATE_TIMEOUT_MS
    bpm_min_new_accepted_per_refresh: int = BPM_MIN_NEW_ACCEPTED_PER_REFRESH
    bpm_min_ms: int = _BPM_COARSE_MIN_MS
    bpm_max_ms: int = _BPM_COARSE_MAX_MS
    hrv_reference_sample_window: int = HRV_REFERENCE_SAMPLE_WINDOW
    hrv_reference_window_ms: int = HRV_REFERENCE_WINDOW_MS
    hrv_stable_window_ms: int = HRV_STABLE_WINDOW_MS
    hrv_hold_ms: int = HRV_HOLD_MS
    hrv_reference_deviation_limit: float = HRV_REFERENCE_DEVIATION_LIMIT
    hrv_stable_deviation_limit: float = HRV_STABLE_DEVIATION_LIMIT
    hrv_min_ms: int = _HRV_MIN_MS
    hrv_max_ms: int = _HRV_MAX_MS

    def __post_init__(self) -> None:
        if self.bpm_accepted_window <= 0:
            raise ValueError("bpm_accepted_window must be > 0")
        if self.bpm_display_step_limit < 0:
            raise ValueError("bpm_display_step_limit must be >= 0")
        if self.bpm_display_refresh_ms <= 0:
            raise ValueError("bpm_display_refresh_ms must be > 0")
        if self.bpm_invalidate_timeout_ms <= 0:
            raise ValueError("bpm_invalidate_timeout_ms must be > 0")
        if self.bpm_min_new_accepted_per_refresh < 0:
            raise ValueError("bpm_min_new_accepted_per_refresh must be >= 0")
        if self.hrv_reference_sample_window <= 0:
            raise ValueError("hrv_reference_sample_window must be > 0")
        if self.hrv_reference_window_ms <= 0:
            raise ValueError("hrv_reference_window_ms must be > 0")
        if self.hrv_stable_window_ms <= 0:
            raise ValueError("hrv_stable_window_ms must be > 0")
        if self.hrv_reference_window_ms > self.hrv_stable_window_ms:
            raise ValueError("hrv_reference_window_ms must be <= hrv_stable_window_ms")


@dataclass(frozen=True)
class HealthMetricsSnapshot:
    timestamp_ms: int = 0
    bpm: int | None = None
    bpm_valid: bool = False
    bpm_mode: str = "cold_start"
    hrv_rmssd: float | None = None
    hrv_sdnn: float | None = None
    hrv_valid: bool = False
    hrv_mode: str = "invalid"
    stress_level: str | None = None
    nn_bpm_valid: bool = False
    nn_hrv_valid: bool = False


def stress_level_from_rmssd(rmssd_ms: float | None) -> str | None:
    if rmssd_ms is None:
        return None
    if rmssd_ms < STRESS_HIGH_THRESHOLD_MS:
        return "高"
    if rmssd_ms <= STRESS_LOW_THRESHOLD_MS:
        return "中"
    return "低"


def _median(values: Iterable[int]) -> float:
    values_list = list(values)
    if not values_list:
        return 0.0
    return float(median(values_list))


class HealthMetricsEngine:
    def __init__(self, config: HealthMetricsConfig | None = None) -> None:
        self.config = config or HealthMetricsConfig()
        self._now_ms = 0
        self._accepted_nn: list[int] = []
        self._accepted_since_last_display = 0
        self._bpm_last_accepted_timestamp_ms: int | None = None
        self._bpm_last_display_update_ms: int | None = None
        self._hrv_samples: list[tuple[int, int]] = []
        self._latest_nn_bpm_valid = False
        self._latest_nn_hrv_valid = False

        self._bpm_mode = "cold_start"
        self._bpm_value: int | None = None
        self._bpm_valid = False
        self._hrv_rmssd: float | None = None
        self._hrv_sdnn: float | None = None
        self._hrv_valid = False
        self._hrv_mode = "invalid"
        self._stress_level: str | None = None
        self._hrv_last_good_timestamp_ms: int | None = None

        self._snapshot = HealthMetricsSnapshot()

    def ingest_nn(self, timestamp_ms: int, nn_ms: int) -> HealthMetricsSnapshot:
        self._validate_timestamp(timestamp_ms)
        self._now_ms = timestamp_ms
        self._latest_nn_bpm_valid = False
        self._latest_nn_hrv_valid = False

        self._reconcile_bpm_timeout(timestamp_ms)
        self._trim_hrv_samples(timestamp_ms)

        if self._is_bpm_valid_candidate(nn_ms):
            self._append_accepted_nn(nn_ms)
            self._bpm_last_accepted_timestamp_ms = timestamp_ms
            self._latest_nn_bpm_valid = True

            if not self._bpm_valid and len(self._accepted_nn) >= self.config.bpm_accepted_window:
                self._bpm_value = self._candidate_bpm()
                self._bpm_valid = self._bpm_value is not None
                self._bpm_mode = "normal" if self._bpm_valid else "cold_start"
                self._bpm_last_display_update_ms = timestamp_ms
                self._accepted_since_last_display = 0
            elif self._bpm_valid:
                self._accepted_since_last_display += 1

            if self._is_hrv_valid_candidate(timestamp_ms, nn_ms):
                self._hrv_samples.append((timestamp_ms, nn_ms))
                self._latest_nn_hrv_valid = True

        self._reconcile_bpm_display(timestamp_ms)
        self._reconcile_hrv(timestamp_ms)
        self._refresh_snapshot(timestamp_ms)
        return self._snapshot

    def advance(self, timestamp_ms: int) -> HealthMetricsSnapshot:
        self._validate_timestamp(timestamp_ms)
        self._now_ms = timestamp_ms

        self._latest_nn_bpm_valid = False
        self._latest_nn_hrv_valid = False
        self._reconcile_bpm_timeout(timestamp_ms)
        self._trim_hrv_samples(timestamp_ms)
        self._reconcile_bpm_display(timestamp_ms)
        self._reconcile_hrv(timestamp_ms)
        self._refresh_snapshot(timestamp_ms)
        return self._snapshot

    def snapshot(self) -> HealthMetricsSnapshot:
        return self._snapshot

    def _validate_timestamp(self, timestamp_ms: int) -> None:
        if timestamp_ms < self._now_ms:
            raise ValueError("timestamp_ms must be monotonic")

    def _is_bpm_valid_candidate(self, nn_ms: int) -> bool:
        if not self.config.bpm_min_ms <= nn_ms <= self.config.bpm_max_ms:
            return False

        if not self._accepted_nn:
            return True

        reference = _median(self._accepted_nn)
        return abs(nn_ms - reference) <= reference * self.config.bpm_accepted_deviation_limit

    def _append_accepted_nn(self, nn_ms: int) -> None:
        self._accepted_nn.append(nn_ms)
        if len(self._accepted_nn) > self.config.bpm_accepted_window:
            self._accepted_nn.pop(0)

    def _candidate_bpm(self) -> int | None:
        if len(self._accepted_nn) < self.config.bpm_accepted_window:
            return None
        return int(round(60000.0 / _median(self._accepted_nn)))

    def _invalidate_bpm(self) -> None:
        self._accepted_nn.clear()
        self._accepted_since_last_display = 0
        self._bpm_last_accepted_timestamp_ms = None
        self._bpm_last_display_update_ms = None
        self._bpm_value = None
        self._bpm_valid = False
        self._bpm_mode = "invalid"

    def _reconcile_bpm_timeout(self, timestamp_ms: int) -> None:
        if self._bpm_last_accepted_timestamp_ms is None:
            return

        if (
            timestamp_ms - self._bpm_last_accepted_timestamp_ms
            >= self.config.bpm_invalidate_timeout_ms
        ):
            self._invalidate_bpm()

    def _reconcile_bpm_display(self, timestamp_ms: int) -> None:
        if not self._bpm_valid or self._bpm_last_display_update_ms is None:
            return

        if (
            timestamp_ms - self._bpm_last_display_update_ms
            < self.config.bpm_display_refresh_ms
        ):
            return

        if (
            self._accepted_since_last_display
            < self.config.bpm_min_new_accepted_per_refresh
        ):
            self._accepted_since_last_display = 0
            self._bpm_last_display_update_ms = timestamp_ms
            return

        candidate_bpm = self._candidate_bpm()
        if candidate_bpm is None:
            return

        delta = candidate_bpm - int(self._bpm_value)
        if abs(delta) <= self.config.bpm_display_step_limit:
            self._bpm_value = candidate_bpm
        elif delta > 0:
            self._bpm_value += self.config.bpm_display_step_limit
        else:
            self._bpm_value -= self.config.bpm_display_step_limit

        self._accepted_since_last_display = 0
        self._bpm_last_display_update_ms = timestamp_ms
        self._bpm_mode = "normal"

    def _is_hrv_valid_candidate(self, timestamp_ms: int, nn_ms: int) -> bool:
        if not self.config.hrv_min_ms <= nn_ms <= self.config.hrv_max_ms:
            return False

        reference_values = [
            sample for _, sample in self._hrv_samples[-self.config.hrv_reference_sample_window :]
        ]
        if not reference_values:
            return True

        reference = _median(reference_values)
        deviation_limit = self.config.hrv_reference_deviation_limit
        if self._has_window_coverage(timestamp_ms, self.config.hrv_stable_window_ms):
            deviation_limit = self.config.hrv_stable_deviation_limit

        return abs(nn_ms - reference) <= reference * deviation_limit

    def _trim_hrv_samples(self, timestamp_ms: int) -> None:
        cutoff_ms = timestamp_ms - self.config.hrv_stable_window_ms

        while self._hrv_samples and self._hrv_samples[0][0] < cutoff_ms:
            self._hrv_samples.pop(0)

    def _window_samples(self, timestamp_ms: int, window_ms: int) -> list[int]:
        cutoff_ms = timestamp_ms - window_ms

        return [nn_ms for sample_ts, nn_ms in self._hrv_samples if sample_ts >= cutoff_ms]

    def _has_window_coverage(self, timestamp_ms: int, window_ms: int) -> bool:
        cutoff_ms = timestamp_ms - window_ms
        window_rows = [sample_ts for sample_ts, _ in self._hrv_samples if sample_ts >= cutoff_ms]
        if len(window_rows) < 2:
            return False
        if any(sample_ts <= cutoff_ms for sample_ts, _ in self._hrv_samples):
            return True
        return window_rows[-1] - window_rows[0] >= window_ms

    def _compute_rmssd(self, values: list[int]) -> float | None:
        if len(values) < 2:
            return None

        sum_sq = 0.0
        for previous, current in zip(values, values[1:]):
            diff = float(current - previous)
            sum_sq += diff * diff

        return sqrt(sum_sq / float(len(values) - 1))

    def _compute_sdnn(self, values: list[int]) -> float | None:
        if len(values) < 2:
            return None

        mean = sum(values) / float(len(values))
        sum_sq = 0.0
        for value in values:
            diff = float(value) - mean
            sum_sq += diff * diff

        return sqrt(sum_sq / float(len(values)))

    def _reconcile_hrv(self, timestamp_ms: int) -> None:
        stable_values = self._window_samples(timestamp_ms, self.config.hrv_stable_window_ms)
        reference_values = self._window_samples(timestamp_ms, self.config.hrv_reference_window_ms)

        if (
            len(stable_values) >= _HRV_STABLE_MIN_SAMPLES
            and self._has_window_coverage(timestamp_ms, self.config.hrv_stable_window_ms)
        ):
            self._hrv_rmssd = self._compute_rmssd(stable_values)
            self._hrv_sdnn = self._compute_sdnn(stable_values)
            self._hrv_valid = self._hrv_rmssd is not None and self._hrv_sdnn is not None
            self._hrv_mode = "stable" if self._hrv_valid else "invalid"
            self._stress_level = stress_level_from_rmssd(self._hrv_rmssd)
            if self._hrv_valid:
                self._hrv_last_good_timestamp_ms = timestamp_ms
            return

        if (
            len(reference_values) >= _HRV_REFERENCE_MIN_SAMPLES
            and self._has_window_coverage(timestamp_ms, self.config.hrv_reference_window_ms)
        ):
            self._hrv_rmssd = self._compute_rmssd(reference_values)
            self._hrv_sdnn = self._compute_sdnn(reference_values)
            self._hrv_valid = self._hrv_rmssd is not None and self._hrv_sdnn is not None
            self._hrv_mode = "reference" if self._hrv_valid else "invalid"
            self._stress_level = None
            if self._hrv_valid:
                self._hrv_last_good_timestamp_ms = timestamp_ms
            return

        if (
            self._hrv_last_good_timestamp_ms is not None
            and timestamp_ms - self._hrv_last_good_timestamp_ms <= self.config.hrv_hold_ms
            and self._hrv_rmssd is not None
            and self._hrv_sdnn is not None
        ):
            self._hrv_valid = True
            self._hrv_mode = "hold"
            return

        self._hrv_valid = False
        self._hrv_mode = "invalid"
        self._hrv_rmssd = None
        self._hrv_sdnn = None
        self._stress_level = None

    def _refresh_snapshot(self, timestamp_ms: int) -> None:
        self._snapshot = HealthMetricsSnapshot(
            timestamp_ms=timestamp_ms,
            bpm=self._bpm_value,
            bpm_valid=self._bpm_valid,
            bpm_mode=self._bpm_mode,
            hrv_rmssd=self._hrv_rmssd,
            hrv_sdnn=self._hrv_sdnn,
            hrv_valid=self._hrv_valid,
            hrv_mode=self._hrv_mode,
            stress_level=self._stress_level,
            nn_bpm_valid=self._latest_nn_bpm_valid,
            nn_hrv_valid=self._latest_nn_hrv_valid,
        )
