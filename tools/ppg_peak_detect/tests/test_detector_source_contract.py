from pathlib import Path
import re
import unittest


class Task4DetectorSourceContractTests(unittest.TestCase):
    def test_detector_declares_pending_peak_window_contract(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        header = (repo_root / "src/services/ppg_peak_detect.h").read_text(
            encoding="utf-8"
        )
        source = (repo_root / "src/services/ppg_peak_detect.c").read_text(
            encoding="utf-8"
        )

        self.assertRegex(source, r"#define\s+PPG_CONFIRMATION_WINDOW_MS\s+180U")
        self.assertRegex(source, r"#define\s+PPG_STARTUP_IBI_DEVIATION_PCT\s+30U")
        self.assertIn("bool have_pending_peak;", header)
        self.assertIn("uint32_t pending_peak_timestamp_ms;", header)
        self.assertIn("int pending_peak_value;", header)
        self.assertIn("uint32_t pending_window_start_ms;", header)
        self.assertIn("bool have_startup_reference_ibi;", header)
        self.assertIn("uint16_t startup_reference_ibi_ms;", header)
        self.assertIn("bool have_last_accepted_ibi;", header)
        self.assertIn("uint16_t last_accepted_ibi_ms;", header)
        self.assertIn("uint32_t confirmed_peak_timestamp_ms;", header)
        self.assertIn("clear_pending_peak", source)
        self.assertIn("confirm_pending_peak", source)
        self.assertIn("within_percent_u16", source)
        self.assertIn("startup_ibi_is_valid", source)
        self.assertRegex(
            source,
            r"if\s*\(\s*state->have_pending_peak\s*&&\s*"
            r"timestamp_ms - state->pending_window_start_ms >= PPG_CONFIRMATION_WINDOW_MS\s*\)",
        )
        self.assertRegex(
            source,
            r"if\s*\(\s*!state->have_pending_peak\s*\)\s*\{\s*"
            r"state->have_pending_peak = true;",
        )
        self.assertRegex(
            source,
            r"if\s*\(\s*state->have_pending_peak\s*\)\s*\{[\s\S]*?"
            r"candidate_qualifies\s*&&[\s\S]*?"
            r"candidate_value\s*>\s*state->pending_peak_value[\s\S]*?"
            r"state->pending_peak_timestamp_ms = candidate_timestamp_ms;",
        )


if __name__ == "__main__":
    unittest.main()
