from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

from tools.ppg_preprocess.capture_task3_sample import (
    capture_rows_from_lines,
    capture_rows_from_serial,
)
from tools.ppg_preprocess.common import max_gap_ms


class CaptureTask3SampleTests(unittest.TestCase):
    def test_capture_rows_uses_first_valid_window(self) -> None:
        lines = [
            "boot log\n",
            "100,10,100,0,ok\n",
            "120,11,101,2,ok\n",
            "140,12,102,4,ok\n",
            "160,13,103,5,ok\n",
        ]
        rows = capture_rows_from_lines(lines, 50)
        self.assertEqual([row.timestamp_ms for row in rows], [100, 120, 140, 160])
        self.assertEqual(max_gap_ms(rows), 20)

    def test_capture_rows_rejects_large_timestamp_gap(self) -> None:
        lines = [
            "100,10,100,0,ok\n",
            "120,11,101,2,ok\n",
            "5000,12,102,4,ok\n",
            "5020,13,103,5,ok\n",
        ]
        with self.assertRaisesRegex(ValueError, "max timestamp gap"):
            capture_rows_from_lines(lines, 50)

    def test_capture_rows_rejects_timestamp_reset(self) -> None:
        lines = [
            "100,10,100,0,ok\n",
            "120,11,101,2,ok\n",
            "110,12,102,4,ok\n",
            "140,13,103,5,ok\n",
        ]
        with self.assertRaisesRegex(ValueError, "strictly increasing"):
            capture_rows_from_lines(lines, 30)

    def test_capture_rows_rejects_incomplete_duration(self) -> None:
        lines = [
            "100,10,100,0,ok\n",
            "120,11,101,2,ok\n",
        ]
        with self.assertRaisesRegex(ValueError, "requested duration"):
            capture_rows_from_lines(lines, 50)

    @mock.patch("tools.ppg_preprocess.capture_task3_sample.serial.Serial")
    def test_capture_rows_from_serial_rejects_non_positive_duration(self, serial_cls: mock.Mock) -> None:
        with self.assertRaisesRegex(ValueError, "duration_ms must be positive"):
            capture_rows_from_serial("COM1", 115200, 1.0, 0)

        serial_cls.assert_not_called()

    def test_script_help_works_from_repo_root(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        result = subprocess.run(
            [
                sys.executable,
                "tools/ppg_preprocess/capture_task3_sample.py",
                "--help",
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("usage:", result.stdout.lower())

    def test_capture_rows_from_serial_fails_on_noise_only_stream(self) -> None:
        serial_cm = mock.MagicMock()
        serial_obj = mock.MagicMock()
        serial_cm.__enter__.return_value = serial_obj
        serial_cm.__exit__.return_value = False
        serial_obj.readline.side_effect = [
            b"boot log\n",
            b"still noise\n",
            b"more noise\n",
            b"last noise\n",
        ]

        with mock.patch("tools.ppg_preprocess.capture_task3_sample.serial.Serial", return_value=serial_cm):
            with mock.patch("tools.ppg_preprocess.capture_task3_sample.MAX_SERIAL_LINES", 3):
                with self.assertRaisesRegex(ValueError, "no valid sample rows captured"):
                    capture_rows_from_serial("COM1", 115200, 1.0, 20)

        self.assertEqual(serial_obj.readline.call_count, 3)

    def test_capture_rows_from_serial_fails_on_empty_reads(self) -> None:
        serial_cm = mock.MagicMock()
        serial_obj = mock.MagicMock()
        serial_cm.__enter__.return_value = serial_obj
        serial_cm.__exit__.return_value = False
        serial_obj.readline.side_effect = [b"", b"", b""]

        with mock.patch("tools.ppg_preprocess.capture_task3_sample.serial.Serial", return_value=serial_cm):
            with mock.patch("tools.ppg_preprocess.capture_task3_sample.MAX_EMPTY_READS", 2):
                with self.assertRaisesRegex(ValueError, "no valid sample rows captured"):
                    capture_rows_from_serial("COM1", 115200, 1.0, 20)

        self.assertEqual(serial_obj.readline.call_count, 2)

    @mock.patch("tools.ppg_preprocess.capture_task3_sample.serial.Serial")
    def test_capture_rows_from_serial_stops_after_duration_window(self, serial_cls: mock.Mock) -> None:
        serial_cm = mock.MagicMock()
        serial_obj = mock.MagicMock()
        serial_cm.__enter__.return_value = serial_obj
        serial_cm.__exit__.return_value = False
        serial_obj.readline.side_effect = [
            b"boot log\n",
            b"100,10,100,0,ok\n",
            b"120,11,101,2,ok\n",
            b"140,12,102,4,ok\n",
        ]
        serial_cls.return_value = serial_cm

        rows = capture_rows_from_serial("COM1", 115200, 1.0, 20)

        self.assertEqual([row.timestamp_ms for row in rows], [100, 120])
        self.assertEqual(serial_obj.readline.call_count, 3)
        serial_cls.assert_called_once_with("COM1", baudrate=115200, timeout=1.0)


if __name__ == "__main__":
    unittest.main()
