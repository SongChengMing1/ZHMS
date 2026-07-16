from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import sys

from PIL import Image, ImageDraw

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.ppg_peak_detect.common import (
    CSV_HEADERS,
    SampleRow,
    parse_sample_line,
    run_detector,
)

CANVAS_WIDTH = 960
CANVAS_HEIGHT = 420
MARGIN_LEFT = 64
MARGIN_RIGHT = 24
MARGIN_TOP = 28
MARGIN_BOTTOM = 44
BACKGROUND_COLOR = (255, 255, 255)
BORDER_COLOR = (200, 200, 200)
GRID_COLOR = (234, 234, 234)
FILTERED_COLOR = (40, 120, 220)
THRESHOLD_COLOR = (120, 120, 120)
PEAK_COLOR = (220, 60, 60)
TEXT_COLOR = (30, 30, 30)
MIN_SAMPLE_ROWS = 3

PEAK_COLUMNS = [
    "timestamp_ms",
    "filtered_ir",
    "threshold",
    "peak_detected",
    "peak_timestamp_ms",
    "ibi_ms",
    "ibi_valid",
    "signal_valid",
]


def _read_rows(input_path: Path) -> list[SampleRow]:
    rows: list[SampleRow] = []
    with input_path.open("r", encoding="utf-8") as fp:
        for line_number, line in enumerate(fp, start=1):
            text = line.strip()
            if not text:
                continue
            if text in CSV_HEADERS:
                if line_number != 1:
                    raise ValueError(
                        f"invalid sample row at line {line_number} in {input_path}: {text}"
                    )
                continue

            row = parse_sample_line(line)
            if row is None:
                raise ValueError(
                    f"invalid sample row at line {line_number} in {input_path}: {text}"
                )

            rows.append(row)

    if len(rows) < MIN_SAMPLE_ROWS:
        raise ValueError(f"need at least {MIN_SAMPLE_ROWS} sample rows in {input_path}")

    return rows


def _x_positions(rows: list[SampleRow]) -> list[int]:
    plot_width = CANVAS_WIDTH - MARGIN_LEFT - MARGIN_RIGHT
    timestamps = [row.timestamp_ms for row in rows]
    low = min(timestamps)
    high = max(timestamps)
    if low == high:
        center = MARGIN_LEFT + plot_width // 2
        return [center for _ in rows]

    span = high - low
    return [
        round(MARGIN_LEFT + ((row.timestamp_ms - low) / span) * plot_width)
        for row in rows
    ]


def _x_position_for_timestamp(rows: list[SampleRow], timestamp_ms: int) -> int:
    plot_width = CANVAS_WIDTH - MARGIN_LEFT - MARGIN_RIGHT
    timestamps = [row.timestamp_ms for row in rows]
    low = min(timestamps)
    high = max(timestamps)
    if low == high:
        return MARGIN_LEFT + plot_width // 2

    span = high - low
    return round(MARGIN_LEFT + ((timestamp_ms - low) / span) * plot_width)


def _scale_y(value: int, low: int, high: int) -> int:
    plot_height = CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM
    if low == high:
        return MARGIN_TOP + plot_height // 2

    return round(MARGIN_TOP + (1.0 - ((value - low) / (high - low))) * plot_height)


def _write_peaks_csv(
    output_path: Path, rows: list[SampleRow], outputs
) -> None:
    with output_path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=PEAK_COLUMNS, lineterminator="\n")
        writer.writeheader()
        for row, output in zip(rows, outputs):
            if not output.peak_detected:
                continue
            writer.writerow(
                {
                    "timestamp_ms": output.peak_timestamp_ms,
                    "filtered_ir": output.candidate_value,
                    "threshold": output.threshold,
                    "peak_detected": str(output.peak_detected).lower(),
                    "peak_timestamp_ms": output.peak_timestamp_ms,
                    "ibi_ms": output.ibi_ms,
                    "ibi_valid": str(output.ibi_valid).lower(),
                    "signal_valid": str(output.signal_valid).lower(),
                }
            )


def _write_summary_json(output_path: Path, outputs) -> None:
    summary = {
        "valid_peak_count": sum(1 for output in outputs if output.peak_detected),
        "valid_ibi_count": sum(1 for output in outputs if output.ibi_valid),
        "signal_valid": outputs[-1].signal_valid if outputs else False,
    }
    output_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


def _remove_stale_outputs(output_dir: Path, stem: str) -> None:
    for suffix in ("-peaks.csv", "-summary.json", "-overlay.png"):
        target_path = output_dir / f"{stem}{suffix}"
        target_path.unlink(missing_ok=True)


def _draw_overlay(
    output_path: Path, rows: list[SampleRow], outputs, input_name: str
) -> None:
    image = Image.new("RGB", (CANVAS_WIDTH, CANVAS_HEIGHT), BACKGROUND_COLOR)
    draw = ImageDraw.Draw(image)

    draw.rectangle(
        [MARGIN_LEFT, MARGIN_TOP, CANVAS_WIDTH - MARGIN_RIGHT, CANVAS_HEIGHT - MARGIN_BOTTOM],
        outline=BORDER_COLOR,
        width=1,
    )

    for fraction in (0.25, 0.5, 0.75):
        y = round(MARGIN_TOP + fraction * (CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM))
        draw.line(
            [(MARGIN_LEFT, y), (CANVAS_WIDTH - MARGIN_RIGHT, y)],
            fill=GRID_COLOR,
            width=1,
        )

    filtered_values = [row.filtered_ir for row in rows]
    threshold_values = [output.threshold for output in outputs]
    value_low = min(filtered_values + threshold_values)
    value_high = max(filtered_values + threshold_values)
    x_positions = _x_positions(rows)
    filtered_points = [
        (x_positions[index], _scale_y(row.filtered_ir, value_low, value_high))
        for index, row in enumerate(rows)
    ]
    threshold_points = [
        (x_positions[index], _scale_y(output.threshold, value_low, value_high))
        for index, output in enumerate(outputs)
    ]

    if len(filtered_points) > 1:
        draw.line(filtered_points, fill=FILTERED_COLOR, width=3)
        draw.line(threshold_points, fill=THRESHOLD_COLOR, width=2)

    for index, output in enumerate(outputs):
        if not output.peak_detected:
            continue
        x = _x_position_for_timestamp(rows, output.peak_timestamp_ms)
        y = _scale_y(output.candidate_value, value_low, value_high)
        draw.ellipse([(x - 5, y - 5), (x + 5, y + 5)], outline=PEAK_COLOR, width=2)
        draw.line([(x, y - 9), (x, y + 9)], fill=PEAK_COLOR, width=1)

    draw.text((MARGIN_LEFT, 6), f"{input_name} peaks", fill=TEXT_COLOR)
    image.save(output_path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()

    try:
        input_path = args.input.expanduser().resolve()
        output_dir = args.output_dir.expanduser().resolve()
        stem = input_path.stem
        output_dir.mkdir(parents=True, exist_ok=True)
        _remove_stale_outputs(output_dir, stem)
        rows = _read_rows(input_path)
        outputs = run_detector(rows)

        _write_peaks_csv(output_dir / f"{stem}-peaks.csv", rows, outputs)
        _write_summary_json(output_dir / f"{stem}-summary.json", outputs)
        _draw_overlay(output_dir / f"{stem}-overlay.png", rows, outputs, stem)

        valid_ibi_count = sum(1 for output in outputs if output.ibi_valid)
        print(f"analyzed {stem}: valid_ibis={valid_ibi_count}")
    except (OSError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
