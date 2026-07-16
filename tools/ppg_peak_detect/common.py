from __future__ import annotations

from dataclasses import dataclass
from statistics import median

CSV_HEADER = "timestamp_ms,red,ir,filtered_ir,status"
BOARD_CSV_HEADER = (
    "timestamp_ms,red,ir,filtered_ir,threshold,peak_flag,ibi_ms,ibi_valid,"
    "signal_valid,status"
)
BOARD_CSV_HEADER_WITH_RESETS = (
    "timestamp_ms,red,ir,filtered_ir,threshold,peak_flag,ibi_ms,ibi_valid,"
    "signal_valid,pipeline_reset_flag,pipeline_reset_count,status"
)
BOARD_CSV_HEADER_WITH_DEBUG_STATE = (
    "timestamp_ms,red,ir,filtered_ir,threshold,peak_flag,ibi_ms,ibi_valid,"
    "signal_valid,pipeline_reset_flag,pipeline_reset_count,signal_level,"
    "noise_level,invalid_candidate_count,have_last_peak,status"
)
BOARD_CSV_HEADER_WITH_FINGER_DEBUG = (
    "timestamp_ms,red,ir,filtered_ir,threshold,peak_flag,ibi_ms,ibi_valid,"
    "signal_valid,pipeline_reset_flag,pipeline_reset_count,signal_level,"
    "noise_level,invalid_candidate_count,have_last_peak,finger_state,"
    "filtered_ir_p2p_1s,status"
)
CSV_HEADERS = frozenset(
    {
        CSV_HEADER,
        BOARD_CSV_HEADER,
        BOARD_CSV_HEADER_WITH_RESETS,
        BOARD_CSV_HEADER_WITH_DEBUG_STATE,
        BOARD_CSV_HEADER_WITH_FINGER_DEBUG,
    }
)
_BOARD_RESET_GAP_MS = 200

_INITIAL_SIGNAL_LEVEL = 8.0
_INITIAL_NOISE_LEVEL = 0.0


@dataclass(frozen=True)
class SampleRow:
    timestamp_ms: int
    red: int
    ir: int
    filtered_ir: int
    status: str


@dataclass(frozen=True)
class PeakDetectorConfig:
    min_peak_distance_ms: int = 360
    min_ibi_ms: int = 400
    max_ibi_ms: int = 1500
    median_window: int = 3
    min_peak_amplitude: int = 110
    threshold_fraction: float = 0.45
    level_decay: float = 0.875
    level_gain: float = 0.125
    ibi_deviation_limit: float = 0.25
    startup_ibi_deviation_limit: float = 0.30
    invalid_candidate_limit: int = 4

    def __post_init__(self) -> None:
        if self.median_window < 1:
            raise ValueError("median_window must be >= 1")
        if self.min_peak_amplitude < 1:
            raise ValueError("min_peak_amplitude must be >= 1")


@dataclass(frozen=True)
class PeakDetectorOutput:
    peak_detected: bool
    ibi_valid: bool
    signal_valid: bool
    peak_timestamp_ms: int
    ibi_ms: int
    threshold: int
    candidate_value: int


def parse_sample_line(line: str) -> SampleRow | None:
    text = line.strip()
    if not text or text in CSV_HEADERS:
        return None

    parts = text.split(",")
    if len(parts) == 5:
        timestamp_ms, red, ir, filtered_ir, status = parts
    elif len(parts) in (10, 12, 16, 18):
        timestamp_ms, red, ir, filtered_ir, *_, status = parts
    else:
        return None

    try:
        if status != "ok":
            return None

        return SampleRow(
            timestamp_ms=int(timestamp_ms),
            red=int(red),
            ir=int(ir),
            filtered_ir=int(filtered_ir),
            status=status,
        )
    except ValueError:
        return None


