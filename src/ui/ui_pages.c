#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <lvgl.h>

#include "../services/max30102_service.h"
#include "lcd_display.h"
#include "time_sync.h"
#include "wifi.h"
#include "ui_pages.h"

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 280
#define PAGE_COUNT    3
#define INDICATOR_Y   -8
#define CLOCK_UTC_OFFSET_SECONDS (8 * 60 * 60)

#define COLOR_BLACK 0x000000
#define COLOR_WHITE 0xFFFFFF
#define COLOR_GRAY  0x888888
#define COLOR_GREEN 0x00FF00
#define COLOR_YELLOW 0xFFFF00
#define COLOR_RED   0xFF0000

#define FONT_CJK_14 &lv_font_source_han_sans_sc_14_cjk
#define FONT_CJK_16 &lv_font_source_han_sans_sc_16_cjk

static lv_obj_t *pages[PAGE_COUNT];
static lv_obj_t *indicator_dots[PAGE_COUNT];
static lv_obj_t *clock_time_label;
static lv_obj_t *clock_date_label;
static lv_obj_t *clock_sync_dot;
static lv_obj_t *clock_sync_label;
static lv_obj_t *wifi_ssid_label;
static lv_obj_t *wifi_state_label;
static lv_obj_t *wifi_ip_label;
static lv_obj_t *wifi_rssi_label;
static lv_obj_t *heart_icon_label;
static lv_obj_t *heart_value_label;
static lv_obj_t *heart_status_dot;
static lv_obj_t *heart_status_label;
static lv_style_t heart_icon_style;
static int current_page;
static int pending_page = -1;

static int rssi_to_percent(int rssi)
{
	if (rssi <= -100) {
		return 0;
	}
	if (rssi >= -50) {
		return 100;
	}

	return 2 * (rssi + 100);
}

static const char *rssi_icon(int percent)
{
	if (percent <= 25) {
		return "|___";
	}
	if (percent <= 50) {
		return "||__";
	}
	if (percent <= 75) {
		return "|||_";
	}

	return "||||";
}

static bool clock_tm_now(struct tm *tm_buf)
{
	time_t now = time(NULL);

	if (now == (time_t)-1 || tm_buf == NULL) {
		return false;
	}

	now += CLOCK_UTC_OFFSET_SECONDS;

	return gmtime_r(&now, tm_buf) != NULL;
}

static void set_label_color(lv_obj_t *label, lv_color_t color)
{
	if (label == NULL) {
		return;
	}

	lv_obj_set_style_text_color(label, color, 0);
}

static void set_dot_color(lv_obj_t *dot, lv_color_t color)
{
	if (dot == NULL) {
		return;
	}

	lv_obj_set_style_bg_color(dot, color, 0);
	lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
}

