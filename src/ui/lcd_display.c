#include <string.h>
#include <time.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "lcd_display.h"
#include "ui_pages.h"

LOG_MODULE_REGISTER(lcd_display, CONFIG_LOG_DEFAULT_LEVEL);

#define DISPLAY_SLEEP_MIN_MS 10U
#define DISPLAY_SLEEP_MAX_MS 50U
#define DISPLAY_STACK_SIZE 6144
#define DISPLAY_PRIORITY   6
#define DISPLAY_WAIT_PERIOD_MS 100
#define DISPLAY_ALL_EVENTS (LCD_DISPLAY_EVENT_CLOCK | LCD_DISPLAY_EVENT_WIFI | \
			    LCD_DISPLAY_EVENT_HEART | LCD_DISPLAY_EVENT_PAGE)
#define DISPLAY_CLOCK_REFRESH_IDLE_MS 60000U
#define CLOCK_UTC_OFFSET_SECONDS (8 * 60 * 60)

static const struct device *const display_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static atomic_t display_initialized = ATOMIC_INIT(0);
static atomic_t display_enabled = ATOMIC_INIT(0);
static struct k_event display_events;

struct display_runtime {
	struct ui_pages_view view;
	bool clock_prepared;
	bool wifi_prepared;
	bool heart_prepared;
	int current_page;
	int64_t next_clock_refresh_ms;
};

static uint32_t clamp_display_sleep_ms(uint32_t suggested_ms)
{
	if (suggested_ms == LV_NO_TIMER_READY || suggested_ms > DISPLAY_SLEEP_MAX_MS) {
		return DISPLAY_SLEEP_MAX_MS;
	}

	if (suggested_ms < DISPLAY_SLEEP_MIN_MS) {
		return DISPLAY_SLEEP_MIN_MS;
	}

	return suggested_ms;
}

static bool clock_view_equals(const struct ui_pages_clock_view *a,
			      const struct ui_pages_clock_view *b)
{
	return strcmp(a->time_text, b->time_text) == 0 &&
	       strcmp(a->date_text, b->date_text) == 0 &&
	       a->sync_state == b->sync_state;
}

static bool wifi_view_equals(const struct ui_pages_wifi_view *a,
			     const struct ui_pages_wifi_view *b)
{
	return a->state == b->state &&
	       strcmp(a->ssid_text, b->ssid_text) == 0 &&
	       strcmp(a->state_text, b->state_text) == 0 &&
	       strcmp(a->ip_text, b->ip_text) == 0 &&
	       strcmp(a->rssi_text, b->rssi_text) == 0;
}

static bool heart_view_equals(const struct ui_pages_heart_view *a,
			      const struct ui_pages_heart_view *b)
{
	return strcmp(a->value_text, b->value_text) == 0 &&
	       strcmp(a->status_text, b->status_text) == 0 &&
	       a->metrics_ready == b->metrics_ready;
}

static int64_t compute_next_clock_refresh_ms(void)
{
	time_t now = time(NULL);
	uint32_t seconds_until_next_minute;

	if (now == (time_t)-1) {
		return k_uptime_get() + DISPLAY_CLOCK_REFRESH_IDLE_MS;
	}

	now += CLOCK_UTC_OFFSET_SECONDS;
	seconds_until_next_minute = 60U - ((uint32_t)now % 60U);
	if (seconds_until_next_minute == 0U) {
		seconds_until_next_minute = 60U;
	}

	return k_uptime_get() + ((int64_t)seconds_until_next_minute * MSEC_PER_SEC);
}

static bool update_clock_model(struct display_runtime *runtime)
{
	struct ui_pages_clock_view next_view;

	ui_pages_prepare_clock(&next_view);
	next_view.sync_state = time_sync_get_state();
	if (!runtime->clock_prepared ||
	    !clock_view_equals(&runtime->view.clock, &next_view)) {
		runtime->view.clock = next_view;
		runtime->clock_prepared = true;
		return true;
	}

	return false;
}

static bool update_wifi_model(struct display_runtime *runtime)
{
	struct ui_pages_wifi_view next_view;

	ui_pages_prepare_wifi(&next_view);
	if (!runtime->wifi_prepared ||
	    !wifi_view_equals(&runtime->view.wifi, &next_view)) {
		runtime->view.wifi = next_view;
		runtime->wifi_prepared = true;
		return true;
	}

	return false;
}

