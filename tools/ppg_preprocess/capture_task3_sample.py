from __future__ import annotations

import argparse
import csv
from pathlib import Path
import sys

import serial

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.ppg_preprocess.common import CSV_HEADER, SampleRow, max_gap_ms, parse_sample_line

MAX_SERIAL_LINES = 20000
MAX_EMPTY_READS = 5
MAX_ALLOWED_GAP_MS = 100


def _validate_duration_ms(duration_ms: int) -> None:
    if duration_ms <= 0:
        raise ValueError("duration_ms must be positive")


def _validate_continuity(rows: list[SampleRow]) -> None:
    if len(rows) < 2:
        return

    for prev, curr in zip(rows, rows[1:]):
        if curr.timestamp_ms <= prev.timestamp_ms:
            raise ValueError("captured sample rows must be strictly increasing")

    gap_ms = max_gap_ms(rows)
    if gap_ms > MAX_ALLOWED_GAP_MS:
        raise ValueError(
            f"captured sample rows exceed max timestamp gap of {MAX_ALLOWED_GAP_MS} ms "
            f"(max_gap_ms={gap_ms})"
        )


def capture_rows_from_lines(lines: list[str], duration_ms: int) -> list[SampleRow]:
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
            _validate_continuity(rows)
            return rows

    raise ValueError("requested duration was not fully captured")


def capture_rows_from_serial(
    port: str,
    baudrate: int,
    timeout: float,
    duration_ms: int,
) -> list[SampleRow]:
    _validate_duration_ms(duration_ms)

    rows: list[SampleRow] = []
    raw_reads = 0
    empty_reads = 0
    start_timestamp_ms: int | None = None

    with serial.Serial(port, baudrate=baudrate, timeout=timeout) as ser:
        while raw_reads < MAX_SERIAL_LINES and empty_reads < MAX_EMPTY_READS:
            raw_reads += 1
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
                _validate_continuity(rows)
                return rows

    if not rows:
        raise ValueError("no valid sample rows captured")

    raise ValueError("captured sample rows do not span requested duration")


def write_rows(output_path: Path, rows: list[SampleRow]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow(CSV_HEADER.split(","))
        for row in rows:
            writer.writerow(
                [row.timestamp_ms, row.red, row.ir, row.filtered_ir, row.status]
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=1.0)
    parser.add_argument("--duration-ms", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        rows = capture_rows_from_serial(
            args.port,
            args.baudrate,
            args.timeout,
            args.duration_ms,
        )
        output_path = args.output.expanduser().resolve()
        write_rows(output_path, rows)
    except (OSError, ValueError, serial.SerialException) as exc:
        raise SystemExit(str(exc)) from exc

    print(f"captured {len(rows)} rows to {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
