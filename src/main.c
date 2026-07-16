#include <zephyr/kernel.h>
#include <zephyr/autoconf.h>
#include <zephyr/logging/log.h>

#include "lcd_display.h"
#include "max30102_service.h"
#include "mqtt_service.h"
#include "time_sync.h"
#include "wifi.h"

LOG_MODULE_REGISTER(zhms, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	LOG_INF("ZHMS init ok");

	wifi_init();
	time_sync_init();
	lcd_display_init();
	max30102_service_init();
#if CONFIG_ZHMS_MQTT_ENABLED
	mqtt_service_init();
#endif
	wifi_connect();
	time_sync_enable();
	lcd_display_enable();
	max30102_service_enable();
#if CONFIG_ZHMS_MQTT_ENABLED
	mqtt_service_enable();
#endif

	while (1) {
		k_msleep(1000);
	}
}
