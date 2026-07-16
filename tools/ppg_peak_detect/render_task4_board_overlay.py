from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import json
from pathlib import Path
import sys

from PIL import Image, ImageDraw

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

RAW_ROW_COLUMN_COUNT = 12
OUTPUT_IMAGE_SIZE = (2000, 1000)
PLOT_MARGINS = (90, 30, 40, 120)
BACKGROUND_COLOR = (255, 255, 255)
GRID_COLOR = (210, 210, 210)
AXIS_COLOR = (0, 0, 0)
FILTERED_IR_COLOR = (31, 119, 201)
THRESHOLD_COLOR = (255, 127, 14)
BPM_COLOR = (106, 90, 205)
PEAK_COLOR = (214, 39, 40)
VALID_IBI_COLOR = (44, 160, 44)
STATE_BACKGROUND_COLORS = {
    0: (230, 230, 230),
    1: (255, 242, 191),
    2: (220, 236, 223),
}
Y_TICKS = (-1000, -500, 0, 500, 1000)
CLEAN_CSV_HEADER = [
    "timestamp_ms",
    "ir",
    "filtered_ir",
    "threshold",
    "peak_flag",
    "ibi_ms",
    "ibi_valid",
    "signal_valid",
    "finger_state",
    "bpm",
    "confirmed_peak_timestamp_ms",
    "status",
]


@dataclass(frozen=True)
class BoardOverlayRow:
    timestamp_ms: int
    ir: int
    filtered_ir: int
    threshold: int
    peak_flag: int
    ibi_ms: int
    ibi_valid: int
    signal_valid: int
    finger_state: int
    bpm: int | None
    confirmed_peak_timestamp_ms: int
    status: str


def _parse_row(text: str) -> BoardOverlayRow | None:
    stripped = text.strip()
    if not stripped or stripped == ",".join(CLEAN_CSV_HEADER):
        return None

    parts = stripped.split(",")
    if len(parts) != RAW_ROW_COLUMN_COUNT or parts[-1] != "ok":
        return None

    (
        timestamp_ms,
        ir,
        filtered_ir,
        threshold,
        peak_flag,
        ibi_ms,
        ibi_valid,
        signal_valid,
        finger_state,
        bpm,
        confirmed_peak_timestamp_ms,
        status,
    ) = parts
    try:
        return BoardOverlayRow(
            timestamp_ms=int(timestamp_ms),
            ir=int(ir),
            filtered_ir=int(filtered_ir),
            threshold=int(threshold),
            peak_flag=int(peak_flag),
            ibi_ms=int(ibi_ms),
            ibi_valid=int(ibi_valid),
            signal_valid=int(signal_valid),
            finger_state=int(finger_state),
            bpm=None if bpm == "na" else int(bpm),
            confirmed_peak_timestamp_ms=int(confirmed_peak_timestamp_ms or "0"),
            status=status,
        )
    except ValueError:
        return None


def _read_rows(input_path: Path) -> list[BoardOverlayRow]:
    rows: list[BoardOverlayRow] = []
    with input_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            row = _parse_row(line)
            if row is not None:
                rows.append(row)

    if not rows:
        raise ValueError(f"no valid board rows found in {input_path}")

    return rows


def _output_stem(input_path: Path) -> str:
    stem = input_path.stem
    if stem.endswith("-clean"):
        return stem
    return f"{stem}-clean"


def _write_clean_csv(output_path: Path, rows: list[BoardOverlayRow]) -> None:
    with output_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(CLEAN_CSV_HEADER)
        for row in rows:
            writer.writerow(
                [
                    row.timestamp_ms,
                    row.ir,
                    row.filtered_ir,
                    row.threshold,
                    row.peak_flag,
                    row.ibi_ms,
                    row.ibi_valid,
                    row.signal_valid,
                    row.finger_state,
                    "na" if row.bpm is None else row.bpm,
                    row.confirmed_peak_timestamp_ms,
                    row.status,
                ]
            )


