#define DT_DRV_COMPAT maxim_max30102

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "max30102.h"

LOG_MODULE_REGISTER(max30102, CONFIG_MAX30102_LOG_LEVEL);

static int max30102_reg_read(const struct device *dev, uint8_t reg, uint8_t *val)
{
	const struct max30102_config *cfg = dev->config;

	return i2c_reg_read_byte_dt(&cfg->i2c, reg, val);
}

static int max30102_reg_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct max30102_config *cfg = dev->config;

	return i2c_reg_write_byte_dt(&cfg->i2c, reg, val);
}

static int max30102_ack_fifo_irq(const struct device *dev)
{
	uint8_t int_sts1;
	uint8_t int_sts2;
	int ret;

	ret = max30102_reg_read(dev, MAX30102_REG_INT_STS1, &int_sts1);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_read(dev, MAX30102_REG_INT_STS2, &int_sts2);
	if (ret < 0) {
		return ret;
	}

	ARG_UNUSED(int_sts1);
	ARG_UNUSED(int_sts2);

	return 0;
}

static void max30102_irq_handler(const struct device *port,
				 struct gpio_callback *cb,
				 gpio_port_pins_t pins)
{
	struct max30102_data *data =
		CONTAINER_OF(cb, struct max30102_data, irq_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	if (atomic_cas(&data->data_ready, 0, 1)) {
		k_sem_give(&data->data_ready_sem);
	}
}

int max30102_wait_data_ready(const struct device *dev, k_timeout_t timeout)
{
	struct max30102_data *data = dev->data;
	int ret;

	if (atomic_cas(&data->data_ready, 1, 0)) {
		(void)k_sem_take(&data->data_ready_sem, K_NO_WAIT);
		return 0;
	}

	ret = k_sem_take(&data->data_ready_sem, timeout);
	if (ret == 0) {
		atomic_set(&data->data_ready, 0);
	}

	return ret;
}

static int max30102_reset(const struct device *dev)
{
	uint8_t mode_cfg;
	int ret;
	int retries = 100;

	ret = max30102_reg_write(dev, MAX30102_REG_MODE_CONFIG,
				 MAX30102_MODE_CFG_RESET_MASK);
	if (ret < 0) {
		return ret;
	}

	do {
		ret = max30102_reg_read(dev, MAX30102_REG_MODE_CONFIG, &mode_cfg);
		if (ret < 0) {
			return ret;
		}
	} while (((mode_cfg & MAX30102_MODE_CFG_RESET_MASK) != 0U) &&
		 (--retries > 0));

	if ((mode_cfg & MAX30102_MODE_CFG_RESET_MASK) != 0U) {
		return -ETIMEDOUT;
	}

	return 0;
}

static bool max30102_fifo_has_data(uint8_t wr_ptr, uint8_t rd_ptr,
				   uint8_t ovf_counter, uint8_t *level,
				   bool *full)
{
	*level = (wr_ptr - rd_ptr) & MAX30102_FIFO_PTR_MASK;
	*full = false;

	if (ovf_counter != 0U) {
		*full = true;
		if (*level == 0U) {
			*level = 32U;
		}
	}

	return (*level != 0U) || *full;
}

static int max30102_fifo_state_get(const struct device *dev, uint8_t *wr_ptr,
				   uint8_t *ovf_counter, uint8_t *rd_ptr,
				   uint8_t *level, bool *full)
{
	int ret;

	ret = max30102_reg_read(dev, MAX30102_REG_FIFO_WR_PTR, wr_ptr);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_read(dev, MAX30102_REG_OVF_COUNTER, ovf_counter);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_read(dev, MAX30102_REG_FIFO_RD_PTR, rd_ptr);
	if (ret < 0) {
		return ret;
	}

	(void)max30102_fifo_has_data(*wr_ptr, *rd_ptr, *ovf_counter, level, full);

	return 0;
}

static void max30102_log_diagnostics(uint8_t fifo_level, bool fifo_full,
				     uint8_t int_sts1, uint8_t int_sts2,
				     uint8_t wr_ptr, uint8_t ovf_counter,
				     uint8_t rd_ptr, uint8_t fifo_cfg,
				     uint8_t mode_cfg, uint8_t spo2_cfg,
				     uint8_t led1_pa, uint8_t led2_pa)
{
	LOG_WRN("MAX30102 diag: fifo_level=%u full=%u int1=0x%02x int2=0x%02x wr=0x%02x ovf=0x%02x rd=0x%02x fifo=0x%02x mode=0x%02x spo2=0x%02x led1=0x%02x led2=0x%02x",
		fifo_level, fifo_full, int_sts1, int_sts2, wr_ptr,
		ovf_counter, rd_ptr, fifo_cfg, mode_cfg, spo2_cfg, led1_pa,
		led2_pa);
}

int max30102_read_fifo_batch(const struct device *dev,
			     struct max30102_fifo_sample *samples,
			     size_t max_samples,
			     size_t *sample_count,
			     bool *fifo_overflowed)
{
	const struct max30102_config *cfg = dev->config;
	uint8_t wr_ptr;
	uint8_t ovf_counter;
	uint8_t rd_ptr;
	uint8_t fifo_level;
	bool fifo_full;
	uint8_t raw[MAX30102_FIFO_SAMPLE_BYTES];
	int ret;

	if ((samples == NULL) || (sample_count == NULL) || (fifo_overflowed == NULL)) {
		return -EINVAL;
	}

	*sample_count = 0U;
	*fifo_overflowed = false;

	ret = max30102_fifo_state_get(dev, &wr_ptr, &ovf_counter, &rd_ptr,
				      &fifo_level, &fifo_full);
	if (ret < 0) {
		return ret;
	}

	if (!max30102_fifo_has_data(wr_ptr, rd_ptr, ovf_counter, &fifo_level,
				    &fifo_full)) {
		return -EAGAIN;
	}

	ret = max30102_ack_fifo_irq(dev);
	if (ret < 0) {
		return ret;
	}

	if (fifo_full) {
		*fifo_overflowed = true;
	}

	while ((*sample_count < max_samples) && (fifo_level > 0U)) {
		ret = i2c_burst_read_dt(&cfg->i2c, MAX30102_REG_FIFO_DATA, raw,
					sizeof(raw));
		if (ret < 0) {
			return -EIO;
		}

		samples[*sample_count].red =
			((((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) |
			  raw[2]) &
			 MAX30102_FIFO_DATA_MASK);
		samples[*sample_count].ir =
			((((uint32_t)raw[3] << 16) | ((uint32_t)raw[4] << 8) |
			  raw[5]) &
			 MAX30102_FIFO_DATA_MASK);
		(*sample_count)++;
		fifo_level--;
	}

	if (fifo_full || (ovf_counter != 0U)) {
		ret = max30102_reg_write(dev, MAX30102_REG_OVF_COUNTER, 0x00);
		if (ret < 0) {
			LOG_WRN("Failed to clear overflow counter (%d)", ret);
		}
	}

	return 0;
}

static int max30102_configure(const struct device *dev)
{
	const struct max30102_config *cfg = dev->config;
	int ret;

	ret = max30102_reg_write(dev, MAX30102_REG_FIFO_WR_PTR, 0x00);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_write(dev, MAX30102_REG_OVF_COUNTER, 0x00);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_write(dev, MAX30102_REG_FIFO_RD_PTR, 0x00);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_write(dev, MAX30102_REG_FIFO_CONFIG, cfg->fifo_cfg);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_write(dev, MAX30102_REG_MODE_CONFIG, cfg->mode_cfg);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_write(dev, MAX30102_REG_SPO2_CONFIG, cfg->spo2_cfg);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_write(dev, MAX30102_REG_LED1_PA, cfg->led_pa[0]);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_write(dev, MAX30102_REG_LED2_PA, cfg->led_pa[1]);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_write(dev, MAX30102_REG_INT_EN1,
				 MAX30102_INT_ENABLE1_A_FULL_EN |
				 MAX30102_INT_ENABLE1_PPG_RDY_EN);
	if (ret < 0) {
		return ret;
	}

	ret = max30102_reg_write(dev, MAX30102_REG_INT_EN2, 0x00);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int max30102_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct max30102_data *data = dev->data;
	const struct max30102_config *cfg = dev->config;
	uint8_t raw[MAX30102_FIFO_SAMPLE_BYTES];
	uint8_t int_sts1 = 0U;
	uint8_t int_sts2 = 0U;
	uint8_t wr_ptr;
	uint8_t ovf_counter;
	uint8_t rd_ptr;
	uint8_t fifo_level;
	uint8_t fifo_cfg = 0U;
	uint8_t mode_cfg = 0U;
	uint8_t spo2_cfg = 0U;
	uint8_t led1_pa = 0U;
	uint8_t led2_pa = 0U;
	bool fifo_full;
	uint32_t red;
	uint32_t ir;
	int ret;

	if ((chan != SENSOR_CHAN_ALL) &&
	    (chan != SENSOR_CHAN_RED) &&
	    (chan != SENSOR_CHAN_IR)) {
		return -ENOTSUP;
	}

	ret = max30102_fifo_state_get(dev, &wr_ptr, &ovf_counter, &rd_ptr,
				      &fifo_level, &fifo_full);
	if (ret < 0) {
		LOG_ERR("Failed to read FIFO level (%d)", ret);
		return ret;
	}

	if (!max30102_fifo_has_data(wr_ptr, rd_ptr, ovf_counter, &fifo_level,
				    &fifo_full)) {
		data->empty_fetch_count++;
		if ((data->empty_fetch_count == 50U) ||
		    ((data->empty_fetch_count % 50U) == 0U)) {
			(void)max30102_reg_read(dev, MAX30102_REG_INT_STS1,
						&int_sts1);
			(void)max30102_reg_read(dev, MAX30102_REG_INT_STS2,
						&int_sts2);
			(void)max30102_reg_read(dev, MAX30102_REG_FIFO_CONFIG,
						&fifo_cfg);
			(void)max30102_reg_read(dev, MAX30102_REG_MODE_CONFIG,
						&mode_cfg);
			(void)max30102_reg_read(dev, MAX30102_REG_SPO2_CONFIG,
						&spo2_cfg);
			(void)max30102_reg_read(dev, MAX30102_REG_LED1_PA,
						&led1_pa);
			(void)max30102_reg_read(dev, MAX30102_REG_LED2_PA,
						&led2_pa);
			max30102_log_diagnostics(fifo_level, fifo_full, int_sts1,
						 int_sts2, wr_ptr,
						 ovf_counter, rd_ptr,
						 fifo_cfg, mode_cfg,
						 spo2_cfg, led1_pa,
						 led2_pa);
		}
		return -EAGAIN;
	}

	data->empty_fetch_count = 0U;

	ret = i2c_burst_read_dt(&cfg->i2c, MAX30102_REG_FIFO_DATA, raw,
				sizeof(raw));
	if (ret < 0) {
		LOG_ERR("Failed to read FIFO (%d)", ret);
		return ret;
	}

	red = ((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2];
	ir = ((uint32_t)raw[3] << 16) | ((uint32_t)raw[4] << 8) | raw[5];

	data->red = red & MAX30102_FIFO_DATA_MASK;
	data->ir = ir & MAX30102_FIFO_DATA_MASK;

	if (fifo_full || (ovf_counter != 0U)) {
		ret = max30102_reg_write(dev, MAX30102_REG_OVF_COUNTER, 0x00);
		if (ret < 0) {
			LOG_WRN("Failed to clear overflow counter (%d)", ret);
		}
	}

	return 0;
}

static int max30102_channel_get(const struct device *dev,
				enum sensor_channel chan,
				struct sensor_value *val)
{
	const struct max30102_data *data = dev->data;

	val->val2 = 0;

	switch (chan) {
	case SENSOR_CHAN_RED:
		val->val1 = (int32_t)data->red;
		return 0;
	case SENSOR_CHAN_IR:
		val->val1 = (int32_t)data->ir;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static DEVICE_API(sensor, max30102_driver_api) = {
	.sample_fetch = max30102_sample_fetch,
	.channel_get = max30102_channel_get,
};

static int max30102_init(const struct device *dev)
{
	struct max30102_data *data = dev->data;
	const struct max30102_config *cfg = dev->config;
	uint8_t part_id;
	int ret;

	data->red = 0U;
	data->ir = 0U;
	data->empty_fetch_count = 0U;
	k_sem_init(&data->data_ready_sem, 0, 1);
	atomic_set(&data->data_ready, 0);

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	ret = max30102_reg_read(dev, MAX30102_REG_PART_ID, &part_id);
	if (ret < 0) {
		LOG_ERR("Failed to read part id (%d)", ret);
		return ret;
	}

	if (part_id != MAX30102_PART_ID) {
		LOG_ERR("Unexpected part id 0x%02x", part_id);
		return -EIO;
	}

	ret = max30102_reset(dev);
	if (ret < 0) {
		LOG_ERR("Reset failed (%d)", ret);
		return ret;
	}

	ret = max30102_configure(dev);
	if (ret < 0) {
		LOG_ERR("Configuration failed (%d)", ret);
		return ret;
	}

	if (cfg->irq_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->irq_gpio)) {
			LOG_ERR("IRQ GPIO not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("Failed to configure IRQ GPIO (%d)", ret);
			return ret;
		}

		gpio_init_callback(&data->irq_cb, max30102_irq_handler,
				   BIT(cfg->irq_gpio.pin));
		ret = gpio_add_callback(cfg->irq_gpio.port, &data->irq_cb);
		if (ret < 0) {
			LOG_ERR("Failed to add IRQ GPIO callback (%d)", ret);
			return ret;
		}

		ret = gpio_pin_interrupt_configure_dt(&cfg->irq_gpio,
						      GPIO_INT_EDGE_TO_ACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to enable IRQ GPIO interrupt (%d)", ret);
			return ret;
		}
	}

	LOG_INF("MAX30102 init ok");

	return 0;
}

#define MAX30102_FIFO_CFG(inst) \
	((DT_INST_PROP(inst, fifo_rollover_en) ? BIT(4) : 0U) | \
	 (DT_INST_PROP(inst, fifo_watermark) & 0x0F))

#define MAX30102_ADC_RGE_BITS(inst) \
	(DT_INST_PROP(inst, adc_rge) == 2048 ? 0U : \
	 DT_INST_PROP(inst, adc_rge) == 4096 ? 1U : \
	 DT_INST_PROP(inst, adc_rge) == 8192 ? 2U : 3U)

#define MAX30102_SMP_SR_BITS(inst) \
	(DT_INST_PROP(inst, smp_sr) == 50 ? 0U : \
	 DT_INST_PROP(inst, smp_sr) == 100 ? 1U : \
	 DT_INST_PROP(inst, smp_sr) == 200 ? 2U : \
	 DT_INST_PROP(inst, smp_sr) == 400 ? 3U : \
	 DT_INST_PROP(inst, smp_sr) == 800 ? 4U : \
	 DT_INST_PROP(inst, smp_sr) == 1000 ? 5U : \
	 DT_INST_PROP(inst, smp_sr) == 1600 ? 6U : 7U)

#define MAX30102_LED_PW_BITS(inst) \
	(DT_INST_PROP(inst, led_pw) == 69 ? 0U : \
	 DT_INST_PROP(inst, led_pw) == 118 ? 1U : \
	 DT_INST_PROP(inst, led_pw) == 215 ? 2U : 3U)

#define MAX30102_SPO2_CFG(inst) \
	((MAX30102_ADC_RGE_BITS(inst) << 5) | \
	 (MAX30102_SMP_SR_BITS(inst) << 2) | \
	 MAX30102_LED_PW_BITS(inst))

#define MAX30102_DEFINE(inst)                                                   \
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, led_pa) == 2,                       \
		     "led-pa must contain exactly two entries");               \
	static struct max30102_data max30102_data_##inst;                       \
	static const struct max30102_config max30102_config_##inst = {          \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                               \
		.irq_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, irq_gpios, {}),       \
		.fifo_cfg = MAX30102_FIFO_CFG(inst),                             \
		.mode_cfg = MAX30102_MODE_SPO2,                                  \
		.spo2_cfg = MAX30102_SPO2_CFG(inst),                             \
		.led_pa = {                                                      \
			DT_INST_PROP_BY_IDX(inst, led_pa, 0),                    \
			DT_INST_PROP_BY_IDX(inst, led_pa, 1),                    \
		},                                                               \
	};                                                                       \
	SENSOR_DEVICE_DT_INST_DEFINE(inst,                                         \
				     max30102_init,                                  \
				     NULL,                                           \
				     &max30102_data_##inst,                          \
				     &max30102_config_##inst,                        \
				     POST_KERNEL,                                    \
				     CONFIG_SENSOR_INIT_PRIORITY,                    \
				     &max30102_driver_api)

DT_INST_FOREACH_STATUS_OKAY(MAX30102_DEFINE);