static lv_obj_t *create_page(lv_obj_t *parent, int index)
{
	pages[index] = lv_obj_create(parent);

	if (pages[index] == NULL) {
		return NULL;
	}

	lv_obj_set_size(pages[index], SCREEN_WIDTH, SCREEN_HEIGHT);
	lv_obj_set_pos(pages[index], 0, 0);
	lv_obj_set_style_bg_color(pages[index], lv_color_hex(COLOR_BLACK), 0);
	lv_obj_set_style_bg_opa(pages[index], LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(pages[index], 0, 0);
	lv_obj_set_style_pad_all(pages[index], 0, 0);
	lv_obj_set_scrollbar_mode(pages[index], LV_SCROLLBAR_MODE_OFF);
	lv_obj_clear_flag(pages[index], LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(pages[index], LV_OBJ_FLAG_CLICK_FOCUSABLE);

	return pages[index];
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text,
			      const lv_font_t *font, lv_color_t color)
{
	lv_obj_t *label = lv_label_create(parent);

	if (label == NULL) {
		return NULL;
	}

	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	set_label_color(label, color);
	lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);

	return label;
}

static lv_obj_t *create_status_dot(lv_obj_t *parent)
{
	lv_obj_t *dot = lv_obj_create(parent);

	if (dot == NULL) {
		return NULL;
	}

	lv_obj_set_size(dot, 10, 10);
	lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(dot, 0, 0);
	lv_obj_set_style_pad_all(dot, 0, 0);
	set_dot_color(dot, lv_color_hex(COLOR_GRAY));
	lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

	return dot;
}

static lv_obj_t *create_heart_icon(lv_obj_t *parent)
{
	static const lv_point_precise_t heart_points[] = {
		{ 22, 34 },
		{ 14, 26 },
		{ 10, 18 },
		{ 10, 12 },
		{ 14, 8 },
		{ 20, 8 },
		{ 22, 12 },
		{ 24, 8 },
		{ 30, 8 },
		{ 34, 12 },
		{ 34, 18 },
		{ 30, 26 },
		{ 22, 34 },
	};
	static bool heart_icon_style_ready;
	lv_obj_t *heart_icon = lv_line_create(parent);

	if (heart_icon == NULL) {
		return NULL;
	}

	lv_line_set_points(heart_icon, heart_points, ARRAY_SIZE(heart_points));
	lv_obj_set_size(heart_icon, 44, 40);
	if (!heart_icon_style_ready) {
		lv_style_init(&heart_icon_style);
		lv_style_set_line_color(&heart_icon_style, lv_color_hex(COLOR_RED));
		lv_style_set_line_width(&heart_icon_style, 4);
#if defined(LV_STYLE_LINE_ROUNDED)
		lv_style_set_line_rounded(&heart_icon_style, true);
#endif
		heart_icon_style_ready = true;
	}
	lv_obj_add_style(heart_icon, &heart_icon_style, 0);
	lv_obj_clear_flag(heart_icon, LV_OBJ_FLAG_SCROLLABLE);

	return heart_icon;
}

void ui_pages_prepare_clock(struct ui_pages_clock_view *view)
{
	static const char *const weekday_text[] = {
		"周日",
		"周一",
		"周二",
		"周三",
		"周四",
		"周五",
		"周六",
	};
	struct tm tm_buf;
	char time_buf[16];
	char date_buf[32];
	const char *weekday = "周?";

	if (view == NULL) {
		return;
	}

	if (!clock_tm_now(&tm_buf)) {
		strncpy(view->time_text, "--:--", sizeof(view->time_text) - 1);
		view->time_text[sizeof(view->time_text) - 1] = '\0';
		strncpy(view->date_text, "---- -- --", sizeof(view->date_text) - 1);
		view->date_text[sizeof(view->date_text) - 1] = '\0';
		return;
	}

	snprintf(time_buf, sizeof(time_buf), "%02d:%02d",
		 tm_buf.tm_hour, tm_buf.tm_min);

	if (tm_buf.tm_wday >= 0 &&
	    tm_buf.tm_wday < (int)ARRAY_SIZE(weekday_text)) {
		weekday = weekday_text[tm_buf.tm_wday];
	}

	snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d %s",
		 tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
		 weekday);

	strncpy(view->time_text, time_buf, sizeof(view->time_text) - 1);
	view->time_text[sizeof(view->time_text) - 1] = '\0';
	strncpy(view->date_text, date_buf, sizeof(view->date_text) - 1);
	view->date_text[sizeof(view->date_text) - 1] = '\0';
}

void ui_pages_render_clock(const struct ui_pages_clock_view *view)
{
	const char *text = "未同步";
	lv_color_t color = lv_color_hex(COLOR_GRAY);

	if (view == NULL || clock_time_label == NULL || clock_date_label == NULL ||
	    clock_sync_dot == NULL || clock_sync_label == NULL) {
		return;
	}

	switch (view->sync_state) {
	case TIME_SYNC_SYNCED:
		text = "已同步";
		color = lv_color_hex(COLOR_GREEN);
		break;
	case TIME_SYNC_IN_PROGRESS:
		text = "同步中";
		color = lv_color_hex(COLOR_YELLOW);
		break;
	case TIME_SYNC_UNSYNCED:
	default:
		break;
	}

	lv_label_set_text(clock_time_label, view->time_text);
	lv_label_set_text(clock_date_label, view->date_text);
	set_dot_color(clock_sync_dot, color);
	set_label_color(clock_sync_label, color);
	lv_label_set_text(clock_sync_label, text);
}

