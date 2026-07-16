import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

import tools.ppg_preprocess.render_task3_compare as renderer
from tools.ppg_preprocess.common import SampleRow


class RenderTask3CompareTests(unittest.TestCase):
    def _run_renderer(self, csv_path: Path) -> subprocess.CompletedProcess[str]:
        repo_root = Path(__file__).resolve().parents[3]

        return subprocess.run(
            [
                sys.executable,
                "tools/ppg_preprocess/render_task3_compare.py",
                "--input",
                str(csv_path),
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )

    def _extract_embedded_js(self, html_text: str) -> str:
        match = re.search(
            r"<script>\n(.*)\n  </script>\n</body>",
            html_text,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        assert match is not None
        return match.group(1)

    def _run_js_helpers_in_node(self, js_text: str) -> dict[str, object]:
        with tempfile.TemporaryDirectory() as tmpdir:
            js_path = Path(tmpdir) / "compare-viewer.js"
            js_path.write_text(js_text, encoding="utf-8")

            node_script = (
                "const viewer = require(process.argv[1]);\n"
                "const result = {\n"
                "  x: viewer.clientToViewBoxX(110, 10, 50, 200),\n"
                "  elapsed: viewer.formatElapsedSeconds(6000, 1000),\n"
                "};\n"
                "process.stdout.write(JSON.stringify(result));\n"
            )
            result = subprocess.run(
                ["node", "-e", node_script, str(js_path)],
                cwd=Path(__file__).resolve().parents[3],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            return json.loads(result.stdout)

    def test_renderer_creates_raw_and_compare_pngs(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "100,10,100,0,ok",
                        "120,20,110,5,ok",
                        "140,30,120,8,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            raw_path = csv_path.with_name("stable-01-raw.png")
            compare_path = csv_path.with_name("stable-01-compare.png")
            self.assertTrue(raw_path.exists())
            self.assertTrue(compare_path.exists())
            self.assertGreater(raw_path.stat().st_size, 0)
            self.assertGreater(compare_path.stat().st_size, 0)
            self.assertIn(
                "rendered stable-01-raw.png and stable-01-compare.png",
                result.stdout,
            )

    def test_renderer_creates_compare_html(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "100,10,100,0,ok",
                        "120,20,110,5,ok",
                        "140,30,120,8,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            html_path = csv_path.with_name("stable-01-compare.html")
            self.assertTrue(html_path.exists())
            self.assertGreater(html_path.stat().st_size, 0)

    def test_renderer_compare_html_contains_interaction_markers(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "100,10,100,0,ok",
                        "120,20,110,5,ok",
                        "140,30,120,8,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            html_text = csv_path.with_name("stable-01-compare.html").read_text(
                encoding="utf-8"
            )
            for marker in ("Raw IR", "Filtered IR", "wheel", "pointerdown", "timeWindow"):
                self.assertIn(marker, html_text)

    def test_renderer_compare_html_uses_viewbox_coordinates_and_elapsed_seconds(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "1000,10,100,0,ok",
                        "6000,20,110,5,ok",
                        "11000,30,120,8,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            html_text = csv_path.with_name("stable-01-compare.html").read_text(
                encoding="utf-8"
            )
            self.assertIn("getBoundingClientRect", html_text)
            self.assertIn("clientToViewBox", html_text)
            self.assertIn("formatElapsedSeconds", html_text)
            self.assertIn(" s", html_text)

    def test_renderer_compare_html_threads_data_mintimestamp_into_elapsed_calls(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "1000,10,100,0,ok",
                        "6000,20,110,5,ok",
                        "11000,30,120,8,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            html_text = csv_path.with_name("stable-01-compare.html").read_text(
                encoding="utf-8"
            )
            self.assertIn(
                "formatElapsedSeconds(timestamp, dataMinTimestamp)",
                html_text,
            )
            self.assertIn(
                "formatElapsedSeconds(timeWindow[0], dataMinTimestamp)",
                html_text,
            )
            self.assertIn(
                "formatElapsedSeconds(timeWindow[1], dataMinTimestamp)",
                html_text,
            )

    def test_renderer_compare_html_executes_helpers_under_node(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "1000,10,100,0,ok",
                        "6000,20,110,5,ok",
                        "11000,30,120,8,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            html_text = csv_path.with_name("stable-01-compare.html").read_text(
                encoding="utf-8"
            )
            js_text = self._extract_embedded_js(html_text)
            results = self._run_js_helpers_in_node(js_text)

            self.assertEqual(results["x"], 400)
            self.assertEqual(results["elapsed"], "5 s")

    def test_renderer_rejects_malformed_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "100,10,100,0,ok",
                        "BADROW",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("BADROW", result.stderr)
            self.assertFalse(csv_path.with_name("stable-01-raw.png").exists())
            self.assertFalse(csv_path.with_name("stable-01-compare.png").exists())
            self.assertFalse(csv_path.with_name("stable-01-compare.html").exists())

    def test_renderer_rejects_undersized_capture(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "100,10,100,0,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("at least 2 sample rows", result.stderr)
            self.assertFalse(csv_path.with_name("stable-01-raw.png").exists())
            self.assertFalse(csv_path.with_name("stable-01-compare.png").exists())
            self.assertFalse(csv_path.with_name("stable-01-compare.html").exists())

    def test_renderer_rejects_non_increasing_timestamps(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,filtered_ir,status",
                        "200,10,100,0,ok",
                        "100,20,110,5,ok",
                        "300,30,120,8,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("strictly increasing", result.stderr)
            self.assertFalse(csv_path.with_name("stable-01-raw.png").exists())
            self.assertFalse(csv_path.with_name("stable-01-compare.png").exists())
            self.assertFalse(csv_path.with_name("stable-01-compare.html").exists())

    def test_renderer_uses_timestamp_spacing_for_points(self) -> None:
        helper = getattr(renderer, "_x_positions_for_rows", None)
        self.assertIsNotNone(helper)
        assert helper is not None

        rows = [
            SampleRow(100, 10, 100, 0, "ok"),
            SampleRow(110, 20, 110, 5, "ok"),
            SampleRow(190, 30, 120, 8, "ok"),
        ]
        x_positions = helper(rows)

        self.assertLess(
            x_positions[1] - x_positions[0],
            x_positions[2] - x_positions[1],
        )

    def test_renderer_time_tick_labels_use_relative_seconds(self) -> None:
        helper = getattr(renderer, "_time_tick_labels", None)
        self.assertIsNotNone(helper)
        assert helper is not None

        rows = [
            SampleRow(1000, 10, 100, 0, "ok"),
            SampleRow(6000, 20, 110, 5, "ok"),
            SampleRow(11000, 30, 120, 8, "ok"),
        ]

        self.assertEqual(
            helper(rows),
            [(1000, "0 s"), (6000, "5 s"), (11000, "10 s")],
        )

    def test_renderer_maps_timestamp_to_plot_x_position(self) -> None:
        helper = getattr(renderer, "_x_position_for_timestamp", None)
        self.assertIsNotNone(helper)
        assert helper is not None

        rows = [
            SampleRow(1000, 10, 100, 0, "ok"),
            SampleRow(6000, 20, 110, 5, "ok"),
            SampleRow(11000, 30, 120, 8, "ok"),
        ]

        self.assertEqual(helper(rows, 1000), 56)
        self.assertEqual(helper(rows, 6000), 496)
        self.assertEqual(helper(rows, 11000), 936)

    def test_renderer_uses_stacked_compare_panels(self) -> None:
        helper = getattr(renderer, "_plot_points_for_rows", None)
        self.assertIsNotNone(helper)
        assert helper is not None

        rows = [
            SampleRow(100, 10, 100, 0, "ok"),
            SampleRow(120, 20, 110, 5, "ok"),
            SampleRow(140, 30, 120, 8, "ok"),
        ]
        top_points = helper(rows, [0.0, 0.5, 1.0], 24, 220)
        bottom_points = helper(rows, [0.0, 0.5, 1.0], 280, 476)

        self.assertLess(max(y for _, y in top_points), min(y for _, y in bottom_points))


if __name__ == "__main__":
    unittest.main()
