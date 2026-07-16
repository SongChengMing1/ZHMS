from pathlib import Path
import unittest


class Max30102DriverSourceContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = Path(__file__).resolve().parents[3]

    def test_driver_exposes_irq_fifo_batch_contract(self) -> None:
        header = (
            self.repo_root / "drivers/sensor/max30102/max30102.h"
        ).read_text(encoding="utf-8")
        source = (
            self.repo_root / "drivers/sensor/max30102/max30102.c"
        ).read_text(encoding="utf-8")
        kconfig = (
            self.repo_root / "drivers/sensor/max30102/Kconfig"
        ).read_text(encoding="utf-8")
        binding = (
            self.repo_root / "dts/bindings/sensor/maxim,max30102.yaml"
        ).read_text(encoding="utf-8")
        acquisition = (
            self.repo_root / "src/services/max30102_acquisition.c"
        ).read_text(encoding="utf-8")

        self.assertIn("struct max30102_fifo_sample", header)
        self.assertIn("struct gpio_callback irq_cb;", header)
        self.assertIn("struct k_sem data_ready_sem;", header)
        self.assertIn("atomic_t data_ready;", header)
        self.assertIn("int max30102_wait_data_ready(", header)
        self.assertIn("int max30102_read_fifo_batch(", header)
        self.assertIn("select GPIO", kconfig)
        self.assertIn("IRQ-assisted FIFO", kconfig)
        self.assertIn("draining for the application service", kconfig)
        self.assertIn("IRQ-capable FIFO runtime", binding)
        self.assertIn("gpio_init_callback", source)
        self.assertIn("gpio_add_callback", source)
        self.assertIn("max30102_irq_handler", source)
        self.assertRegex(
            source,
            r"if\s*\(\s*atomic_cas\(&data->data_ready,\s*0,\s*1\)\s*\)\s*\{\s*"
            r"k_sem_give\(&data->data_ready_sem\);\s*\}",
        )
        self.assertRegex(
            source,
            r"if\s*\(\s*atomic_cas\(&data->data_ready,\s*1,\s*0\)\s*\)\s*\{\s*"
            r"\(void\)k_sem_take\(&data->data_ready_sem,\s*K_NO_WAIT\);\s*"
            r"return\s+0;\s*\}",
        )
        self.assertIn("k_sem_take(&data->data_ready_sem, timeout)", source)
        self.assertIn("i2c_burst_read_dt(&cfg->i2c, MAX30102_REG_FIFO_DATA", source)
        self.assertIn("max30102_reg_write(dev, MAX30102_REG_INT_EN1", source)
        self.assertIn("max30102_reg_write(dev, MAX30102_REG_INT_EN2", source)
        self.assertIn("MAX30102_INT_ENABLE1_A_FULL_EN", source)
        self.assertIn("MAX30102_INT_ENABLE1_PPG_RDY_EN", source)
        self.assertIn("max30102_reg_read(dev, MAX30102_REG_INT_STS1", source)
        self.assertIn("max30102_reg_read(dev, MAX30102_REG_INT_STS2", source)
        self.assertRegex(
            source,
            r"while\s*\(\s*\(\*sample_count\s*<\s*max_samples\)\s*&&\s*\(fifo_level\s*>\s*0U\)\s*\)",
        )
        self.assertIn("return -EAGAIN;", source)
        self.assertRegex(
            source,
            r"if\s*\(\s*ovf_counter\s*!=\s*0U\s*\)\s*\{\s*\*full\s*=\s*true;\s*"
            r"if\s*\(\s*\*level\s*==\s*0U\s*\)\s*\{\s*\*level\s*=\s*32U;\s*\}\s*\}",
        )
        self.assertIn("max30102_ack_fifo_irq", source)
        self.assertIn("max30102_wait_data_ready(max30102_runtime.dev, K_MSEC(20U))", acquisition)
        self.assertIn("max30102_read_fifo_batch(", acquisition)
        self.assertIn("MAX30102_STREAM_GAP_FIFO_OVERFLOW", acquisition)
        self.assertIn("source_flags = wake_fifo_overflowed ? 1U : 0U", acquisition)
        self.assertIn("sample_count == ARRAY_SIZE(samples)", acquisition)
        self.assertNotIn("sensor_sample_fetch(", acquisition)
        self.assertNotIn("sensor_channel_get(", acquisition)


if __name__ == "__main__":
    unittest.main()
