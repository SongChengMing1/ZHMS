from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import sys

from PIL import Image, ImageDraw

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.ppg_preprocess.common import CSV_HEADER, SampleRow, max_gap_ms, normalize_series

CANVAS_WIDTH = 960
CANVAS_HEIGHT = 520
MIN_SAMPLE_ROWS = 2
MARGIN_LEFT = 56
MARGIN_RIGHT = 24
MARGIN_TOP = 24
MARGIN_BOTTOM = 48
PANEL_GAP = 36
GRID_COLOR = (225, 225, 225)
AXIS_COLOR = (160, 160, 160)
RAW_RED_COLOR = (220, 60, 60)
RAW_IR_COLOR = (45, 120, 220)
FILTERED_IR_COLOR = (30, 150, 80)
BACKGROUND_COLOR = (255, 255, 255)
PLOT_BORDER_COLOR = (200, 200, 200)
LINE_WIDTH = 3
HTML_VIEWER_WIDTH = CANVAS_WIDTH
HTML_VIEWER_HEIGHT = CANVAS_HEIGHT + 88


def _parse_sample_row(row: list[str], line_number: int, input_path: Path) -> SampleRow:
    if len(row) != 5 or any(field != field.strip() for field in row):
        raise ValueError(
            f"invalid sample row at line {line_number} in {input_path}: {','.join(row)}"
        )

    timestamp_text, red_text, ir_text, filtered_text, status = row
    if status != "ok":
        raise ValueError(
            f"invalid sample row at line {line_number} in {input_path}: {','.join(row)}"
        )

    return SampleRow(
        timestamp_ms=int(timestamp_text),
        red=int(red_text),
        ir=int(ir_text),
        filtered_ir=int(filtered_text),
        status=status,
    )


def _read_rows(input_path: Path) -> list[SampleRow]:
    rows: list[SampleRow] = []
    with input_path.open("r", encoding="utf-8", newline="") as fp:
        reader = csv.reader(fp)
        for line_number, row in enumerate(reader, start=1):
            if line_number == 1:
                if row != CSV_HEADER.split(","):
                    raise ValueError(
                        f"invalid CSV header in {input_path}: {','.join(row)}"
                    )
                continue

            rows.append(_parse_sample_row(row, line_number, input_path))

    if not rows:
        raise ValueError(f"no valid sample rows found in {input_path}")
    if len(rows) < MIN_SAMPLE_ROWS:
        raise ValueError(
            f"need at least {MIN_SAMPLE_ROWS} sample rows in {input_path}"
        )

    return rows


def _normalize_against_combined_range(
    first: list[int],
    second: list[int],
) -> tuple[list[float], list[float]]:
    combined = first + second
    low = min(combined)
    high = max(combined)
    if low == high:
        return [0.5 for _ in first], [0.5 for _ in second]

    scale = high - low
    return (
        [(value - low) / scale for value in first],
        [(value - low) / scale for value in second],
    )


def _timestamp_bounds(rows: list[SampleRow]) -> tuple[int, int]:
    if len(rows) < MIN_SAMPLE_ROWS:
        raise ValueError(
            f"need at least {MIN_SAMPLE_ROWS} sample rows to render a plot"
        )

    timestamps = [row.timestamp_ms for row in rows]
    low_timestamp = min(timestamps)
    high_timestamp = max(timestamps)
    if high_timestamp == low_timestamp:
        raise ValueError(
            f"sample timestamps must span a positive range in {rows!r}"
        )

    return low_timestamp, high_timestamp


def _validate_strictly_increasing_timestamps(rows: list[SampleRow]) -> None:
    for prev, curr in zip(rows, rows[1:]):
        if curr.timestamp_ms <= prev.timestamp_ms:
            raise ValueError("sample timestamps must be strictly increasing")