void ui_pages_prepare_wifi(struct ui_pages_wifi_view *view)
{
	struct wifi_snapshot snapshot = {
		.state = WIFI_DISCONNECTED,
	};
	const char *state_text = "Offline";

	if (view == NULL) {
		return;
	}

	if (wifi_get_snapshot(&snapshot) < 0) {
		snapshot.state = WIFI_DISCONNECTED;
	}

	view->state = snapshot.state;
	snprintf(view->ssid_text, sizeof(view->ssid_text), "SSID: %s",
		 WIFI_SSID[0] != '\0' ? WIFI_SSID : "未配置");
	strncpy(view->ip_text, "IP: --", sizeof(view->ip_text) - 1);
	view->ip_text[sizeof(view->ip_text) - 1] = '\0';
	strncpy(view->rssi_text, "RSSI: --", sizeof(view->rssi_text) - 1);
	view->rssi_text[sizeof(view->rssi_text) - 1] = '\0';

	switch (snapshot.state) {
	case WIFI_CONNECTING:
		state_text = "Connecting...";
		break;
	case WIFI_OBTAINING_IP:
		state_text = "Waiting IP...";
		break;
	case WIFI_READY:
		state_text = "Online";
		if (snapshot.ip_valid) {
			snprintf(view->ip_text, sizeof(view->ip_text), "IP: %s",
				 snapshot.ip_addr);
		}
		if (snapshot.rssi_valid) {
			int percent = rssi_to_percent(snapshot.rssi_dbm);

			snprintf(view->rssi_text, sizeof(view->rssi_text),
				 "RSSI: %s %d%%",
				 rssi_icon(percent), percent);
		}
		break;
	case WIFI_RECONNECTING:
		state_text = "Retrying...";
		break;
	case WIFI_ERROR:
		state_text = "Error";
		break;
	case WIFI_DISCONNECTED:
	default:
		break;
	}

	snprintf(view->state_text, sizeof(view->state_text), "State: %s",
		 state_text);
}

void ui_pages_render_wifi(const struct ui_pages_wifi_view *view)
{
	lv_color_t state_color = lv_color_hex(COLOR_GRAY);

	if (view == NULL || wifi_ssid_label == NULL || wifi_state_label == NULL ||
	    wifi_ip_label == NULL || wifi_rssi_label == NULL) {
		return;
	}

	switch (view->state) {
	case WIFI_CONNECTING:
	case WIFI_OBTAINING_IP:
	case WIFI_RECONNECTING:
		state_color = lv_color_hex(COLOR_YELLOW);
		break;
	case WIFI_READY:
		state_color = lv_color_hex(COLOR_GREEN);
		break;
	case WIFI_ERROR:
		state_color = lv_color_hex(COLOR_RED);
		break;
	case WIFI_DISCONNECTED:
	default:
		break;
	}

	lv_label_set_text(wifi_ssid_label, view->ssid_text);
	lv_label_set_text(wifi_state_label, view->state_text);
	lv_label_set_text(wifi_ip_label, view->ip_text);
	lv_label_set_text(wifi_rssi_label, view->rssi_text);
	set_label_color(wifi_state_label, state_color);
}

void ui_pages_prepare_heart(struct ui_pages_heart_view *view)
{
	struct max30102_health_snapshot snapshot = {0};

	if (view == NULL) {
		return;
	}

	max30102_service_get_health_snapshot(&snapshot);
	if (snapshot.bpm_valid) {
		snprintk(view->value_text, sizeof(view->value_text), "%u",
			 snapshot.bpm);
		strncpy(view->status_text, "Monitoring",
			sizeof(view->status_text) - 1);
		view->status_text[sizeof(view->status_text) - 1] = '\0';
		view->metrics_ready = true;
		return;
	}

	strncpy(view->value_text, "---", sizeof(view->value_text) - 1);
	view->value_text[sizeof(view->value_text) - 1] = '\0';
	strncpy(view->status_text, "Sensor wait", sizeof(view->status_text) - 1);
	view->status_text[sizeof(view->status_text) - 1] = '\0';
	view->metrics_ready = false;
}

