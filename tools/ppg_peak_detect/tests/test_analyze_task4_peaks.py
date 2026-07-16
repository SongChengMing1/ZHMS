from __future__ import annotations

import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image

from tools.ppg_peak_detect.common import (
    BOARD_CSV_HEADER,
    BOARD_CSV_HEADER_WITH_DEBUG_STATE,
    BOARD_CSV_HEADER_WITH_RESETS,
)


class AnalyzeTask4PeaksTests(unittest.TestCase):
    def _run_analyzer(
        self, csv_path: Path, output_dir: Path
    ) -> subprocess.CompletedProcess[str]:
        repo_root = Path(__file__).resolve().parents[3]

        return subprocess.run(
            [
                sys.executable,
                "tools/ppg_peak_detect/analyze_task4_peaks.py",
                "--input",
                str(csv_path),
                "--output-dir",
                str(output_dir),
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_analyzer_creates_expected_artifacts_for_stable_like_csv(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "stable-01.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "0,0,0,0,ok",
                        "100,0,0,20,ok",
                        "200,0,0,120,ok",
                        "300,0,0,30,ok",
                        "400,0,0,0,ok",
                        "900,0,0,20,ok",
                        "1000,0,0,130,ok",
                        "1100,0,0,40,ok",
                        "1200,0,0,0,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_analyzer(csv_path, output_dir)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertIn("analyzed stable-01: valid_ibis=0", result.stdout)
            self.assertTrue((output_dir / "stable-01-peaks.csv").exists())
            self.assertTrue((output_dir / "stable-01-summary.json").exists())
            self.assertTrue((output_dir / "stable-01-overlay.png").exists())

    def test_analyzer_accepts_board_export_style_csv(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "board-export.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        BOARD_CSV_HEADER,
                        "0,0,0,0,0,0,0,0,0,ok",
                        "100,0,0,20,12,0,0,0,0,ok",
                        "200,0,0,120,34,1,0,1,1,ok",
                        "300,0,0,30,18,0,0,1,1,ok",
                        "400,0,0,0,10,0,0,1,1,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_analyzer(csv_path, output_dir)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue((output_dir / "board-export-peaks.csv").exists())
            self.assertTrue((output_dir / "board-export-summary.json").exists())
            self.assertTrue((output_dir / "board-export-overlay.png").exists())

    def test_analyzer_accepts_board_export_csv_with_reset_columns(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "board-export.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        BOARD_CSV_HEADER_WITH_RESETS,
                        "0,0,0,0,0,0,0,0,0,0,0,ok",
                        "100,0,0,20,12,0,0,0,0,0,0,ok",
                        "200,0,0,120,34,1,0,1,1,0,0,ok",
                        "300,0,0,30,18,0,0,1,1,0,0,ok",
                        "400,0,0,0,10,0,0,1,1,0,0,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_analyzer(csv_path, output_dir)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue((output_dir / "board-export-peaks.csv").exists())
            self.assertTrue((output_dir / "board-export-summary.json").exists())
            self.assertTrue((output_dir / "board-export-overlay.png").exists())

    def test_analyzer_accepts_board_export_csv_with_debug_state_columns(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "board-export.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        BOARD_CSV_HEADER_WITH_DEBUG_STATE,
                        "0,0,0,0,0,0,0,0,0,0,0,8.000,0.000,0,0,ok",
                        "100,0,0,20,12,0,0,0,0,0,0,8.000,0.000,0,0,ok",
                        "200,0,0,120,34,1,0,1,1,0,0,22.000,0.000,0,1,ok",
                        "300,0,0,30,18,0,0,1,1,0,0,22.000,2.250,0,1,ok",
                        "400,0,0,0,10,0,0,1,1,0,0,22.000,2.250,0,1,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_analyzer(csv_path, output_dir)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue((output_dir / "board-export-peaks.csv").exists())
            self.assertTrue((output_dir / "board-export-summary.json").exists())
            self.assertTrue((output_dir / "board-export-overlay.png").exists())

    def test_analyzer_accepts_board_export_csv_with_finger_debug_columns(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "board-export.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,threshold,peak_flag,ibi_ms,ibi_valid,signal_valid,pipeline_reset_flag,pipeline_reset_count,signal_level,noise_level,invalid_candidate_count,have_last_peak,finger_state,filtered_ir_p2p_1s,status",
                        "0,0,0,0,0,0,0,0,0,0,0,8.000,0.000,0,0,1,10,ok",
                        "100,0,0,20,12,0,0,0,0,0,0,8.000,0.000,0,0,1,10,ok",
                        "200,0,0,120,34,1,0,1,1,0,0,22.000,0.000,0,1,1,10,ok",
                        "300,0,0,30,18,0,0,1,1,0,0,22.000,2.250,0,1,1,10,ok",
                        "400,0,0,0,10,0,0,1,1,0,0,22.000,2.250,0,1,1,10,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_analyzer(csv_path, output_dir)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue((output_dir / "board-export-peaks.csv").exists())
            self.assertTrue((output_dir / "board-export-summary.json").exists())
            self.assertTrue((output_dir / "board-export-overlay.png").exists())

    def test_analyzer_emits_peak_event_row_for_true_peak_sample(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "stable-01.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "0,0,0,0,ok",
                        "100,0,0,20,ok",
                        "200,0,0,120,ok",
                        "300,0,0,30,ok",
                        "400,0,0,0,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_analyzer(csv_path, output_dir)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            with (output_dir / "stable-01-peaks.csv").open(
                encoding="utf-8", newline=""
            ) as handle:
                rows = list(csv.DictReader(handle))

            peak_rows = [row for row in rows if row["peak_detected"] == "true"]
            self.assertEqual(len(peak_rows), 1)
            peak_row = peak_rows[0]
            self.assertEqual(len(rows), 1)
            self.assertEqual(peak_row["timestamp_ms"], "200")
            self.assertEqual(peak_row["filtered_ir"], "120")
            self.assertEqual(peak_row["peak_timestamp_ms"], "200")
            self.assertEqual(peak_row["peak_detected"], "true")

            with Image.open(output_dir / "stable-01-overlay.png") as overlay:
                actual_peak_x = self._overlay_x_position(
                    [0, 100, 200, 300, 400], 200, overlay.width
                )
                trailing_x = self._overlay_x_position(
                    [0, 100, 200, 300, 400], 300, overlay.width
                )
                self.assertGreater(
                    self._count_red_pixels_in_column(overlay, actual_peak_x), 0
                )
                self.assertEqual(
                    self._count_red_pixels_in_column(overlay, trailing_x), 0
                )

    def test_analyzer_fails_fast_on_repeated_header_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "broken-01.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "0,0,0,0,ok",
                        "100,0,0,2,ok",
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "200,0,0,10,ok",
                        "300,0,0,3,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_analyzer(csv_path, output_dir)

            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((output_dir / "broken-01-peaks.csv").exists())
            self.assertFalse((output_dir / "broken-01-summary.json").exists())
            self.assertFalse((output_dir / "broken-01-overlay.png").exists())

    def test_analyzer_reports_idle_like_signal_as_invalid(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "idle-01.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "0,0,0,0,ok",
                        "100,0,0,0,ok",
                        "200,0,0,0,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_analyzer(csv_path, output_dir)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            summary = json.loads((output_dir / "idle-01-summary.json").read_text())
            self.assertEqual(summary["valid_peak_count"], 0)
            self.assertEqual(summary["valid_ibi_count"], 0)
            self.assertFalse(summary["signal_valid"])

    def test_analyzer_fails_fast_on_malformed_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "broken-01.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "0,0,0,0,ok",
                        "BROKEN LINE",
                        "100,0,0,2,ok",
                        "200,0,0,10,ok",
                        "300,0,0,3,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_analyzer(csv_path, output_dir)

            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((output_dir / "broken-01-peaks.csv").exists())
            self.assertFalse((output_dir / "broken-01-summary.json").exists())
            self.assertFalse((output_dir / "broken-01-overlay.png").exists())

    def test_analyzer_clears_stale_outputs_before_failed_rerun(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "stable-01.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "0,0,0,0,ok",
                        "100,0,0,20,ok",
                        "200,0,0,120,ok",
                        "300,0,0,30,ok",
                        "400,0,0,0,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            first_result = self._run_analyzer(csv_path, output_dir)
            self.assertEqual(first_result.returncode, 0, msg=first_result.stderr)
            self.assertTrue((output_dir / "stable-01-peaks.csv").exists())
            self.assertTrue((output_dir / "stable-01-summary.json").exists())
            self.assertTrue((output_dir / "stable-01-overlay.png").exists())

            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "0,0,0,0,ok",
                        "BROKEN LINE",
                        "100,0,0,20,ok",
                        "200,0,0,120,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            second_result = self._run_analyzer(csv_path, output_dir)
            self.assertNotEqual(second_result.returncode, 0)
            self.assertFalse((output_dir / "stable-01-peaks.csv").exists())
            self.assertFalse((output_dir / "stable-01-summary.json").exists())
            self.assertFalse((output_dir / "stable-01-overlay.png").exists())

    def test_analyzer_rejects_real_repo_idle_fixture(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        csv_path = (
            repo_root
            / "docs"
            / "需求"
            / "任务划分"
            / "第二阶段"
            / "task3-ppg-preprocess"
            / "idle-01.csv"
        )
        with tempfile.TemporaryDirectory() as tmpdir:
            output_dir = Path(tmpdir) / "out"

            result = self._run_analyzer(csv_path, output_dir)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            summary = json.loads((output_dir / "idle-01-summary.json").read_text())
            self.assertEqual(summary["valid_peak_count"], 0)
            self.assertEqual(summary["valid_ibi_count"], 0)
            self.assertFalse(summary["signal_valid"])

    def test_analyzer_keeps_real_repo_stable_fixture_valid(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        csv_path = (
            repo_root
            / "docs"
            / "需求"
            / "任务划分"
            / "第二阶段"
            / "task3-ppg-preprocess"
            / "stable-01.csv"
        )
        with tempfile.TemporaryDirectory() as tmpdir:
            output_dir = Path(tmpdir) / "out"

            result = self._run_analyzer(csv_path, output_dir)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            summary = json.loads((output_dir / "stable-01-summary.json").read_text())
            self.assertGreater(summary["valid_peak_count"], 0)
            self.assertGreater(summary["valid_ibi_count"], 0)

    def test_peak_csv_uses_expected_columns(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "stable-01.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "0,0,0,0,ok",
                        "100,0,0,2,ok",
                        "200,0,0,10,ok",
                        "300,0,0,3,ok",
                        "400,0,0,0,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_analyzer(csv_path, output_dir)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            with (output_dir / "stable-01-peaks.csv").open(
                encoding="utf-8", newline=""
            ) as handle:
                reader = csv.DictReader(handle)
                self.assertEqual(
                    reader.fieldnames,
                    [
                        "timestamp_ms",
                        "filtered_ir",
                        "threshold",
                        "peak_detected",
                        "peak_timestamp_ms",
                        "ibi_ms",
                        "ibi_valid",
                        "signal_valid",
                    ],
                )

    def _overlay_x_position(self, timestamps: list[int], timestamp_ms: int, width: int) -> int:
        margin_left = 64
        margin_right = 24
        plot_width = width - margin_left - margin_right
        low = min(timestamps)
        high = max(timestamps)
        return round(margin_left + ((timestamp_ms - low) / (high - low)) * plot_width)

    def _count_red_pixels_in_column(self, image: Image.Image, x: int) -> int:
        red_pixels = 0
        for y in range(image.height):
            r, g, b = image.getpixel((x, y))
            if r > 180 and g < 120 and b < 120:
                red_pixels += 1
        return red_pixels


if __name__ == "__main__":
    unittest.main()