def _x_position_for_timestamp(rows: list[SampleRow], timestamp_ms: int) -> int:
    if len(rows) < MIN_SAMPLE_ROWS:
        raise ValueError(
            f"need at least {MIN_SAMPLE_ROWS} sample rows to render a plot"
        )

    low_timestamp, high_timestamp = _timestamp_bounds(rows)
    plot_width = CANVAS_WIDTH - MARGIN_LEFT - MARGIN_RIGHT
    span = high_timestamp - low_timestamp
    return round(MARGIN_LEFT + ((timestamp_ms - low_timestamp) / span) * plot_width)


def _x_positions_for_rows(rows: list[SampleRow]) -> list[int]:
    if not rows:
        return []
    return [_x_position_for_timestamp(rows, row.timestamp_ms) for row in rows]


def _time_tick_labels(rows: list[SampleRow]) -> list[tuple[int, str]]:
    if len(rows) < MIN_SAMPLE_ROWS:
        raise ValueError(
            f"need at least {MIN_SAMPLE_ROWS} sample rows to render a plot"
        )

    first_timestamp = rows[0].timestamp_ms
    labels: list[tuple[int, str]] = []
    last_label: str | None = None
    for row in rows:
        label = f"{(row.timestamp_ms - first_timestamp) // 1000} s"
        if label == last_label:
            continue
        labels.append((row.timestamp_ms, label))
        last_label = label

    return labels


def _plot_points_for_rows(
    rows: list[SampleRow],
    values: list[float],
    plot_top: int,
    plot_bottom: int,
) -> list[tuple[int, int]]:
    if not rows or not values:
        return []
    if len(rows) != len(values):
        raise ValueError("row count must match value count")
    if len(rows) < MIN_SAMPLE_ROWS:
        raise ValueError(
            f"need at least {MIN_SAMPLE_ROWS} sample rows to render a plot"
        )

    plot_height = plot_bottom - plot_top
    x_positions = _x_positions_for_rows(rows)

    return [
        (
            x_positions[index],
            round(plot_top + (1.0 - value) * plot_height),
        )
        for index, value in enumerate(values)
    ]


def _draw_panel_base(
    draw: ImageDraw.ImageDraw,
    plot_top: int,
    plot_bottom: int,
    label: str,
    color: tuple[int, int, int],
) -> None:
    plot_left = MARGIN_LEFT
    plot_right = CANVAS_WIDTH - MARGIN_RIGHT

    draw.rectangle(
        [plot_left, plot_top, plot_right, plot_bottom],
        outline=PLOT_BORDER_COLOR,
        width=1,
    )
    for fraction in (0.25, 0.5, 0.75):
        y = round(plot_top + fraction * (plot_bottom - plot_top))
        draw.line([(plot_left, y), (plot_right, y)], fill=GRID_COLOR, width=1)
    draw.line(
        [(plot_left, plot_bottom), (plot_right, plot_bottom)],
        fill=AXIS_COLOR,
        width=1,
    )
    draw.line(
        [(plot_left, plot_top), (plot_left, plot_bottom)],
        fill=AXIS_COLOR,
        width=1,
    )
    draw.text((plot_left, plot_top - 18), label, fill=color)


def _draw_time_axis(draw: ImageDraw.ImageDraw, rows: list[SampleRow]) -> None:
    tick_labels = _time_tick_labels(rows)
    if not tick_labels:
        return

    axis_top = CANVAS_HEIGHT - MARGIN_BOTTOM
    tick_bottom = axis_top + 8
    label_y = tick_bottom + 2

    for timestamp_ms, label in tick_labels:
        x_position = _x_position_for_timestamp(rows, timestamp_ms)
        draw.line(
            [(x_position, axis_top), (x_position, tick_bottom)],
            fill=AXIS_COLOR,
            width=1,
        )
        draw.text((x_position - 10, label_y), label, fill=AXIS_COLOR)


def _draw_base(draw: ImageDraw.ImageDraw) -> None:
    _draw_panel_base(
        draw,
        MARGIN_TOP,
        CANVAS_HEIGHT - MARGIN_BOTTOM,
        "Task 3 raw Red/IR overlay",
        (30, 30, 30),
    )


