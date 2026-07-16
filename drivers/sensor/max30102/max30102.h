#ifndef ZEPHYR_DRIVERS_SENSOR_MAX30102_MAX30102_H_
#define ZEPHYR_DRIVERS_SENSOR_MAX30102_MAX30102_H_

#include <stdbool.h>
#include <stddef.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define MAX30102_REG_INT_STS1		0x00
#define MAX30102_REG_INT_STS2		0x01
#define MAX30102_REG_FIFO_WR_PTR	0x04
#define MAX30102_REG_OVF_COUNTER	0x05
#define MAX30102_REG_FIFO_RD_PTR	0x06
#define MAX30102_REG_FIFO_DATA		0x07
#define MAX30102_REG_FIFO_CONFIG	0x08
#define MAX30102_REG_MODE_CONFIG	0x09
#define MAX30102_REG_SPO2_CONFIG	0x0A
#define MAX30102_REG_LED1_PA		0x0C
#define MAX30102_REG_LED2_PA		0x0D
#define MAX30102_REG_PILOT_PA		0x10
#define MAX30102_REG_INT_EN1		0x02
#define MAX30102_REG_INT_EN2		0x03
#define MAX30102_REG_PART_ID		0xFF

#define MAX30102_PART_ID		0x15
#define MAX30102_FIFO_SAMPLE_BYTES	6
#define MAX30102_FIFO_DATA_MASK		0x03FFFF
#define MAX30102_FIFO_PTR_MASK		0x1F
#define MAX30102_MODE_CFG_RESET_MASK	BIT(6)
#define MAX30102_MODE_SPO2		0x03
#define MAX30102_INT_ENABLE1_A_FULL_EN	BIT(7)
#define MAX30102_INT_ENABLE1_PPG_RDY_EN	BIT(6)
#define MAX30102_INT_ENABLE1_ALC_OVF_EN	BIT(5)

struct max30102_fifo_sample {
	uint32_t red;
	uint32_t ir;
};

struct max30102_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec irq_gpio;
	uint8_t fifo_cfg;
	uint8_t mode_cfg;
	uint8_t spo2_cfg;
	uint8_t led_pa[2];
};

struct max30102_data {
	uint32_t red;
	uint32_t ir;
	uint32_t empty_fetch_count;
	struct gpio_callback irq_cb;
	struct k_sem data_ready_sem;
	atomic_t data_ready;
};

int max30102_wait_data_ready(const struct device *dev, k_timeout_t timeout);
int max30102_read_fifo_batch(const struct device *dev,
			     struct max30102_fifo_sample *samples,
			     size_t max_samples,
			     size_t *sample_count,
			     bool *fifo_overflowed);

#endif /* ZEPHYR_DRIVERS_SENSOR_MAX30102_MAX30102_H_ */
