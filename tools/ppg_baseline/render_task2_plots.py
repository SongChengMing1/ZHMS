from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

from PIL import Image, ImageDraw

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.ppg_baseline.common import CSV_HEADER, SampleRow, VALID_STATUSES, max_gap_ms, normalize_series

CANVAS_WIDTH = 960
CANVAS_HEIGHT = 360
MARGIN_LEFT = 56
MARGIN_RIGHT = 24
MARGIN_TOP = 24
MARGIN_BOTTOM = 48
GRID_COLOR = (225, 225, 225)
AXIS_COLOR = (160, 160, 160)
RAW_RED_COLOR = (220, 60, 60)
RAW_IR_COLOR = (45, 120, 220)
NORM_RED_COLOR = (180, 35, 35)
NORM_IR_COLOR = (35, 90, 180)
BACKGROUND_COLOR = (255, 255, 255)
PLOT_BORDER_COLOR = (200, 200, 200)
LINE_WIDTH = 3


def _read_rows(input_path: Path) -> list[SampleRow]:
    rows: list[SampleRow] = []
    with input_path.open("r", encoding="utf-8", newline="") as fp:
        reader = csv.reader(fp)
        for line_number, row in enumerate(reader, start=1):
            if line_number == 1:
                if row != CSV_HEADER.split(","):
                    raise ValueError(f"invalid CSV header in {input_path}: {','.join(row)}")
                continue

            if len(row) != 4 or any(field != field.strip() for field in row):
                raise ValueError(f"invalid sample row at line {line_number} in {input_path}: {','.join(row)}")

            timestamp_text, red_text, ir_text, status = row
            if status not in VALID_STATUSES:
                raise ValueError(f"invalid sample row at line {line_number} in {input_path}: {','.join(row)}")

            rows.append(
                SampleRow(
                    timestamp_ms=int(timestamp_text),
                    red=int(red_text),
                    ir=int(ir_text),
                    status=status,
                )
            )

    if not rows:
        raise ValueError(f"no valid sample rows found in {input_path}")

    return rows


def _normalize_against_combined_range(rows: list[SampleRow]) -> tuple[list[float], list[float]]:
    combined_values = [row.red for row in rows] + [row.ir for row in rows]
    low = min(combined_values)
    high = max(combined_values)
    if low == high:
        return [0.5 for _ in rows], [0.5 for _ in rows]

    scale = high - low
    red_values = [(row.red - low) / scale for row in rows]
    ir_values = [(row.ir - low) / scale for row in rows]
    return red_values, ir_values


def _plot_points(values: list[float]) -> list[tuple[int, int]]:
    if not values:
        return []

    plot_width = CANVAS_WIDTH - MARGIN_LEFT - MARGIN_RIGHT
    plot_height = CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM
    if len(values) == 1:
        x_positions = [MARGIN_LEFT + plot_width // 2]
    else:
        x_positions = [
            round(MARGIN_LEFT + index * plot_width / (len(values) - 1))
            for index in range(len(values))
        ]

    return [
        (
            x_positions[index],
            round(MARGIN_TOP + (1.0 - value) * plot_height),
        )
        for index, value in enumerate(values)
    ]


def _draw_base(draw: ImageDraw.ImageDraw) -> None:
    plot_left = MARGIN_LEFT
    plot_top = MARGIN_TOP
    plot_right = CANVAS_WIDTH - MARGIN_RIGHT
    plot_bottom = CANVAS_HEIGHT - MARGIN_BOTTOM

    draw.rectangle([plot_left, plot_top, plot_right, plot_bottom], outline=PLOT_BORDER_COLOR, width=1)
    for fraction in (0.25, 0.5, 0.75):
        y = round(plot_top + fraction * (plot_bottom - plot_top))
        draw.line([(plot_left, y), (plot_right, y)], fill=GRID_COLOR, width=1)
    draw.line([(plot_left, plot_bottom), (plot_right, plot_bottom)], fill=AXIS_COLOR, width=1)
    draw.line([(plot_left, plot_top), (plot_left, plot_bottom)], fill=AXIS_COLOR, width=1)


def _draw_overlay_plot(
    output_path: Path,
    title: str,
    red_values: list[float],
    ir_values: list[float],
    red_color: tuple[int, int, int],
    ir_color: tuple[int, int, int],
) -> None:
    image = Image.new("RGB", (CANVAS_WIDTH, CANVAS_HEIGHT), BACKGROUND_COLOR)
    draw = ImageDraw.Draw(image)

    _draw_base(draw)
    draw.text((MARGIN_LEFT, 6), title, fill=(30, 30, 30))

    red_points = _plot_points(red_values)
    ir_points = _plot_points(ir_values)
    if len(red_points) >= 2:
        draw.line(red_points, fill=red_color, width=LINE_WIDTH)
    if len(ir_points) >= 2:
        draw.line(ir_points, fill=ir_color, width=LINE_WIDTH)

    image.save(output_path)


def _render_task2_plots(input_path: Path, rows: list[SampleRow]) -> tuple[Path, Path]:
    raw_values_red, raw_values_ir = _normalize_against_combined_range(rows)
    norm_values_red = normalize_series([row.red for row in rows])
    norm_values_ir = normalize_series([row.ir for row in rows])

    raw_path = input_path.with_name(f"{input_path.stem}-raw.png")
    norm_path = input_path.with_name(f"{input_path.stem}-norm.png")

    _draw_overlay_plot(
        raw_path,
        "Task 2 raw overlay",
        raw_values_red,
        raw_values_ir,
        RAW_RED_COLOR,
        RAW_IR_COLOR,
    )
    _draw_overlay_plot(
        norm_path,
        "Task 2 normalized overlay",
        norm_values_red,
        norm_values_ir,
        NORM_RED_COLOR,
        NORM_IR_COLOR,
    )

    return raw_path, norm_path


def render_task2_plots(input_path: Path) -> tuple[Path, Path]:
    rows = _read_rows(input_path)
    return _render_task2_plots(input_path, rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", dest="input_csv", type=Path, required=True)
    args = parser.parse_args()

    input_path = args.input_csv.expanduser().resolve()
    try:
        rows = _read_rows(input_path)
        raw_path, norm_path = _render_task2_plots(input_path, rows)
    except (OSError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc

    gap_ms = max_gap_ms(rows)
    print(f"rendered {raw_path.name} and {norm_path.name} (max_gap_ms={gap_ms})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