def _draw_overlay_plot(
    output_path: Path,
    rows: list[SampleRow],
    first_values: list[float],
    second_values: list[float],
    first_color: tuple[int, int, int],
    second_color: tuple[int, int, int],
) -> None:
    image = Image.new("RGB", (CANVAS_WIDTH, CANVAS_HEIGHT), BACKGROUND_COLOR)
    draw = ImageDraw.Draw(image)

    _draw_base(draw)

    first_points = _plot_points_for_rows(
        rows, first_values, MARGIN_TOP, CANVAS_HEIGHT - MARGIN_BOTTOM
    )
    second_points = _plot_points_for_rows(
        rows, second_values, MARGIN_TOP, CANVAS_HEIGHT - MARGIN_BOTTOM
    )
    if len(first_points) >= 2:
        draw.line(first_points, fill=first_color, width=LINE_WIDTH)
    if len(second_points) >= 2:
        draw.line(second_points, fill=second_color, width=LINE_WIDTH)

    image.save(output_path)


def _draw_stacked_compare_plot(
    output_path: Path,
    rows: list[SampleRow],
    raw_ir_values: list[float],
    filtered_ir_values: list[float],
) -> None:
    image = Image.new("RGB", (CANVAS_WIDTH, CANVAS_HEIGHT), BACKGROUND_COLOR)
    draw = ImageDraw.Draw(image)

    total_plot_height = CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM - PANEL_GAP
    panel_height = total_plot_height // 2
    top_plot_top = MARGIN_TOP
    top_plot_bottom = top_plot_top + panel_height
    bottom_plot_top = top_plot_bottom + PANEL_GAP
    bottom_plot_bottom = bottom_plot_top + panel_height

    _draw_panel_base(draw, top_plot_top, top_plot_bottom, "Raw IR", RAW_IR_COLOR)
    _draw_panel_base(
        draw,
        bottom_plot_top,
        bottom_plot_bottom,
        "Filtered IR",
        FILTERED_IR_COLOR,
    )

    raw_points = _plot_points_for_rows(
        rows,
        raw_ir_values,
        top_plot_top,
        top_plot_bottom,
    )
    filtered_points = _plot_points_for_rows(
        rows,
        filtered_ir_values,
        bottom_plot_top,
        bottom_plot_bottom,
    )
    draw.line(raw_points, fill=RAW_IR_COLOR, width=LINE_WIDTH)
    draw.line(filtered_points, fill=FILTERED_IR_COLOR, width=LINE_WIDTH)
    _draw_time_axis(draw, rows)

    image.save(output_path)


