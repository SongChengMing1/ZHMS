from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class RenderTask2PlotsTests(unittest.TestCase):
    def _run_renderer(self, csv_path: Path) -> subprocess.CompletedProcess[str]:
        repo_root = Path(__file__).resolve().parents[3]

        return subprocess.run(
            [
                sys.executable,
                "tools/ppg_baseline/render_task2_plots.py",
                "--input",
                str(csv_path),
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_renderer_creates_raw_and_norm_pngs(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,status",
                        "100,10,100,ok",
                        "120,20,90,ok",
                        "140,30,80,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertEqual(result.returncode, 0, msg=result.stderr)

            raw_path = csv_path.with_name("stable-01-raw.png")
            norm_path = csv_path.with_name("stable-01-norm.png")

            self.assertTrue(raw_path.exists())
            self.assertTrue(norm_path.exists())
            self.assertGreater(raw_path.stat().st_size, 0)
            self.assertGreater(norm_path.stat().st_size, 0)
            self.assertIn("rendered stable-01-raw.png and stable-01-norm.png", result.stdout)
            self.assertIn("max_gap_ms=20", result.stdout)

    def test_renderer_rejects_malformed_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,status",
                        "100,10,100,ok",
                        "BADROW",
                        "140,30,80,ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("BADROW", result.stderr)
            self.assertFalse(csv_path.with_name("stable-01-raw.png").exists())
            self.assertFalse(csv_path.with_name("stable-01-norm.png").exists())

    def test_renderer_rejects_unexpected_status_values(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text(
                "\n".join(
                    [
                        "timestamp_ms,red,ir,status",
                        "100,10,100,ok",
                        "120,20,90,error",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = self._run_renderer(csv_path)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("error", result.stderr)
            self.assertFalse(csv_path.with_name("stable-01-raw.png").exists())
            self.assertFalse(csv_path.with_name("stable-01-norm.png").exists())

    def test_renderer_rejects_empty_or_no_valid_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            csv_path = Path(tmpdir) / "stable-01.csv"
            csv_path.write_text("timestamp_ms,red,ir,status\n", encoding="utf-8")

            result = self._run_renderer(csv_path)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no valid sample rows found", result.stderr)
            self.assertFalse(csv_path.with_name("stable-01-raw.png").exists())
            self.assertFalse(csv_path.with_name("stable-01-norm.png").exists())