def _build_marker_row_indexes(rows: list[BoardOverlayRow]) -> tuple[list[int], list[int]]:
    timestamp_to_index = {row.timestamp_ms: index for index, row in enumerate(rows)}
    peak_rows: list[int] = []
    ibi_rows: list[int] = []

    for index, row in enumerate(rows):
        if row.peak_flag != 1:
            continue

        target_index = timestamp_to_index.get(row.confirmed_peak_timestamp_ms, index)
        peak_rows.append(target_index)
        if row.ibi_valid == 1:
            ibi_rows.append(target_index)

    return peak_rows, ibi_rows


def _build_summary(rows: list[BoardOverlayRow], y_min: int, y_max: int) -> dict[str, object]:
    valid_bpms = [row.bpm for row in rows if row.bpm is not None]
    finger_states = sorted({row.finger_state for row in rows})
    return {
        "row_count": len(rows),
        "peak_count": sum(row.peak_flag for row in rows),
        "valid_ibi_count": sum(row.ibi_valid for row in rows),
        "finger_state_counts": {
            str(state): sum(1 for row in rows if row.finger_state == state)
            for state in finger_states
        },
        "bpm_valid_sample_count": len(valid_bpms),
        "bpm_min": min(valid_bpms) if valid_bpms else None,
        "bpm_max": max(valid_bpms) if valid_bpms else None,
        "max_threshold": max(row.threshold for row in rows),
        "plot_y_range": [y_min, y_max],
    }


def _write_summary(output_path: Path, summary: dict[str, object]) -> None:
    output_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


def _x_for_index(index: int, row_count: int, plot_left: int, plot_width: int) -> int:
    if row_count <= 1:
        return plot_left
    return plot_left + round((index / (row_count - 1)) * plot_width)


def _y_for_value(value: float, y_min: int, y_max: int, plot_top: int, plot_height: int) -> int:
    clipped = max(y_min, min(y_max, value))
    fraction = (clipped - y_min) / (y_max - y_min)
    return plot_top + round((1.0 - fraction) * plot_height)


