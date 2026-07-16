from pathlib import Path
import re
import unittest


class Task3ServiceTimingTests(unittest.TestCase):
    def test_max30102_overlay_remains_at_100hz(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        overlay = (repo_root / "boards/esp32s3_devkitc.overlay").read_text(
            encoding="utf-8"
        )

        smp_sr_match = re.search(r"\bsmp-sr\s*=\s*<(\d+)>\s*;", overlay)
        self.assertIsNotNone(smp_sr_match, "max30102 smp-sr setting missing from overlay")
        assert smp_sr_match is not None
        self.assertEqual(int(smp_sr_match.group(1)), 100)


if __name__ == "__main__":
    unittest.main()
