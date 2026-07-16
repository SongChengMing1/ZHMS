from __future__ import annotations

from dataclasses import dataclass
import math
import re
import struct

CSV_HEADER = "timestamp_ms,red,ir,filtered_ir,status"
VALID_STATUSES = {"ok"}
ROW_RE = re.compile(
    r"^(?P<timestamp_ms>\d+),(?P<red>-?\d+),(?P<ir>-?\d+),"
    r"(?P<filtered_ir>-?\d+),(?P<status>ok)$"
)

BASELINE_ALPHA = 0.03093
LOWPASS_B0 = 0.016581932082772255
LOWPASS_B1 = 0.03316386416554451
LOWPASS_B2 = 0.016581932082772255
LOWPASS_A1 = -1.6041301488876343
LOWPASS_A2 = 0.6704578995704651
MAX_STEP_DELTA = 12000


@dataclass(frozen=True)
class SampleRow:
    timestamp_ms: int
    red: int
    ir: int
    filtered_ir: int
    status: str


def parse_sample_line(line: str) -> SampleRow | None:
    text = line.strip()
    if not text or text == CSV_HEADER:
        return None

    match = ROW_RE.match(text)
    if match is None:
        return None

    status = match.group("status")
    if status not in VALID_STATUSES:
        return None

    return SampleRow(
        timestamp_ms=int(match.group("timestamp_ms")),
        red=int(match.group("red")),
        ir=int(match.group("ir")),
        filtered_ir=int(match.group("filtered_ir")),
        status=status,
    )


def max_gap_ms(rows: list[SampleRow]) -> int:
    if len(rows) < 2:
        return 0

    return max(
        rows[index].timestamp_ms - rows[index - 1].timestamp_ms
        for index in range(1, len(rows))
    )


def normalize_series(values: list[int]) -> list[float]:
    if not values:
        return []

    low = min(values)
    high = max(values)
    if low == high:
        return [0.5 for _ in values]

    scale = high - low
    return [(value - low) / scale for value in values]


def _f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _lroundf(value: float) -> int:
    if value >= 0.0:
        return int(math.floor(value + 0.5))

    return int(math.ceil(value - 0.5))


def _median3(first: int, second: int, third: int) -> int:
    return sorted((first, second, third))[1]


def apply_preprocess_chain(values: list[int]) -> list[int]:
    if not values:
        return []

    outputs: list[int] = []
    initialized = False
    median_buf = [0, 0, 0]
    median_count = 0
    median_index = 0
    baseline = _f32(0.0)
    lowpass = _f32(0.0)
    x1 = _f32(0.0)
    x2 = _f32(0.0)
    y1 = _f32(0.0)
    y2 = _f32(0.0)
    last_filtered = 0

    for raw_ir in values:
        median_buf[median_index] = raw_ir
        median_index = (median_index + 1) % 3
        if median_count < 3:
            median_count += 1

        median_ir = raw_ir if median_count < 3 else _median3(*median_buf)
        median_value = _f32(float(median_ir))

        if not initialized:
            initialized = True
            baseline = median_value
            outputs.append(0)
            continue

        baseline = _f32(
            baseline
            + _f32(_f32(BASELINE_ALPHA) * _f32(median_value - baseline))
        )
        ac_value = _f32(median_value - baseline)
        lowpass = _f32(
            _f32(_f32(LOWPASS_B0) * ac_value)
            + _f32(_f32(LOWPASS_B1) * x1)
            + _f32(_f32(LOWPASS_B2) * x2)
            - _f32(_f32(LOWPASS_A1) * y1)
            - _f32(_f32(LOWPASS_A2) * y2)
        )

        x2 = x1
        x1 = ac_value
        y2 = y1
        y1 = lowpass

        filtered = _lroundf(lowpass)
        delta = filtered - last_filtered
        if delta > MAX_STEP_DELTA:
            filtered = last_filtered + MAX_STEP_DELTA
        elif delta < -MAX_STEP_DELTA:
            filtered = last_filtered - MAX_STEP_DELTA

        outputs.append(filtered)
        last_filtered = filtered

    return outputs