static bool update_heart_model(struct display_runtime *runtime)
{
	struct ui_pages_heart_view next_view;

	ui_pages_prepare_heart(&next_view);
	if (!runtime->heart_prepared ||
	    !heart_view_equals(&runtime->view.heart, &next_view)) {
		runtime->view.heart = next_view;
		runtime->heart_prepared = true;
		return true;
	}

	return false;
}

static bool update_current_page_model(struct display_runtime *runtime)
{
	switch (runtime->current_page) {
	case UI_PAGE_CLOCK:
		return update_clock_model(runtime);
	case UI_PAGE_WIFI:
		return update_wifi_model(runtime);
	case UI_PAGE_HEART:
		return update_heart_model(runtime);
	default:
		return false;
	}
}

static void render_current_page(const struct display_runtime *runtime)
{
	switch (runtime->current_page) {
	case UI_PAGE_CLOCK:
		ui_pages_render_clock(&runtime->view.clock);
		break;
	case UI_PAGE_WIFI:
		ui_pages_render_wifi(&runtime->view.wifi);
		break;
	case UI_PAGE_HEART:
		ui_pages_render_heart(&runtime->view.heart);
		break;
	default:
		break;
	}
}

static bool apply_requested_page(struct display_runtime *runtime,
				 uint32_t *pending_dirty)
{
	int requested_page;
	bool current_page_changed;

	if (!ui_pages_consume_page_request(&requested_page)) {
		return false;
	}

	runtime->current_page = requested_page;
	*pending_dirty |= LCD_DISPLAY_EVENT_PAGE;
	current_page_changed = update_current_page_model(runtime);
	switch (runtime->current_page) {
	case UI_PAGE_CLOCK:
		runtime->next_clock_refresh_ms = compute_next_clock_refresh_ms();
		*pending_dirty |= LCD_DISPLAY_EVENT_CLOCK;
		break;
	case UI_PAGE_WIFI:
		runtime->next_clock_refresh_ms = INT64_MAX;
		*pending_dirty |= LCD_DISPLAY_EVENT_WIFI;
		break;
	case UI_PAGE_HEART:
		runtime->next_clock_refresh_ms = INT64_MAX;
		*pending_dirty |= LCD_DISPLAY_EVENT_HEART;
		break;
	default:
		break;
	}

	if (current_page_changed) {
		/* Model already refreshed for the target page. */
	}

	return true;
}