void ui_pages_render_heart(const struct ui_pages_heart_view *view)
{
	if (view == NULL || heart_value_label == NULL || heart_status_dot == NULL ||
	    heart_status_label == NULL) {
		return;
	}

	lv_label_set_text(heart_value_label, view->value_text);
	set_dot_color(heart_status_dot,
		      view->metrics_ready ? lv_color_hex(COLOR_GREEN) :
		      lv_color_hex(COLOR_GRAY));
	set_label_color(heart_status_label,
			view->metrics_ready ? lv_color_hex(COLOR_GREEN) :
			lv_color_hex(COLOR_GRAY));
	lv_label_set_text(heart_status_label, view->status_text);
}

static int build_clock_page(lv_obj_t *page)
{
	clock_time_label = create_label(page, "--:--",
					&lv_font_montserrat_36,
					lv_color_hex(COLOR_WHITE));
	if (clock_time_label == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(clock_time_label, LV_ALIGN_TOP_MID, 0, 62);

	clock_date_label = create_label(page, "---- -- --",
					FONT_CJK_16,
					lv_color_hex(COLOR_GRAY));
	if (clock_date_label == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(clock_date_label, LV_ALIGN_TOP_MID, 0, 124);

	clock_sync_dot = create_status_dot(page);
	if (clock_sync_dot == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(clock_sync_dot, LV_ALIGN_BOTTOM_MID, -32, -52);

	clock_sync_label = create_label(page, "未同步",
					FONT_CJK_14,
					lv_color_hex(COLOR_GRAY));
	if (clock_sync_label == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(clock_sync_label, LV_ALIGN_BOTTOM_MID, 10, -48);

	return 0;
}

static int build_wifi_page(lv_obj_t *page)
{
	lv_obj_t *title = create_label(page, "WiFi 信息",
				       FONT_CJK_16,
				       lv_color_hex(COLOR_WHITE));
	if (title == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

	wifi_ssid_label = create_label(page, "SSID: --",
				       &lv_font_montserrat_18,
				       lv_color_hex(COLOR_WHITE));
	if (wifi_ssid_label == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(wifi_ssid_label, LV_ALIGN_TOP_LEFT, 22, 72);

	wifi_state_label = create_label(page, "State: Offline",
					&lv_font_montserrat_18,
					lv_color_hex(COLOR_GRAY));
	if (wifi_state_label == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(wifi_state_label, LV_ALIGN_TOP_LEFT, 22, 106);

	wifi_ip_label = create_label(page, "IP: --",
				     &lv_font_montserrat_18,
				     lv_color_hex(COLOR_WHITE));
	if (wifi_ip_label == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(wifi_ip_label, LV_ALIGN_TOP_LEFT, 22, 140);

	wifi_rssi_label = create_label(page, "RSSI: --",
				       &lv_font_montserrat_18,
				       lv_color_hex(COLOR_WHITE));
	if (wifi_rssi_label == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(wifi_rssi_label, LV_ALIGN_TOP_LEFT, 22, 174);

	return 0;
}

static int build_heart_page(lv_obj_t *page)
{
	heart_icon_label = create_heart_icon(page);
	if (heart_icon_label == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(heart_icon_label, LV_ALIGN_TOP_MID, 0, 48);

	heart_value_label = create_label(page, "---", &lv_font_montserrat_48,
					 lv_color_hex(COLOR_WHITE));
	if (heart_value_label == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(heart_value_label, LV_ALIGN_TOP_MID, 0, 108);

	lv_obj_t *bpm_label = create_label(page, "bpm", &lv_font_montserrat_18,
					   lv_color_hex(COLOR_GRAY));
	if (bpm_label == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(bpm_label, LV_ALIGN_TOP_MID, 0, 172);

	heart_status_dot = create_status_dot(page);
	if (heart_status_dot == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(heart_status_dot, LV_ALIGN_BOTTOM_MID, -48, -52);

	heart_status_label = create_label(page, "Sensor wait",
					  &lv_font_montserrat_14,
					  lv_color_hex(COLOR_GRAY));
	if (heart_status_label == NULL) {
		return -ENOMEM;
	}
	lv_obj_align(heart_status_label, LV_ALIGN_BOTTOM_MID, 12, -48);

	return 0;
}

static void update_indicators(void)
{
	for (int i = 0; i < PAGE_COUNT; i++) {
		if (indicator_dots[i] == NULL) {
			continue;
		}

		lv_label_set_text(indicator_dots[i],
				  i == current_page ? LV_SYMBOL_BULLET : "o");
		set_label_color(indicator_dots[i],
				i == current_page ? lv_color_hex(COLOR_GREEN) :
				lv_color_hex(COLOR_GRAY));
	}
}

void ui_pages_show_page(int page_index)
{
	for (int i = 0; i < PAGE_COUNT; i++) {
		if (pages[i] == NULL) {
			continue;
		}

		if (i == page_index) {
			lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
		}
	}

	current_page = page_index;
	update_indicators();
}

static void request_page_switch(int next_page)
{
	int target_page = (next_page + PAGE_COUNT) % PAGE_COUNT;

	if (target_page != current_page) {
		pending_page = target_page;
		lcd_display_post_event(LCD_DISPLAY_EVENT_PAGE);
	}
}

static void gesture_event_cb(lv_event_t *event)
{
	ARG_UNUSED(event);

	lv_indev_t *indev = lv_indev_active();
	if (indev == NULL) {
		return;
	}

	lv_dir_t dir = lv_indev_get_gesture_dir(indev);
	if (dir == LV_DIR_LEFT) {
		request_page_switch(current_page + 1);
	} else if (dir == LV_DIR_RIGHT) {
		request_page_switch(current_page - 1);
	}

	lv_indev_wait_release(indev);
}

int ui_pages_create(lv_obj_t *screen)
{
	if (screen == NULL) {
		return -EINVAL;
	}

	current_page = 0;

	lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BLACK), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
	lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_add_event_cb(screen, gesture_event_cb, LV_EVENT_GESTURE, NULL);

	lv_obj_t *clock_page = create_page(screen, 0);
	lv_obj_t *wifi_page = create_page(screen, 1);
	lv_obj_t *heart_page = create_page(screen, 2);

	if (clock_page == NULL || wifi_page == NULL || heart_page == NULL) {
		return -ENOMEM;
	}

	lv_obj_add_event_cb(clock_page, gesture_event_cb, LV_EVENT_GESTURE, NULL);
	lv_obj_add_event_cb(wifi_page, gesture_event_cb, LV_EVENT_GESTURE, NULL);
	lv_obj_add_event_cb(heart_page, gesture_event_cb, LV_EVENT_GESTURE, NULL);

	int ret = build_clock_page(clock_page);
	if (ret < 0) {
		return ret;
	}

	ret = build_wifi_page(wifi_page);
	if (ret < 0) {
		return ret;
	}

	ret = build_heart_page(heart_page);
	if (ret < 0) {
		return ret;
	}

	for (int i = 0; i < PAGE_COUNT; i++) {
		indicator_dots[i] = create_label(screen, "o",
						 &lv_font_montserrat_14,
						 lv_color_hex(COLOR_GRAY));
		if (indicator_dots[i] == NULL) {
			return -ENOMEM;
		}
		lv_obj_align(indicator_dots[i], LV_ALIGN_BOTTOM_MID,
			     (i - 1) * 18, INDICATOR_Y);
	}

	ui_pages_show_page(current_page);

	return 0;
}

int ui_pages_get_current_page(void)
{
	return current_page;
}

bool ui_pages_consume_page_request(int *page_index)
{
	if (pending_page < 0) {
		return false;
	}

	if (page_index != NULL) {
		*page_index = pending_page;
	}

	pending_page = -1;
	return true;
}