def _render_task3_compare_html(
    output_path: Path,
    rows: list[SampleRow],
    raw_ir_values: list[int],
    filtered_ir_values: list[int],
) -> None:
    payload = {
        "timestamps": [row.timestamp_ms for row in rows],
        "rawIr": raw_ir_values,
        "filteredIr": filtered_ir_values,
        "plot": {
            "width": CANVAS_WIDTH,
            "height": CANVAS_HEIGHT,
            "marginLeft": MARGIN_LEFT,
            "marginRight": MARGIN_RIGHT,
            "marginTop": MARGIN_TOP,
            "marginBottom": MARGIN_BOTTOM,
            "panelGap": PANEL_GAP,
        },
    }
    html = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{output_path.stem}</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #f7f7f7;
      --panel-bg: #ffffff;
      --border: #d8d8d8;
      --grid: #ececec;
      --axis: #9b9b9b;
      --raw: #2d78dc;
      --filtered: #1e9650;
      --text: #222222;
      --muted: #666666;
    }}
    body {{
      margin: 0;
      padding: 20px;
      background: linear-gradient(180deg, #fdfdfd 0%, var(--bg) 100%);
      color: var(--text);
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }}
    .viewer {{
      max-width: {CANVAS_WIDTH}px;
      margin: 0 auto;
    }}
    .header {{
      display: flex;
      flex-wrap: wrap;
      justify-content: space-between;
      gap: 8px 16px;
      margin-bottom: 10px;
      font-size: 14px;
    }}
    .hint {{
      color: var(--muted);
    }}
    #status {{
      font-variant-numeric: tabular-nums;
    }}
    svg {{
      display: block;
      width: 100%;
      height: auto;
      background: var(--panel-bg);
      border: 1px solid var(--border);
      border-radius: 10px;
      box-shadow: 0 8px 24px rgba(0, 0, 0, 0.04);
      touch-action: none;
      user-select: none;
    }}
    .panel-label {{
      font-size: 15px;
      font-weight: 600;
    }}
    .panel-label.raw {{
      fill: var(--raw);
    }}
    .panel-label.filtered {{
      fill: var(--filtered);
    }}
    .axis-label,
    .tick-label {{
      fill: var(--axis);
      font-size: 12px;
      font-variant-numeric: tabular-nums;
    }}
    .grid {{
      stroke: var(--grid);
      stroke-width: 1;
    }}
    .axis {{
      stroke: var(--axis);
      stroke-width: 1;
    }}
    .panel-border {{
      fill: none;
      stroke: var(--border);
      stroke-width: 1;
    }}
    .raw-line {{
      fill: none;
      stroke: var(--raw);
      stroke-width: 3;
    }}
    .filtered-line {{
      fill: none;
      stroke: var(--filtered);
      stroke-width: 3;
    }}
  </style>