static void lcd_display_thread(void *arg1, void *arg2, void *arg3)
{
	struct display_runtime runtime = {
		.current_page = UI_PAGE_CLOCK,
		.next_clock_refresh_ms = INT64_MAX,
	};
	uint32_t pending_dirty = 0U;
	uint32_t lvgl_wait_ms = DISPLAY_SLEEP_MAX_MS;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	k_thread_name_set(k_current_get(), "lcd_display");
	LOG_INF("LCD display thread started");

	while (!atomic_get(&display_enabled)) {
		k_msleep(DISPLAY_WAIT_PERIOD_MS);
	}

	lvgl_lock();
	if (ui_pages_create(lv_scr_act()) < 0) {
		lvgl_unlock();
		LOG_ERR("Failed to create UI pages");
		return;
	}
	lvgl_unlock();

	display_blanking_off(display_dev);

	runtime.current_page = ui_pages_get_current_page();
	(void)update_clock_model(&runtime);
	(void)update_wifi_model(&runtime);
	(void)update_heart_model(&runtime);
	if (runtime.current_page == UI_PAGE_CLOCK) {
		runtime.next_clock_refresh_ms = compute_next_clock_refresh_ms();
	}

	pending_dirty = LCD_DISPLAY_EVENT_PAGE;
	switch (runtime.current_page) {
	case UI_PAGE_CLOCK:
		pending_dirty |= LCD_DISPLAY_EVENT_CLOCK;
		break;
	case UI_PAGE_WIFI:
		pending_dirty |= LCD_DISPLAY_EVENT_WIFI;
		break;
	case UI_PAGE_HEART:
		pending_dirty |= LCD_DISPLAY_EVENT_HEART;
		break;
	default:
		break;
	}

	lvgl_lock();
	ui_pages_show_page(runtime.current_page);
	render_current_page(&runtime);
	lvgl_wait_ms = clamp_display_sleep_ms(lv_timer_handler());
	lvgl_unlock();
	pending_dirty = 0U;

	LOG_INF("LCD display service enabled, entering main loop");

	while (1) {
		int64_t now_ms = k_uptime_get();
		int64_t next_due_ms = now_ms + lvgl_wait_ms;
		uint32_t wait_ms;
		uint32_t events;

		(void)apply_requested_page(&runtime, &pending_dirty);

		if (runtime.current_page == UI_PAGE_CLOCK &&
		    runtime.next_clock_refresh_ms < next_due_ms) {
			next_due_ms = runtime.next_clock_refresh_ms;
		}

		if (next_due_ms <= now_ms) {
			wait_ms = 0U;
		} else {
			wait_ms = clamp_display_sleep_ms((uint32_t)(next_due_ms - now_ms));
		}

		events = k_event_wait(&display_events, DISPLAY_ALL_EVENTS, true,
				       K_MSEC(wait_ms));

		if (events == 0U && runtime.current_page == UI_PAGE_CLOCK &&
		    k_uptime_get() >= runtime.next_clock_refresh_ms) {
			if (update_clock_model(&runtime)) {
				pending_dirty |= LCD_DISPLAY_EVENT_CLOCK;
			}
			runtime.next_clock_refresh_ms = compute_next_clock_refresh_ms();
		}

		if ((events & LCD_DISPLAY_EVENT_PAGE) != 0U) {
			(void)apply_requested_page(&runtime, &pending_dirty);
		}

		if ((events & LCD_DISPLAY_EVENT_CLOCK) != 0U) {
			if (update_clock_model(&runtime) &&
			    runtime.current_page == UI_PAGE_CLOCK) {
				pending_dirty |= LCD_DISPLAY_EVENT_CLOCK;
			}
			if (runtime.current_page == UI_PAGE_CLOCK) {
				runtime.next_clock_refresh_ms = compute_next_clock_refresh_ms();
			}
		}

		if ((events & LCD_DISPLAY_EVENT_WIFI) != 0U) {
			if (update_wifi_model(&runtime) &&
			    runtime.current_page == UI_PAGE_WIFI) {
				pending_dirty |= LCD_DISPLAY_EVENT_WIFI;
			}
		}

		if ((events & LCD_DISPLAY_EVENT_HEART) != 0U) {
			if (update_heart_model(&runtime) &&
			    runtime.current_page == UI_PAGE_HEART) {
				pending_dirty |= LCD_DISPLAY_EVENT_HEART;
			}
		}

		lvgl_lock();
		if ((pending_dirty & LCD_DISPLAY_EVENT_PAGE) != 0U) {
			ui_pages_show_page(runtime.current_page);
		}
		if ((pending_dirty & LCD_DISPLAY_EVENT_PAGE) != 0U ||
		    ((pending_dirty & LCD_DISPLAY_EVENT_CLOCK) != 0U &&
		     runtime.current_page == UI_PAGE_CLOCK) ||
		    ((pending_dirty & LCD_DISPLAY_EVENT_WIFI) != 0U &&
		     runtime.current_page == UI_PAGE_WIFI) ||
		    ((pending_dirty & LCD_DISPLAY_EVENT_HEART) != 0U &&
		     runtime.current_page == UI_PAGE_HEART)) {
			render_current_page(&runtime);
		}
		pending_dirty = 0U;
		lvgl_wait_ms = clamp_display_sleep_ms(lv_timer_handler());
		lvgl_unlock();
	}
}

K_THREAD_DEFINE(display_thread, DISPLAY_STACK_SIZE,
		lcd_display_thread, NULL, NULL, NULL,
		DISPLAY_PRIORITY, 0, 0);

void lcd_display_init(void)
{
	atomic_set(&display_initialized, 0);
	atomic_set(&display_enabled, 0);

	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device not ready");
		return;
	}

	k_event_init(&display_events);
	atomic_set(&display_initialized, 1);
	LOG_INF("LCD display service initialized");
}

void lcd_display_enable(void)
{
	if (!atomic_get(&display_initialized)) {
		LOG_ERR("LCD display not initialized");
		return;
	}

	atomic_set(&display_enabled, 1);
	lcd_display_post_event(LCD_DISPLAY_EVENT_CLOCK |
			       LCD_DISPLAY_EVENT_WIFI |
			       LCD_DISPLAY_EVENT_HEART);
	LOG_INF("LCD display service enabled");
}

void lcd_display_post_event(uint32_t events)
{
	if (!atomic_get(&display_initialized) || events == 0U) {
		return;
	}

	k_event_post(&display_events, events);
}
