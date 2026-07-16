from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
from typing import Iterable

import serial

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.ppg_baseline.common import CSV_HEADER, SampleRow, max_gap_ms, parse_sample_line

MAX_SERIAL_LINES = 20000
MAX_EMPTY_READS = 5


def _validate_duration_ms(duration_ms: int) -> None:
    if duration_ms <= 0:
        raise ValueError("duration_ms must be positive")


def capture_rows_from_iterable(lines: Iterable[str], duration_ms: int) -> list[SampleRow]:
    _validate_duration_ms(duration_ms)
    rows: list[SampleRow] = []
    start_timestamp_ms: int | None = None

    for line in lines:
        row = parse_sample_line(line)
        if row is None:
            continue

        if start_timestamp_ms is None:
            start_timestamp_ms = row.timestamp_ms

        rows.append(row)
        if row.timestamp_ms - start_timestamp_ms >= duration_ms:
            break

    if not rows:
        raise ValueError("no valid sample rows captured")

    if rows[-1].timestamp_ms - rows[0].timestamp_ms < duration_ms:
        raise ValueError("captured sample rows do not span requested duration")

    return rows


def capture_rows_from_lines(lines: list[str], duration_ms: int) -> list[SampleRow]:
    return capture_rows_from_iterable(lines, duration_ms)


def capture_rows_from_serial(port: str, baudrate: int, timeout: float, duration_ms: int) -> list[SampleRow]:
    _validate_duration_ms(duration_ms)
    rows: list[SampleRow] = []
    start_timestamp_ms: int | None = None
    empty_reads = 0

    with serial.Serial(port, baudrate=baudrate, timeout=timeout) as ser:
        while len(rows) < MAX_SERIAL_LINES and empty_reads < MAX_EMPTY_READS:
            raw = ser.readline()
            if not raw:
                empty_reads += 1
                continue

            empty_reads = 0
            row = parse_sample_line(raw.decode("utf-8", errors="ignore"))
            if row is None:
                continue

            if start_timestamp_ms is None:
                start_timestamp_ms = row.timestamp_ms

            rows.append(row)
            if row.timestamp_ms - start_timestamp_ms >= duration_ms:
                break

    if not rows:
        raise ValueError("no valid sample rows captured")

    if rows[-1].timestamp_ms - rows[0].timestamp_ms < duration_ms:
        raise ValueError("captured sample rows do not span requested duration")

    return rows


def write_rows(path: Path, rows: list[SampleRow]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fp:
        writer = csv.writer(fp)
        writer.writerow(CSV_HEADER.split(","))
        for row in rows:
            writer.writerow([row.timestamp_ms, row.red, row.ir, row.status])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=1.0)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--duration-ms", type=int, default=30000)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        rows = capture_rows_from_serial(args.port, args.baudrate, args.timeout, args.duration_ms)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc

    gap_ms = max_gap_ms(rows)
    if gap_ms > 100:
        raise SystemExit(f"capture rejected: max timestamp gap {gap_ms} ms exceeds 100 ms limit")

    write_rows(args.output, rows)
    print(f"scene={args.scene}, status=ok, max_gap_ms={gap_ms}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