</head>
<body>
  <div class="viewer">
    <div class="header">
      <div><strong>Raw IR</strong> and <strong>Filtered IR</strong></div>
      <div class="hint">wheel to zoom, drag with pointerdown to pan</div>
      <div id="status">timeWindow: pending</div>
    </div>
    <svg id="chart" viewBox="0 0 {HTML_VIEWER_WIDTH} {HTML_VIEWER_HEIGHT}" role="img" aria-label="Task 3 compare viewer">
      <defs>
        <clipPath id="topClip">
          <rect x="{MARGIN_LEFT}" y="{MARGIN_TOP}" width="{CANVAS_WIDTH - MARGIN_LEFT - MARGIN_RIGHT}" height="{(CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM - PANEL_GAP) // 2}" />
        </clipPath>
        <clipPath id="bottomClip">
          <rect x="{MARGIN_LEFT}" y="{MARGIN_TOP + ((CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM - PANEL_GAP) // 2) + PANEL_GAP}" width="{CANVAS_WIDTH - MARGIN_LEFT - MARGIN_RIGHT}" height="{(CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM - PANEL_GAP) // 2}" />
        </clipPath>
      </defs>
      <g id="topPanel">
        <rect class="panel-border" x="{MARGIN_LEFT}" y="{MARGIN_TOP}" width="{CANVAS_WIDTH - MARGIN_LEFT - MARGIN_RIGHT}" height="{(CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM - PANEL_GAP) // 2}" />
        <g id="topGrid"></g>
        <g clip-path="url(#topClip)">
          <path id="rawLine" class="raw-line"></path>
        </g>
        <text class="panel-label raw" x="{MARGIN_LEFT}" y="{MARGIN_TOP - 8}">Raw IR</text>
      </g>
      <g id="bottomPanel">
        <rect class="panel-border" x="{MARGIN_LEFT}" y="{MARGIN_TOP + ((CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM - PANEL_GAP) // 2) + PANEL_GAP}" width="{CANVAS_WIDTH - MARGIN_LEFT - MARGIN_RIGHT}" height="{(CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM - PANEL_GAP) // 2}" />
        <g id="bottomGrid"></g>
        <g clip-path="url(#bottomClip)">
          <path id="filteredLine" class="filtered-line"></path>
        </g>
        <text class="panel-label filtered" x="{MARGIN_LEFT}" y="{MARGIN_TOP + ((CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM - PANEL_GAP) // 2) + PANEL_GAP - 8}">Filtered IR</text>
      </g>
      <g id="axisLayer"></g>
      <rect id="interactionLayer" x="{MARGIN_LEFT}" y="{MARGIN_TOP}" width="{CANVAS_WIDTH - MARGIN_LEFT - MARGIN_RIGHT}" height="{CANVAS_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM}" fill="transparent"></rect>
    </svg>
  </div>
  <script id="chart-data" type="application/json">{json.dumps(payload, separators=(",", ":")).replace("</", "<\\/")}</script>
  <script>
    function clientToViewBoxX(clientX, clientLeft, clientWidth, viewBoxWidth) {{
      return ((clientX - clientLeft) / clientWidth) * viewBoxWidth;
    }}

    function clientToViewBoxY(clientY, clientTop, clientHeight, viewBoxHeight) {{
      return ((clientY - clientTop) / clientHeight) * viewBoxHeight;
    }}

    function formatElapsedSeconds(timestamp, firstTimestamp) {{
      return `${{Math.floor((timestamp - firstTimestamp) / 1000)}} s`;
    }}

    function bootTask3CompareViewer() {{
    const data = JSON.parse(document.getElementById("chart-data").textContent);
    const svg = document.getElementById("chart");
    const viewBox = svg.viewBox.baseVal;
    const rawLine = document.getElementById("rawLine");
    const filteredLine = document.getElementById("filteredLine");
    const topGrid = document.getElementById("topGrid");
    const bottomGrid = document.getElementById("bottomGrid");
    const axisLayer = document.getElementById("axisLayer");
    const status = document.getElementById("status");
    const interactionLayer = document.getElementById("interactionLayer");
    const plotLeft = data.plot.marginLeft;
    const plotRight = data.plot.width - data.plot.marginRight;
    const plotWidth = plotRight - plotLeft;
    const totalPlotHeight = data.plot.height - data.plot.marginTop - data.plot.marginBottom - data.plot.panelGap;
    const panelHeight = Math.floor(totalPlotHeight / 2);
    const topPlotTop = data.plot.marginTop;
    const topPlotBottom = topPlotTop + panelHeight;
    const bottomPlotTop = topPlotBottom + data.plot.panelGap;
    const bottomPlotBottom = bottomPlotTop + panelHeight;
    const rawMin = Math.min(...data.rawIr);
    const rawMax = Math.max(...data.rawIr);
    const filteredMin = Math.min(...data.filteredIr);
    const filteredMax = Math.max(...data.filteredIr);
    const dataMinTimestamp = data.timestamps[0];
    const dataMaxTimestamp = data.timestamps[data.timestamps.length - 1];
    let timeWindow = [dataMinTimestamp, dataMaxTimestamp];
    let dragState = null;

    function clamp(value, low, high) {{
      return Math.min(high, Math.max(low, value));
    }}

    function clientToViewBox(clientX, clientY) {{
      const rect = svg.getBoundingClientRect();
      return {{
        x: clientToViewBoxX(clientX, rect.left, rect.width, viewBox.width),
        y: clientToViewBoxY(clientY, rect.top, rect.height, viewBox.height),
      }};
    }}

    function scaleX(timestamp) {{
      const [start, end] = timeWindow;
      const span = Math.max(end - start, 1);
      return plotLeft + ((timestamp - start) / span) * plotWidth;
    }}

    function scaleY(value, low, high, top, bottom) {{
      const span = Math.max(high - low, 1);
      return bottom - ((value - low) / span) * (bottom - top);
    }}

    function buildPath(values, low, high, top, bottom) {{
      return data.timestamps.map((timestamp, index) => {{
        const x = scaleX(timestamp);
        const y = scaleY(values[index], low, high, top, bottom);
        return `${{index === 0 ? "M" : "L"}} ${{x.toFixed(2)}} ${{y.toFixed(2)}}`;
      }}).join(" ");
    }}

    function renderTicks(container, top, bottom) {{
      container.replaceChildren();
      const tickCount = 5;
      for (let index = 0; index < tickCount; index += 1) {{
        const timestamp = timeWindow[0] + ((timeWindow[1] - timeWindow[0]) * index) / (tickCount - 1);
        const x = scaleX(timestamp);
        const tickLine = document.createElementNS("http://www.w3.org/2000/svg", "line");
        tickLine.setAttribute("x1", x.toFixed(2));
        tickLine.setAttribute("x2", x.toFixed(2));
        tickLine.setAttribute("y1", bottom.toFixed(2));
        tickLine.setAttribute("y2", (bottom + 8).toFixed(2));
        tickLine.setAttribute("class", "axis");
        container.appendChild(tickLine);

        const tickLabel = document.createElementNS("http://www.w3.org/2000/svg", "text");
        tickLabel.setAttribute("x", (x - 18).toFixed(2));
        tickLabel.setAttribute("y", (bottom + 22).toFixed(2));
        tickLabel.setAttribute("class", "tick-label");
        tickLabel.textContent = formatElapsedSeconds(timestamp, dataMinTimestamp);
        container.appendChild(tickLabel);
      }}
    }}

    function renderGrid(container, top, bottom) {{
      container.replaceChildren();
      const fractions = [0.25, 0.5, 0.75];
      for (const fraction of fractions) {{
        const y = top + fraction * (bottom - top);
        const line = document.createElementNS("http://www.w3.org/2000/svg", "line");
        line.setAttribute("x1", plotLeft.toFixed(2));
        line.setAttribute("x2", plotRight.toFixed(2));
        line.setAttribute("y1", y.toFixed(2));
        line.setAttribute("y2", y.toFixed(2));
        line.setAttribute("class", "grid");
        container.appendChild(line);
      }}
    }}

    function clampWindow(start, end) {{
      const maxSpan = dataMaxTimestamp - dataMinTimestamp;
      let nextStart = start;
      let nextEnd = end;
      let span = nextEnd - nextStart;
      if (span >= maxSpan) {{
        return [dataMinTimestamp, dataMaxTimestamp];
      }}
      if (nextStart < dataMinTimestamp) {{
        nextEnd += dataMinTimestamp - nextStart;
        nextStart = dataMinTimestamp;
      }}
      if (nextEnd > dataMaxTimestamp) {{
        nextStart -= nextEnd - dataMaxTimestamp;
        nextEnd = dataMaxTimestamp;
      }}
      if (nextEnd - nextStart < 1) {{
        span = 1;
        nextEnd = nextStart + span;
      }}
      return [nextStart, nextEnd];
    }}

    function zoomAt(clientX, wheelDelta) {{
      const point = clientToViewBox(clientX, 0);
      const x = clamp(point.x, plotLeft, plotRight);
      const [start, end] = timeWindow;
      const span = end - start;
      const anchor = start + ((x - plotLeft) / plotWidth) * span;
      const factor = wheelDelta > 0 ? 1.12 : 0.89;
      const nextSpan = clamp(span * factor, 1, dataMaxTimestamp - dataMinTimestamp);
      let nextStart = anchor - ((anchor - start) / span) * nextSpan;
      let nextEnd = nextStart + nextSpan;
      [nextStart, nextEnd] = clampWindow(nextStart, nextEnd);
      timeWindow = [nextStart, nextEnd];
      render();
    }}

    function onWheel(event) {{
      event.preventDefault();
      zoomAt(event.clientX, event.deltaY);
    }}

    function onPointerDown(event) {{
      event.preventDefault();
      interactionLayer.setPointerCapture(event.pointerId);
      const point = clientToViewBox(event.clientX, event.clientY);
      dragState = {{
        pointerId: event.pointerId,
        startX: point.x,
        window: timeWindow.slice(),
      }};
    }}

    function onPointerMove(event) {{
      if (!dragState || dragState.pointerId !== event.pointerId) {{
        return;
      }}
      const point = clientToViewBox(event.clientX, event.clientY);
      const deltaPx = point.x - dragState.startX;
      const windowSpan = dragState.window[1] - dragState.window[0];
      const deltaTime = (deltaPx / plotWidth) * windowSpan;
      let nextStart = dragState.window[0] - deltaTime;
      let nextEnd = dragState.window[1] - deltaTime;
      [nextStart, nextEnd] = clampWindow(nextStart, nextEnd);
      timeWindow = [nextStart, nextEnd];
      render();
    }}

    function onPointerUp(event) {{
      if (dragState && dragState.pointerId === event.pointerId) {{
        dragState = null;
      }}
    }}

    function render() {{
      rawLine.setAttribute("d", buildPath(data.rawIr, rawMin, rawMax, topPlotTop, topPlotBottom));
      filteredLine.setAttribute("d", buildPath(data.filteredIr, filteredMin, filteredMax, bottomPlotTop, bottomPlotBottom));
      renderGrid(topGrid, topPlotTop, topPlotBottom);
      renderGrid(bottomGrid, bottomPlotTop, bottomPlotBottom);
      renderTicks(axisLayer, bottomPlotTop, bottomPlotBottom);
      status.textContent = `timeWindow: ${{formatElapsedSeconds(timeWindow[0], dataMinTimestamp)}} - ${{formatElapsedSeconds(timeWindow[1], dataMinTimestamp)}}`;
    }}

    interactionLayer.addEventListener("wheel", onWheel, {{ passive: false }});
    interactionLayer.addEventListener("pointerdown", onPointerDown);
    interactionLayer.addEventListener("pointermove", onPointerMove);
    interactionLayer.addEventListener("pointerup", onPointerUp);
    interactionLayer.addEventListener("pointercancel", onPointerUp);
    render();
    }}

    if (typeof module !== "undefined" && module.exports) {{
      module.exports = {{
        clientToViewBoxX,
        clientToViewBoxY,
        formatElapsedSeconds,
      }};
    }}

    if (typeof document !== "undefined") {{
      bootTask3CompareViewer();
    }}
  </script>
