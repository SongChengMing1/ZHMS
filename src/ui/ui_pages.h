#ifndef UI_PAGES_H_
#define UI_PAGES_H_

#include <stdbool.h>
#include <lvgl.h>

#include "../services/time_sync.h"
#include "../services/wifi.h"

#define UI_PAGE_CLOCK 0
#define UI_PAGE_WIFI  1
#define UI_PAGE_HEART 2

#define UI_PAGE_TIME_TEXT_LEN 16
#define UI_PAGE_DATE_TEXT_LEN 32
#define UI_PAGE_WIFI_SSID_TEXT_LEN 64
#define UI_PAGE_WIFI_STATE_TEXT_LEN 64
#define UI_PAGE_WIFI_IP_TEXT_LEN 48
#define UI_PAGE_WIFI_RSSI_TEXT_LEN 32
#define UI_PAGE_HEART_VALUE_TEXT_LEN 8
#define UI_PAGE_HEART_STATUS_TEXT_LEN 32

struct ui_pages_clock_view {
	char time_text[UI_PAGE_TIME_TEXT_LEN];
	char date_text[UI_PAGE_DATE_TEXT_LEN];
	enum time_sync_state sync_state;
};

struct ui_pages_wifi_view {
	enum wifi_conn_state state;
	char ssid_text[UI_PAGE_WIFI_SSID_TEXT_LEN];
	char state_text[UI_PAGE_WIFI_STATE_TEXT_LEN];
	char ip_text[UI_PAGE_WIFI_IP_TEXT_LEN];
	char rssi_text[UI_PAGE_WIFI_RSSI_TEXT_LEN];
};

struct ui_pages_heart_view {
	char value_text[UI_PAGE_HEART_VALUE_TEXT_LEN];
	char status_text[UI_PAGE_HEART_STATUS_TEXT_LEN];
	bool metrics_ready;
};

struct ui_pages_view {
	struct ui_pages_clock_view clock;
	struct ui_pages_wifi_view wifi;
	struct ui_pages_heart_view heart;
};

int ui_pages_create(lv_obj_t *screen);
int ui_pages_get_current_page(void);
bool ui_pages_consume_page_request(int *page_index);
void ui_pages_show_page(int page_index);
void ui_pages_prepare_clock(struct ui_pages_clock_view *view);
void ui_pages_prepare_wifi(struct ui_pages_wifi_view *view);
void ui_pages_prepare_heart(struct ui_pages_heart_view *view);
void ui_pages_render_clock(const struct ui_pages_clock_view *view);
void ui_pages_render_wifi(const struct ui_pages_wifi_view *view);
void ui_pages_render_heart(const struct ui_pages_heart_view *view);

#endif /* UI_PAGES_H_ */
