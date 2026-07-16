from __future__ import annotations

import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image
from tools.ppg_peak_detect.render_task4_board_overlay import (
    _build_marker_row_indexes,
    _read_rows,
)


class RenderTask4BoardOverlayTests(unittest.TestCase):
    def _run_renderer(
        self, csv_path: Path, output_dir: Path, *extra_args: str
    ) -> subprocess.CompletedProcess[str]:
        repo_root = Path(__file__).resolve().parents[3]

        return subprocess.run(
            [
                sys.executable,
                "tools/ppg_peak_detect/render_task4_board_overlay.py",
                "--input",
                str(csv_path),
                "--output-dir",
                str(output_dir),
                *extra_args,
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_renderer_creates_clean_csv_summary_and_overlay(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "stable-13-board.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        "0,300,-50,0,0,0,0,0,0,na,0,ok",
                        "10,320,-10,0,0,0,0,0,0,na,0,ok",
                        "20,330,20,10,0,0,0,0,1,na,0,ok",
                        "30,340,180,80,0,0,0,1,2,na,0,ok",
                        "40,350,120,90,0,0,0,1,2,72,0,ok",
                        "50,360,210,100,1,810,1,1,2,74,30,ok",
                        "60,370,80,95,0,0,0,1,2,75,0,ok",
                        "W: Connect timeout",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path, output_dir)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            clean_path = output_dir / "stable-13-board-clean.csv"
            summary_path = output_dir / "stable-13-board-clean-summary.json"
            overlay_path = output_dir / "stable-13-board-clean-overlay.png"
            self.assertTrue(clean_path.exists())
            self.assertTrue(summary_path.exists())
            self.assertTrue(overlay_path.exists())

            with clean_path.open(encoding="utf-8", newline="") as handle:
                rows = list(csv.reader(handle))
            self.assertEqual(
                rows[0],
                [
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
                ],
            )
            self.assertEqual(len(rows), 8)

            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            self.assertEqual(summary["row_count"], 7)
            self.assertEqual(summary["peak_count"], 1)
            self.assertEqual(summary["valid_ibi_count"], 1)
            self.assertEqual(summary["finger_state_counts"], {"0": 2, "1": 1, "2": 4})
            self.assertEqual(summary["bpm_valid_sample_count"], 3)
            self.assertEqual(summary["bpm_min"], 72)
            self.assertEqual(summary["bpm_max"], 75)
            self.assertEqual(summary["plot_y_range"], [-1000, 1000])

            with Image.open(overlay_path) as overlay:
                self.assertEqual(overlay.size, (2000, 1000))

    def test_renderer_accepts_explicit_y_axis_range(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            csv_path = tmpdir_path / "stable-14-board.csv"
            output_dir = tmpdir_path / "out"
            csv_path.write_text(
                "\n".join(
                    [
                        "0,300,-20,10,0,0,0,0,0,na,0,ok",
                        "10,320,0,20,0,0,0,0,1,na,0,ok",
                        "20,330,40,30,1,0,0,1,2,80,20,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(
                csv_path, output_dir, "--y-min", "-500", "--y-max", "500"
            )

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            summary = json.loads(
                (output_dir / "stable-14-board-clean-summary.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(summary["plot_y_range"], [-500, 500])

    def test_build_marker_row_indexes_remaps_event_row_to_confirmed_peak_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-15-board.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "0,300,-50,0,0,0,0,0,0,na,0,ok",
                        "10,320,-10,0,0,0,0,0,0,na,0,ok",
                        "20,330,20,10,0,0,0,0,1,na,0,ok",
                        "30,340,180,80,0,0,0,1,2,na,0,ok",
                        "40,350,120,90,0,0,0,1,2,72,0,ok",
                        "50,360,210,100,1,810,1,1,2,74,30,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            rows = _read_rows(csv_path)
            peak_rows, ibi_rows = _build_marker_row_indexes(rows)

            self.assertEqual(peak_rows, [3])
            self.assertEqual(ibi_rows, [3])


if __name__ == "__main__":
    unittest.main()