</body>
</html>
"""
    output_path.write_text(html, encoding="utf-8")


def _render_task3_plots(
    input_path: Path, rows: list[SampleRow]
) -> tuple[Path, Path, Path]:
    _validate_strictly_increasing_timestamps(rows)

    raw_red_values, raw_ir_values = _normalize_against_combined_range(
        [row.red for row in rows],
        [row.ir for row in rows],
    )
    compare_raw_ir = normalize_series([row.ir for row in rows])
    compare_filtered_ir = normalize_series([row.filtered_ir for row in rows])

    raw_path = input_path.with_name(f"{input_path.stem}-raw.png")
    compare_path = input_path.with_name(f"{input_path.stem}-compare.png")
    html_path = input_path.with_name(f"{input_path.stem}-compare.html")

    _draw_overlay_plot(
        raw_path,
        rows,
        raw_red_values,
        raw_ir_values,
        RAW_RED_COLOR,
        RAW_IR_COLOR,
    )
    _draw_stacked_compare_plot(
        compare_path,
        rows,
        compare_raw_ir,
        compare_filtered_ir,
    )
    _render_task3_compare_html(
        html_path,
        rows,
        [row.ir for row in rows],
        [row.filtered_ir for row in rows],
    )

    return raw_path, compare_path, html_path


def render_task3_compare(input_path: Path) -> tuple[Path, Path, Path]:
    rows = _read_rows(input_path)
    return _render_task3_plots(input_path, rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", dest="input_csv", type=Path, required=True)
    args = parser.parse_args()

    input_path = args.input_csv.expanduser().resolve()
    try:
        rows = _read_rows(input_path)
        raw_path, compare_path, _html_path = _render_task3_plots(input_path, rows)
    except (OSError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc

    gap_ms = max_gap_ms(rows)
    print(
        f"rendered {raw_path.name} and {compare_path.name} (max_gap_ms={gap_ms})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
