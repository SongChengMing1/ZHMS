from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

from tools.ppg_baseline.capture_task2_sample import capture_rows_from_lines, capture_rows_from_serial
from tools.ppg_baseline.common import max_gap_ms, normalize_series, parse_sample_line


class CaptureTask2SampleTests(unittest.TestCase):
    def test_parse_sample_line_filters_logs(self) -> None:
        self.assertIsNone(parse_sample_line("LOG_INF booting"))
        row = parse_sample_line("123,456,789,ok")
        self.assertIsNotNone(row)
        assert row is not None
        self.assertEqual(row.timestamp_ms, 123)
        self.assertEqual(row.red, 456)
        self.assertEqual(row.ir, 789)
        self.assertEqual(row.status, "ok")

    def test_capture_rows_uses_first_valid_window(self) -> None:
        lines = [
            "boot log\n",
            "100,1000,2000,ok\n",
            "120,1001,2001,ok\n",
            "140,1002,2002,ok\n",
            "160,1003,2003,ok\n",
        ]
        rows = capture_rows_from_lines(lines, 50)
        self.assertEqual([row.timestamp_ms for row in rows], [100, 120, 140, 160])
        self.assertEqual(max_gap_ms(rows), 20)

    def test_capture_rows_ignores_non_csv_lines(self) -> None:
        lines = [
            "log line\n",
            "100,1000,2000,ok\n",
            "noise\n",
            "120,1001,2001,ok\n",
        ]
        rows = capture_rows_from_lines(lines, 10)
        self.assertEqual([row.timestamp_ms for row in rows], [100, 120])

    def test_capture_rows_rejects_incomplete_duration(self) -> None:
        lines = [
            "100,1000,2000,ok\n",
            "120,1001,2001,ok\n",
        ]
        with self.assertRaisesRegex(ValueError, "requested duration"):
            capture_rows_from_lines(lines, 50)

    def test_capture_rows_rejects_non_positive_duration(self) -> None:
        lines = [
            "100,1000,2000,ok\n",
        ]
        with self.assertRaisesRegex(ValueError, "duration_ms must be positive"):
            capture_rows_from_lines(lines, 0)

    def test_normalize_series_handles_flat_data(self) -> None:
        self.assertEqual(normalize_series([7, 7, 7]), [0.5, 0.5, 0.5])

    def test_script_help_works_from_repo_root(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        result = subprocess.run(
            [
                sys.executable,
                "tools/ppg_baseline/capture_task2_sample.py",
                "--help",
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("usage:", result.stdout.lower())

    @mock.patch("tools.ppg_baseline.capture_task2_sample.serial.Serial")
    def test_capture_rows_from_serial_stops_after_duration_window(self, serial_cls: mock.Mock) -> None:
        serial_cm = mock.MagicMock()
        serial_obj = mock.MagicMock()
        serial_cm.__enter__.return_value = serial_obj
        serial_cm.__exit__.return_value = False
        serial_obj.readline.side_effect = [
            b"boot log\n",
            b"100,1000,2000,ok\n",
            b"120,1001,2001,ok\n",
            b"140,1002,2002,ok\n",
        ]
        serial_cls.return_value = serial_cm

        rows = capture_rows_from_serial("COM1", 115200, 1.0, 20)

        self.assertEqual([row.timestamp_ms for row in rows], [100, 120])
        self.assertEqual(serial_obj.readline.call_count, 3)
        serial_cls.assert_called_once_with("COM1", baudrate=115200, timeout=1.0)
