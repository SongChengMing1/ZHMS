from __future__ import annotations

from dataclasses import dataclass
import re

CSV_HEADER = "timestamp_ms,red,ir,status"
VALID_STATUSES = {"ok"}
ROW_RE = re.compile(
    r"^(?P<timestamp_ms>\d+),(?P<red>-?\d+),(?P<ir>-?\d+),(?P<status>ok)$"
)


@dataclass(frozen=True)
class SampleRow:
    timestamp_ms: int
    red: int
    ir: int
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