class PeakDetector:
    def __init__(self, config: PeakDetectorConfig | None = None) -> None:
        self.config = config or PeakDetectorConfig()
        self._rows: list[SampleRow] = []
        self._signal_level = _INITIAL_SIGNAL_LEVEL
        self._noise_level = _INITIAL_NOISE_LEVEL
        self._last_peak_timestamp_ms: int | None = None
        self._valid_ibis: list[int] = []
        self._invalid_candidate_count = 0
        self._signal_valid = False
        self._startup_reference_ibi_ms: int | None = None
        self._last_accepted_ibi_ms: int | None = None

    def push(self, row: SampleRow) -> PeakDetectorOutput:
        self._rows.append(row)
        self._expire_signal_valid(row.timestamp_ms)
        if len(self._rows) < 3:
            return PeakDetectorOutput(False, False, self._signal_valid, 0, 0, 0, 0)
        if len(self._rows) > 3:
            self._rows.pop(0)

        prev_row = self._rows[1]
        candidate_value = prev_row.filtered_ir
        threshold = int(round(self._current_threshold()))

        if not self._is_local_peak():
            return PeakDetectorOutput(
                False, False, self._signal_valid, 0, 0, threshold, candidate_value
            )

        if candidate_value < self.config.min_peak_amplitude or candidate_value <= threshold:
            self._noise_level = self._blend(self._noise_level, float(candidate_value))
            self._invalid_candidate_count += 1
            if self._invalid_candidate_count > self.config.invalid_candidate_limit:
                self._signal_valid = False
            return PeakDetectorOutput(
                False, False, self._signal_valid, 0, 0, threshold, candidate_value
            )

        candidate_timestamp = prev_row.timestamp_ms
        if self._last_peak_timestamp_ms is not None:
            distance_ms = candidate_timestamp - self._last_peak_timestamp_ms
            if distance_ms < self.config.min_peak_distance_ms:
                self._noise_level = self._blend(self._noise_level, float(candidate_value))
                self._invalid_candidate_count += 1
                if self._invalid_candidate_count > self.config.invalid_candidate_limit:
                    self._signal_valid = False
                return PeakDetectorOutput(
                    False, False, self._signal_valid, 0, 0, threshold, candidate_value
                )

        self._signal_level = self._blend(self._signal_level, float(candidate_value))
        self._invalid_candidate_count = 0
        self._signal_valid = True

        ibi_ms = 0
        ibi_valid = False
        if self._last_peak_timestamp_ms is not None:
            ibi_ms = candidate_timestamp - self._last_peak_timestamp_ms
            ibi_valid = self._is_valid_ibi(ibi_ms)
            if ibi_valid:
                if self._startup_reference_ibi_ms is None:
                    self._startup_reference_ibi_ms = ibi_ms
                self._last_accepted_ibi_ms = ibi_ms
                self._valid_ibis.append(ibi_ms)
                if len(self._valid_ibis) > self.config.median_window:
                    self._valid_ibis.pop(0)

        self._last_peak_timestamp_ms = candidate_timestamp
        return PeakDetectorOutput(
            True,
            ibi_valid,
            self._signal_valid,
            candidate_timestamp,
            ibi_ms,
            threshold,
            candidate_value,
        )

    def _is_local_peak(self) -> bool:
        left, center, right = self._rows
        return center.filtered_ir > left.filtered_ir and center.filtered_ir >= right.filtered_ir

    def _current_threshold(self) -> float:
        return self._noise_level + self.config.threshold_fraction * (
            self._signal_level - self._noise_level
        )

    def _blend(self, old_value: float, new_value: float) -> float:
        return (self.config.level_decay * old_value) + (self.config.level_gain * new_value)

    def _within_percent(self, value: int, reference: int, limit: float) -> bool:
        return abs(value - reference) <= reference * limit

    def _expire_signal_valid(self, timestamp_ms: int) -> None:
        if self._last_peak_timestamp_ms is None:
            return

        if timestamp_ms - self._last_peak_timestamp_ms > self.config.max_ibi_ms:
            self._signal_valid = False

    def _is_valid_ibi(self, ibi_ms: int) -> bool:
        if ibi_ms < self.config.min_ibi_ms or ibi_ms > self.config.max_ibi_ms:
            return False

        if not self._valid_ibis:
            return True

        if len(self._valid_ibis) < self.config.median_window:
            assert self._startup_reference_ibi_ms is not None
            if len(self._valid_ibis) == 1:
                return self._within_percent(
                    ibi_ms,
                    self._startup_reference_ibi_ms,
                    self.config.startup_ibi_deviation_limit,
                )

            assert self._last_accepted_ibi_ms is not None
            return self._within_percent(
                ibi_ms,
                self._startup_reference_ibi_ms,
                self.config.startup_ibi_deviation_limit,
            ) and self._within_percent(
                ibi_ms,
                self._last_accepted_ibi_ms,
                self.config.startup_ibi_deviation_limit,
            )

        local_median = median(self._valid_ibis[-self.config.median_window :])
        return abs(ibi_ms - local_median) <= local_median * self.config.ibi_deviation_limit


def run_detector(rows: list[SampleRow]) -> list[PeakDetectorOutput]:
    detector = PeakDetector()
    outputs: list[PeakDetectorOutput] = []
    previous_timestamp_ms: int | None = None

    for row in rows:
        if (
            previous_timestamp_ms is not None
            and row.timestamp_ms - previous_timestamp_ms >= _BOARD_RESET_GAP_MS
        ):
            detector = PeakDetector()

        outputs.append(detector.push(row))
        previous_timestamp_ms = row.timestamp_ms

    return outputs
