from pathlib import Path
import re
import unittest


class Max30102RealtimeServiceSourceContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = Path(__file__).resolve().parents[3]

    def test_stream_header_declares_sample_gap_and_driver_error_items(self) -> None:
        header = (self.repo_root / "src/services/max30102_stream.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("enum max30102_stream_item_type", header)
        self.assertIn("MAX30102_ITEM_SAMPLE", header)
        self.assertIn("MAX30102_ITEM_STREAM_GAP", header)
        self.assertIn("MAX30102_ITEM_DRIVER_ERROR", header)
        self.assertIn("struct max30102_raw_sample", header)
        self.assertIn("uint32_t timestamp_ms;", header)
        self.assertIn("uint16_t seq;", header)
        self.assertIn("struct max30102_stream_item", header)

    def test_service_split_declares_acquisition_and_algorithm_threads(self) -> None:
        internal_header = (
            self.repo_root / "src/services/max30102_service_internal.h"
        ).read_text(encoding="utf-8")
        service = (self.repo_root / "src/services/max30102_service.c").read_text(
            encoding="utf-8"
        )
        acquisition = (
            self.repo_root / "src/services/max30102_acquisition.c"
        ).read_text(encoding="utf-8")
        algorithm = (
            self.repo_root / "src/services/max30102_algorithm.c"
        ).read_text(encoding="utf-8")

        self.assertRegex(service, r"#define\s+MAX30102_STREAM_QUEUE_CAPACITY\s+128U")
        self.assertRegex(service, r"#define\s+MAX30102_ACQUISITION_PRIORITY\s+4")
        self.assertRegex(service, r"#define\s+MAX30102_ALGORITHM_PRIORITY\s+7")
        self.assertIn("struct max30102_service_runtime", internal_header)
        self.assertIn("max30102_stream_queue_push", service)
        self.assertIn("max30102_stream_queue_pop", service)
        self.assertRegex(
            service,
            r"if\s*\(\s*queue->count\s*==\s*1U\s*\)\s*\{\s*"
            r"wake_stream_thread\s*=\s*true;\s*\}\s*"
            r"k_spin_unlock\(&queue->lock,\s*key\);\s*"
            r"if\s*\(\s*wake_stream_thread\s*\)\s*\{\s*"
            r"k_sem_give\(&max30102_runtime\.stream_sem\);\s*\}",
        )
        self.assertIn("void max30102_acquisition_thread", acquisition)
        self.assertIn("void max30102_algorithm_thread", algorithm)

    def test_acquisition_owns_sampling_and_monotonic_timestamp_generation(self) -> None:
        acquisition = (
            self.repo_root / "src/services/max30102_acquisition.c"
        ).read_text(encoding="utf-8")

        self.assertIn("max30102_wait_data_ready(max30102_runtime.dev, K_MSEC(20U))", acquisition)
        self.assertIn("max30102_read_fifo_batch(", acquisition)
        self.assertIn("struct max30102_fifo_sample samples[16];", acquisition)
        self.assertIn("MAX30102_STREAM_GAP_FIFO_OVERFLOW", acquisition)
        self.assertIn("max30102_runtime.irq_wakeup_count++", acquisition)
        self.assertIn("max30102_runtime.fifo_overflow_count++", acquisition)
        self.assertRegex(
            acquisition,
            r"item\.sample\.timestamp_ms\s*=\s*max30102_acquisition_next_timestamp_ms\(\);",
        )
        self.assertRegex(acquisition, r"item\.sample\.seq\s*=\s*max30102_runtime\.next_seq\+\+;")
        self.assertIn("max30102_stream_queue_push(&item)", acquisition)
        self.assertIn("source_flags = wake_fifo_overflowed ? 1U : 0U", acquisition)
        self.assertIn("while (1)", acquisition)
        self.assertNotIn("sensor_sample_fetch(", acquisition)
        self.assertNotIn("sensor_channel_get(", acquisition)
        self.assertNotIn("ppg_preprocess_apply", acquisition)
        self.assertNotIn("health_metrics_push", acquisition)

    def test_algorithm_owns_task4_task5_processing_chain(self) -> None:
        algorithm = (
            self.repo_root / "src/services/max30102_algorithm.c"
        ).read_text(encoding="utf-8")

        self.assertIn("ppg_preprocess_apply", algorithm)
        self.assertIn("max30102_update_finger_contact_state", algorithm)
        self.assertIn("ppg_peak_detect_push", algorithm)
        self.assertIn("health_metrics_push", algorithm)
        self.assertIn("health_metrics_advance", algorithm)
        self.assertIn("max30102_print_metrics_summary", algorithm)
        self.assertIn("max30102_stream_queue_pop(&item)", algorithm)
        self.assertIn("k_sem_take(&max30102_runtime.stream_sem", algorithm)
        self.assertIn("summary_due", algorithm)
        self.assertIn('printk("%s,%s,%s\\n"', algorithm)
        self.assertRegex(
            algorithm,
            r"if\s*\(\s*max30102_finger_state\s*==\s*MAX30102_FINGER_STATE_ACTIVE\s*\)\s*\{[\s\S]*?"
            r"ppg_peak_detect_push\(&ppg_peak_state,\s*timestamp_ms,\s*"
            r"filtered\.filtered_ir,\s*&peak\);[\s\S]*?"
            r"if\s*\(\s*peak\.peak_detected\s*&&\s*peak\.ibi_valid\s*&&\s*peak\.ibi_ms\s*>\s*0U\s*\)\s*\{[\s\S]*?"
            r"health_metrics_push\(&health_metrics_state,\s*timestamp_ms,\s*peak\.ibi_ms,\s*&metrics\);[\s\S]*?"
            r"\}\s*else\s*\{[\s\S]*?"
            r"health_metrics_advance\(&health_metrics_state,\s*timestamp_ms,\s*&metrics\);",
        )
        self.assertRegex(
            algorithm,
            r"health_metrics_push\(&health_metrics_state,\s*timestamp_ms,\s*peak\.ibi_ms,\s*&metrics\);",
        )
        self.assertNotIn("sensor_sample_fetch", algorithm)
        self.assertNotIn("sensor_channel_get", algorithm)

    def test_gap_items_and_telemetry_are_handled_in_service_and_algorithm(self) -> None:
        public_header = (self.repo_root / "src/services/max30102_service.h").read_text(
            encoding="utf-8"
        )
        internal_header = (
            self.repo_root / "src/services/max30102_service_internal.h"
        ).read_text(encoding="utf-8")
        acquisition = (
            self.repo_root / "src/services/max30102_acquisition.c"
        ).read_text(encoding="utf-8")
        algorithm = (
            self.repo_root / "src/services/max30102_algorithm.c"
        ).read_text(encoding="utf-8")

        self.assertIn("struct max30102_service_telemetry", public_header)
        self.assertIn("void max30102_service_get_telemetry(", public_header)
        self.assertIn("uint32_t queue_drop_count;", internal_header)
        self.assertIn("uint32_t fifo_overflow_count;", internal_header)
        self.assertIn("uint32_t driver_error_count;", internal_header)
        self.assertIn("uint32_t irq_wakeup_count;", internal_header)
        self.assertIn("uint32_t max_queue_depth_seen;", internal_header)
        self.assertIn("uint16_t last_stream_gap_reason;", internal_header)
        self.assertIn("MAX30102_STREAM_GAP_QUEUE_OVERFLOW", acquisition)
        self.assertIn("MAX30102_STREAM_GAP_DRIVER_ERROR", acquisition)
        self.assertIn("MAX30102_STREAM_GAP_FIFO_OVERFLOW", acquisition)
        self.assertIn("max30102_try_flush_pending_stream_gap", acquisition)
        self.assertIn("max30102_queue_pending_stream_gap", acquisition)
        self.assertIn("max30102_consume_dropped_sample_timestamps", acquisition)
        self.assertIn("pending_stream_gap", acquisition)
        self.assertIn("max30102_runtime.pending_stream_gap.active", acquisition)
        self.assertIn("max30102_runtime.pending_stream_gap.dropped_samples", acquisition)
        self.assertIn("max30102_runtime.driver_error_count++", acquisition)
        self.assertIn("max30102_runtime.queue_drop_count += dropped_samples;", acquisition)
        self.assertIn("wake_fifo_overflowed = true;", acquisition)
        self.assertNotIn("max30102_stream_queue_pop(", acquisition)
        self.assertRegex(
            acquisition,
            r"if\s*\(\s*max30102_runtime\.pending_stream_gap\.active\s*\)\s*\{\s*"
            r"if\s*\(\s*!\s*max30102_try_flush_pending_stream_gap\(\)\s*\)\s*\{\s*"
            r"k_sleep\(K_MSEC\(MAX30102_ACQUISITION_POLL_PERIOD_MS\)\);\s*"
            r"continue;\s*\}\s*"
            r"continue;\s*\}\s*[\s\S]*?"
            r"if\s*\(\s*max30102_wait_data_ready\(max30102_runtime\.dev,\s*"
            r"K_MSEC\(20U\)\)\s*==\s*0\s*\)\s*\{\s*"
            r"max30102_runtime\.irq_wakeup_count\+\+;\s*\}\s*[\s\S]*?"
            r"ret\s*=\s*max30102_read_fifo_batch\(",
        )
        self.assertIn("do {", acquisition)
        self.assertIn("sample_count = 0U;", acquisition)
        self.assertIn("ret = max30102_read_fifo_batch(", acquisition)
        self.assertIn("while ((ret == 0) && (sample_count == ARRAY_SIZE(samples)));", acquisition)
        self.assertIn("const size_t dropped_samples = sample_count - i;", acquisition)
        self.assertIn("max30102_emit_stream_gap(item.sample.timestamp_ms,", acquisition)
        self.assertIn("if (dropped_samples > 1U)", acquisition)
        self.assertRegex(
            acquisition,
            r"max30102_consume_dropped_sample_timestamps\(\s*"
            r"dropped_samples - 1U\s*\);",
        )
        self.assertRegex(
            acquisition,
            r"if\s*\(\s*ret\s*==\s*-EAGAIN\s*\)\s*\{\s*"
            r"if\s*\(\s*\+\+max30102_empty_fetch_count\s*>=\s*"
            r"MAX30102_ACQUISITION_EMPTY_FETCH_THRESHOLD\s*\)\s*\{\s*"
            r"max30102_runtime\.driver_error_count\+\+;\s*"
            r"max30102_queue_pending_stream_gap\(\s*"
            r"MAX30102_STREAM_GAP_DRIVER_ERROR,\s*1U\s*\);\s*"
            r"max30102_empty_fetch_count\s*=\s*0U;\s*\}\s*"
            r"k_sleep\(K_MSEC\(MAX30102_ACQUISITION_POLL_PERIOD_MS\)\);\s*"
            r"break;\s*\}",
        )
        self.assertIn("max30102_algorithm_handle_stream_gap", algorithm)
        self.assertIn("health_metrics_reset", algorithm)
        self.assertIn("ppg_peak_detect_reset", algorithm)
        self.assertIn("item.type == MAX30102_ITEM_STREAM_GAP", algorithm)
        self.assertIn("max30102_runtime.last_stream_gap_reason", acquisition)
        self.assertIn("max30102_runtime.queue_drop_count", acquisition)
        self.assertIn("max30102_runtime.driver_error_count", acquisition)


if __name__ == "__main__":
    unittest.main()