def _draw_overlay(
    output_path: Path,
    rows: list[BoardOverlayRow],
    summary: dict[str, object],
    input_name: str,
    y_min: int,
    y_max: int,
) -> None:
    width, height = OUTPUT_IMAGE_SIZE
    margin_left, margin_right, margin_top, margin_bottom = PLOT_MARGINS
    plot_width = width - margin_left - margin_right
    plot_height = height - margin_top - margin_bottom

    image = Image.new("RGB", OUTPUT_IMAGE_SIZE, BACKGROUND_COLOR)
    draw = ImageDraw.Draw(image)

    start = 0
    for index in range(1, len(rows) + 1):
        if index == len(rows) or rows[index].finger_state != rows[start].finger_state:
            x0 = _x_for_index(start, len(rows), margin_left, plot_width)
            x1 = _x_for_index(index - 1, len(rows), margin_left, plot_width)
            draw.rectangle(
                [x0, margin_top, x1, margin_top + plot_height],
                fill=STATE_BACKGROUND_COLORS.get(rows[start].finger_state, BACKGROUND_COLOR),
            )
            start = index

    for tick in Y_TICKS:
        if tick < y_min or tick > y_max:
            continue
        y = _y_for_value(tick, y_min, y_max, margin_top, plot_height)
        draw.line([(margin_left, y), (width - margin_right, y)], fill=GRID_COLOR, width=1)
        draw.text((10, y - 8), str(tick), fill=AXIS_COLOR)

    draw.line(
        [(margin_left, margin_top), (margin_left, margin_top + plot_height)],
        fill=AXIS_COLOR,
        width=2,
    )
    draw.line(
        [(margin_left, margin_top + plot_height), (width - margin_right, margin_top + plot_height)],
        fill=AXIS_COLOR,
        width=2,
    )

    filtered_points = [
        (
            _x_for_index(index, len(rows), margin_left, plot_width),
            _y_for_value(row.filtered_ir, y_min, y_max, margin_top, plot_height),
        )
        for index, row in enumerate(rows)
    ]
    if len(filtered_points) > 1:
        draw.line(filtered_points, fill=FILTERED_IR_COLOR, width=1)

    for index, row in enumerate(rows[:-1]):
        x0 = _x_for_index(index, len(rows), margin_left, plot_width)
        x1 = _x_for_index(index + 1, len(rows), margin_left, plot_width)
        y0 = _y_for_value(row.threshold, y_min, y_max, margin_top, plot_height)
        y1 = _y_for_value(rows[index + 1].threshold, y_min, y_max, margin_top, plot_height)
        draw.line([(x0, y0), (x1, y0)], fill=THRESHOLD_COLOR, width=1)
        draw.line([(x1, y0), (x1, y1)], fill=THRESHOLD_COLOR, width=1)

    last_bpm_point: tuple[int, int] | None = None
    for index, row in enumerate(rows):
        if row.bpm is None:
            last_bpm_point = None
            continue
        point = (
            _x_for_index(index, len(rows), margin_left, plot_width),
            _y_for_value(row.bpm, y_min, y_max, margin_top, plot_height),
        )
        if last_bpm_point is not None:
            draw.line([last_bpm_point, point], fill=BPM_COLOR, width=1)
        last_bpm_point = point

    peak_rows, ibi_rows = _build_marker_row_indexes(rows)

    for row_index in peak_rows:
        row = rows[row_index]
        x = _x_for_index(row_index, len(rows), margin_left, plot_width)
        y = _y_for_value(row.filtered_ir, y_min, y_max, margin_top, plot_height)
        draw.ellipse([x - 3, y - 3, x + 3, y + 3], fill=PEAK_COLOR, outline=PEAK_COLOR)

    for row_index in ibi_rows:
        row = rows[row_index]
        x = _x_for_index(row_index, len(rows), margin_left, plot_width)
        y = _y_for_value(row.filtered_ir, y_min, y_max, margin_top, plot_height)
        draw.ellipse(
            [x - 5, y - 5, x + 5, y + 5],
            fill=VALID_IBI_COLOR,
            outline=VALID_IBI_COLOR,
        )

    caption_lines = [
        f"{input_name} overlay",
        (
            f"rows={summary['row_count']} peaks={summary['peak_count']} "
            f"valid_ibis={summary['valid_ibi_count']}"
        ),
        f"finger_state_counts={summary['finger_state_counts']}",
        (
            f"bpm_valid_samples={summary['bpm_valid_sample_count']} "
            f"bpm_range=[{summary['bpm_min']},{summary['bpm_max']}]"
            if summary["bpm_min"] is not None
            else "bpm_valid_samples=0 bpm_range=[na,na]"
        ),
        "gray=no_finger yellow=warmup green=active",
        "blue=filtered_ir orange=threshold purple=bpm red=peak green-dot=ibi_valid",
        f"plot_y_range=[{y_min},{y_max}]",
    ]
    text_y = height - 100
    for line in caption_lines:
        draw.text((margin_left, text_y), line, fill=AXIS_COLOR)
        text_y += 14

    image.save(output_path)


def _remove_stale_outputs(output_dir: Path, stem: str) -> None:
    for suffix in (".csv", "-summary.json", "-overlay.png"):
        (output_dir / f"{stem}{suffix}").unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--y-min", default=-1000, type=int)
    parser.add_argument("--y-max", default=1000, type=int)
    args = parser.parse_args()

    try:
        if args.y_min >= args.y_max:
            raise ValueError("--y-min must be smaller than --y-max")

        input_path = args.input.expanduser().resolve()
        output_dir = args.output_dir.expanduser().resolve()
        output_dir.mkdir(parents=True, exist_ok=True)

        rows = _read_rows(input_path)
        stem = _output_stem(input_path)
        _remove_stale_outputs(output_dir, stem)

        clean_path = output_dir / f"{stem}.csv"
        summary_path = output_dir / f"{stem}-summary.json"
        overlay_path = output_dir / f"{stem}-overlay.png"

        _write_clean_csv(clean_path, rows)
        summary = _build_summary(rows, args.y_min, args.y_max)
        _write_summary(summary_path, summary)
        _draw_overlay(overlay_path, rows, summary, stem, args.y_min, args.y_max)

        print(
            f"rendered {stem}: rows={summary['row_count']} "
            f"peaks={summary['peak_count']} valid_ibis={summary['valid_ibi_count']}"
        )
    except (OSError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
