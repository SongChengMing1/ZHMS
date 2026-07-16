#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>

#include "../ui/lcd_display.h"
#include "time_sync.h"
#include "wifi.h"

LOG_MODULE_REGISTER(wifi, CONFIG_LOG_DEFAULT_LEVEL);

#define WIFI_STACK_SIZE  4096
#define WIFI_PRIORITY    5
#define MSGQ_MAX_MSGS    8
#define MAX_RETRY        6

#define TIMEOUT_CONNECT     K_SECONDS(15)
#define TIMEOUT_OBTAIN_IP   K_SECONDS(60)
#define TIMEOUT_RECONNECT   K_SECONDS(5)
#define TIMEOUT_MONITOR     K_SECONDS(5)
#define WIFI_INIT_WAIT_PERIOD K_MSEC(100)

/* --- Event types --- */
enum wifi_event_type {
	WIFI_EVT_CONNECT_CMD,
	WIFI_EVT_DISCONNECT_CMD,
	WIFI_EVT_CONNECT_RESULT,
	WIFI_EVT_DISCONNECTED,
	WIFI_EVT_IPV4_READY,
	WIFI_EVT_TIMER,
};

struct wifi_event {
	enum wifi_event_type type;
	union {
		int connect_status; /* WIFI_EVT_CONNECT_RESULT */
	};
};

/* --- Module state --- */
static struct net_if *iface;
static struct wifi_connect_req_params params;

K_MSGQ_DEFINE(wifi_msgq, sizeof(struct wifi_event), MSGQ_MAX_MSGS, 4);

static atomic_t wifi_initialized = ATOMIC_INIT(0);
static atomic_t conn_state = ATOMIC_INIT(WIFI_DISCONNECTED);
static struct wifi_snapshot wifi_runtime_snapshot = {
	.state = WIFI_DISCONNECTED,
};
static struct k_spinlock wifi_snapshot_lock;

static struct net_mgmt_event_callback wifi_conn_cb;
static struct net_mgmt_event_callback wifi_disc_cb;
static struct net_mgmt_event_callback ipv4_cb;

static bool wifi_credentials_configured(void)
{
	return WIFI_SSID[0] != '\0' && WIFI_PSK[0] != '\0';
}

static bool wifi_snapshot_equals(const struct wifi_snapshot *a,
				 const struct wifi_snapshot *b)
{
	return a->state == b->state &&
	       a->ip_valid == b->ip_valid &&
	       strcmp(a->ip_addr, b->ip_addr) == 0 &&
	       a->rssi_valid == b->rssi_valid &&
	       a->rssi_dbm == b->rssi_dbm;
}

/* --- Helpers --- */

static void wifi_snapshot_reset(enum wifi_conn_state state)
{
	k_spinlock_key_t key = k_spin_lock(&wifi_snapshot_lock);
	struct wifi_snapshot before = wifi_runtime_snapshot;
	bool changed;

	wifi_runtime_snapshot.state = state;
	wifi_runtime_snapshot.ip_valid = false;
	wifi_runtime_snapshot.ip_addr[0] = '\0';
	wifi_runtime_snapshot.rssi_valid = false;
	wifi_runtime_snapshot.rssi_dbm = 0;
	changed = !wifi_snapshot_equals(&before, &wifi_runtime_snapshot);

	k_spin_unlock(&wifi_snapshot_lock, key);
	if (changed) {
		lcd_display_post_event(LCD_DISPLAY_EVENT_WIFI);
	}
}

static void wifi_snapshot_set_state(enum wifi_conn_state state)
{
	k_spinlock_key_t key = k_spin_lock(&wifi_snapshot_lock);
	struct wifi_snapshot before = wifi_runtime_snapshot;
	bool changed;

	wifi_runtime_snapshot.state = state;
	changed = !wifi_snapshot_equals(&before, &wifi_runtime_snapshot);

	k_spin_unlock(&wifi_snapshot_lock, key);
	if (changed) {
		lcd_display_post_event(LCD_DISPLAY_EVENT_WIFI);
	}
}

static void wifi_snapshot_set_ip(const char *ip_addr)
{
	k_spinlock_key_t key = k_spin_lock(&wifi_snapshot_lock);
	struct wifi_snapshot before = wifi_runtime_snapshot;
	bool changed;

	if (ip_addr != NULL && ip_addr[0] != '\0') {
		strncpy(wifi_runtime_snapshot.ip_addr, ip_addr,
			sizeof(wifi_runtime_snapshot.ip_addr) - 1);
		wifi_runtime_snapshot
			.ip_addr[sizeof(wifi_runtime_snapshot.ip_addr) - 1] = '\0';
		wifi_runtime_snapshot.ip_valid = true;
	} else {
		wifi_runtime_snapshot.ip_addr[0] = '\0';
		wifi_runtime_snapshot.ip_valid = false;
	}
	changed = !wifi_snapshot_equals(&before, &wifi_runtime_snapshot);

	k_spin_unlock(&wifi_snapshot_lock, key);
	if (changed) {
		lcd_display_post_event(LCD_DISPLAY_EVENT_WIFI);
	}
}

static void wifi_snapshot_set_rssi(int rssi_dbm)
{
	k_spinlock_key_t key = k_spin_lock(&wifi_snapshot_lock);
	struct wifi_snapshot before = wifi_runtime_snapshot;
	bool changed;

	wifi_runtime_snapshot.rssi_dbm = rssi_dbm;
	wifi_runtime_snapshot.rssi_valid = true;
	changed = !wifi_snapshot_equals(&before, &wifi_runtime_snapshot);

	k_spin_unlock(&wifi_snapshot_lock, key);
	if (changed) {
		lcd_display_post_event(LCD_DISPLAY_EVENT_WIFI);
	}
}

static void wifi_snapshot_clear_rssi(void)
{
	k_spinlock_key_t key = k_spin_lock(&wifi_snapshot_lock);
	struct wifi_snapshot before = wifi_runtime_snapshot;
	bool changed;

	wifi_runtime_snapshot.rssi_dbm = 0;
	wifi_runtime_snapshot.rssi_valid = false;
	changed = !wifi_snapshot_equals(&before, &wifi_runtime_snapshot);

	k_spin_unlock(&wifi_snapshot_lock, key);
	if (changed) {
		lcd_display_post_event(LCD_DISPLAY_EVENT_WIFI);
	}
}

static void set_conn_state(enum wifi_conn_state state)
{
	enum wifi_conn_state prev_state =
		(enum wifi_conn_state)atomic_get(&conn_state);

	atomic_set(&conn_state, state);
	if (state == WIFI_READY && prev_state != WIFI_READY) {
		time_sync_notify_wifi_ready();
	}

	if (state == WIFI_READY) {
		wifi_snapshot_set_state(state);
		return;
	}

	wifi_snapshot_reset(state);
}

static k_timeout_t state_timeout(enum wifi_conn_state st)
{
	switch (st) {
	case WIFI_CONNECTING:   return TIMEOUT_CONNECT;
	case WIFI_OBTAINING_IP:  return TIMEOUT_OBTAIN_IP;
	case WIFI_RECONNECTING: return TIMEOUT_RECONNECT;
	case WIFI_READY:        return TIMEOUT_MONITOR;
	default:                return K_FOREVER;
	}
}

static bool read_ip_addr(char *ip_addr, size_t ip_addr_size)
{
	if (ip_addr == NULL || ip_addr_size == 0U) {
		return false;
	}

	ip_addr[0] = '\0';

	if (!iface || !iface->config.ip.ipv4) {
		return false;
	}

	struct in_addr *addr =
		&iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr;

	if (net_addr_ntop(AF_INET, addr, ip_addr, ip_addr_size) == NULL) {
		return false;
	}

	return true;
}

static int do_connect(void)
{
	if (!iface) {
		LOG_ERR("No interface for connect");
		return -ENODEV;
	}
	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface,
			   &params, sizeof(params));
	if (ret < 0) {
		LOG_ERR("Connect request failed (%d)", ret);
	}
	return ret;
}

static void do_disconnect(void)
{
	if (!iface) {
		return;
	}
	int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	if (ret < 0) {
		LOG_WRN("Disconnect request failed (%d)", ret);
	}
}

static int query_iface_status(struct wifi_iface_status *status)
{
	if (status == NULL) {
		return -EINVAL;
	}

	if (!iface) {
		return -ENODEV;
	}

	return net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface,
			status, sizeof(*status));
}

static bool iface_status_is_connected(const struct wifi_iface_status *status)
{
	return status != NULL && status->state >= WIFI_STATE_ASSOCIATED;
}

static void refresh_ready_snapshot(const struct wifi_iface_status *status)
{
	struct wifi_iface_status local_status;
	const struct wifi_iface_status *ready_status = status;
	char ip_addr[WIFI_SNAPSHOT_IP_ADDR_LEN];

	if (ready_status == NULL && query_iface_status(&local_status) == 0) {
		ready_status = &local_status;
	}

	wifi_snapshot_set_state(WIFI_READY);
	wifi_snapshot_set_ip(read_ip_addr(ip_addr, sizeof(ip_addr)) ? ip_addr :
			     NULL);

	if (iface_status_is_connected(ready_status)) {
		wifi_snapshot_set_rssi(ready_status->rssi);
	} else {
		wifi_snapshot_clear_rssi();
	}
}

/* --- mgmt event callbacks --- only enqueue, no state change --- */

static void on_connect_result(struct net_mgmt_event_callback *cb,
			      uint64_t mgmt_event, struct net_if *iface)
{
	const struct wifi_status *s = (const struct wifi_status *)cb->info;
	if (!s) {
		LOG_ERR("CONNECT_RESULT with NULL info");
		return;
	}
	struct wifi_event evt = {
		.type = WIFI_EVT_CONNECT_RESULT,
		.connect_status = s->status,
	};
	if (k_msgq_put(&wifi_msgq, &evt, K_NO_WAIT) != 0) {
		LOG_WRN("WiFi msgq full, dropping event %d", evt.type);
	}
}

static void on_disconnected(struct net_mgmt_event_callback *cb,
			    uint64_t mgmt_event, struct net_if *iface)
{
	struct wifi_event evt = { .type = WIFI_EVT_DISCONNECTED };
	if (k_msgq_put(&wifi_msgq, &evt, K_NO_WAIT) != 0) {
		LOG_WRN("WiFi msgq full, dropping event %d", evt.type);
	}
}

static void on_ipv4_add(struct net_mgmt_event_callback *cb,
			uint64_t mgmt_event, struct net_if *iface)
{
	struct wifi_event evt = { .type = WIFI_EVT_IPV4_READY };
	if (k_msgq_put(&wifi_msgq, &evt, K_NO_WAIT) != 0) {
		LOG_WRN("WiFi msgq full, dropping event %d", evt.type);
	}
}

/* --- State machine --- */

static void dispatch(enum wifi_conn_state st, struct wifi_event *evt, int *retry)
{
	switch (st) {

	case WIFI_DISCONNECTED:
		if (evt->type == WIFI_EVT_CONNECT_CMD) {
			do_connect();
			set_conn_state(WIFI_CONNECTING);
		} else if (evt->type == WIFI_EVT_DISCONNECT_CMD) {
			LOG_INF("Already disconnected, ignoring");
		}
		break;

	case WIFI_CONNECTING:
		switch (evt->type) {
		case WIFI_EVT_IPV4_READY:
			*retry = 0;
			set_conn_state(WIFI_READY);
			refresh_ready_snapshot(NULL);
			LOG_INF("IP obtained during connect");
			break;
		case WIFI_EVT_CONNECT_RESULT:
			if (evt->connect_status == 0) {
				set_conn_state(WIFI_OBTAINING_IP);
			} else {
				LOG_ERR("Connect failed (status %d)", evt->connect_status);
				(*retry)++;
				if (*retry >= MAX_RETRY) {
					LOG_ERR("Max retries, entering ERROR");
					set_conn_state(WIFI_ERROR);
				} else {
					set_conn_state(WIFI_RECONNECTING);
				}
			}
			break;
		case WIFI_EVT_DISCONNECT_CMD:
			do_disconnect();
			*retry = 0;
			set_conn_state(WIFI_DISCONNECTED);
			break;
		case WIFI_EVT_CONNECT_CMD:
			LOG_WRN("Connect requested while already connecting");
			break;
		case WIFI_EVT_TIMER:
			LOG_WRN("Connect timeout");
			(*retry)++;
			if (*retry >= MAX_RETRY) {
				LOG_ERR("Max retries, entering ERROR");
				set_conn_state(WIFI_ERROR);
			} else {
				set_conn_state(WIFI_RECONNECTING);
			}
			break;
		default:
			break;
		}
		break;

	case WIFI_OBTAINING_IP:
		switch (evt->type) {
		case WIFI_EVT_IPV4_READY:
			*retry = 0;
			set_conn_state(WIFI_READY);
			refresh_ready_snapshot(NULL);
			LOG_INF("Ready, IP obtained");
			break;
		case WIFI_EVT_DISCONNECTED:
			LOG_WRN("Disconnected while obtaining IP");
			(*retry)++;
			if (*retry >= MAX_RETRY) {
				LOG_ERR("Max retries, entering ERROR");
				set_conn_state(WIFI_ERROR);
			} else {
				set_conn_state(WIFI_RECONNECTING);
			}
			break;
		case WIFI_EVT_DISCONNECT_CMD:
			do_disconnect();
			*retry = 0;
			set_conn_state(WIFI_DISCONNECTED);
			break;
		case WIFI_EVT_TIMER:
			{
				char ip_addr[WIFI_SNAPSHOT_IP_ADDR_LEN];

				if (read_ip_addr(ip_addr, sizeof(ip_addr))) {
					LOG_INF("DHCP slow but IP obtained: %s",
						ip_addr);
					*retry = 0;
					set_conn_state(WIFI_READY);
					refresh_ready_snapshot(NULL);
				} else {
					LOG_WRN("DHCP timeout");
					(*retry)++;
					if (*retry >= MAX_RETRY) {
						LOG_ERR("Max retries, entering ERROR");
						set_conn_state(WIFI_ERROR);
					} else {
						set_conn_state(WIFI_RECONNECTING);
					}
				}
			}
			break;
		default:
			break;
		}
		break;

	case WIFI_READY:
		switch (evt->type) {
		case WIFI_EVT_DISCONNECTED:
			LOG_WRN("Connection lost");
			*retry = 0;
			set_conn_state(WIFI_RECONNECTING);
			break;
		case WIFI_EVT_DISCONNECT_CMD:
			do_disconnect();
			*retry = 0;
			set_conn_state(WIFI_DISCONNECTED);
			break;
		case WIFI_EVT_TIMER:
			{
				struct wifi_iface_status status;

				if (query_iface_status(&status) < 0 ||
				    !iface_status_is_connected(&status)) {
					LOG_WRN("Connection lost (monitor)");
					*retry = 0;
					set_conn_state(WIFI_RECONNECTING);
				} else {
					refresh_ready_snapshot(&status);
				}
			}
			break;
		default:
			break;
		}
		break;

	case WIFI_RECONNECTING:
		switch (evt->type) {
		case WIFI_EVT_IPV4_READY:
			*retry = 0;
			set_conn_state(WIFI_READY);
			refresh_ready_snapshot(NULL);
			LOG_INF("IP obtained during reconnect");
			break;
		case WIFI_EVT_TIMER:
			{
				struct wifi_iface_status status;

				if (query_iface_status(&status) == 0 &&
				    iface_status_is_connected(&status)) {
					char ip_addr[WIFI_SNAPSHOT_IP_ADDR_LEN];

					if (read_ip_addr(ip_addr, sizeof(ip_addr))) {
						LOG_INF("Already connected with IP: %s",
							ip_addr);
						*retry = 0;
						set_conn_state(WIFI_READY);
						refresh_ready_snapshot(&status);
					} else {
						LOG_INF("Already connected, waiting for IP...");
						*retry = 0;
						set_conn_state(WIFI_OBTAINING_IP);
					}
				} else {
					LOG_INF("Reconnecting (attempt %d/%d)...",
						*retry + 1, MAX_RETRY);
					if (do_connect() < 0) {
						(*retry)++;
						if (*retry >= MAX_RETRY) {
							LOG_ERR("Max retries, entering ERROR");
							set_conn_state(WIFI_ERROR);
						}
					} else {
						set_conn_state(WIFI_CONNECTING);
					}
				}
			}
			break;
		case WIFI_EVT_DISCONNECT_CMD:
			*retry = 0;
			set_conn_state(WIFI_DISCONNECTED);
			break;
		default:
			break;
		}
		break;

	case WIFI_ERROR:
		if (evt->type == WIFI_EVT_CONNECT_CMD) {
			LOG_INF("Retrying from ERROR state...");
			*retry = 0;
			do_connect();
			set_conn_state(WIFI_CONNECTING);
		}
		break;
	}
}

/* --- Thread --- */

static void wifi_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct wifi_event evt;
	int retry = 0;

	LOG_INF("WiFi thread started");

	while (1) {
		if (!atomic_get(&wifi_initialized)) {
			k_sleep(WIFI_INIT_WAIT_PERIOD);
			continue;
		}

		enum wifi_conn_state st = atomic_get(&conn_state);
		k_timeout_t timeout = state_timeout(st);

		int ret = k_msgq_get(&wifi_msgq, &evt, timeout);
		if (ret == -EAGAIN) {
			evt.type = WIFI_EVT_TIMER;
		}

		dispatch(st, &evt, &retry);
	}
}

K_THREAD_DEFINE(wifi_thread, WIFI_STACK_SIZE,
		wifi_thread_fn, NULL, NULL, NULL,
		WIFI_PRIORITY, 0, 0);

/* --- Public API --- */

void wifi_init(void)
{
	atomic_set(&wifi_initialized, 0);
	set_conn_state(WIFI_DISCONNECTED);

	iface = net_if_get_default();
	if (!iface) {
		LOG_ERR("No default network interface");
		return;
	}
	if (!wifi_credentials_configured()) {
		LOG_WRN("WiFi credentials are not configured; set "
			 "CONFIG_ZHMS_WIFI_SSID and CONFIG_ZHMS_WIFI_PSK");
		return;
	}

	params.ssid = (const uint8_t *)WIFI_SSID;
	params.ssid_length = strlen(WIFI_SSID);
	params.psk = (const uint8_t *)WIFI_PSK;
	params.psk_length = strlen(WIFI_PSK);
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.band = WIFI_FREQ_BAND_UNKNOWN;
	params.channel = WIFI_CHANNEL_ANY;
	params.mfp = WIFI_MFP_OPTIONAL;

	net_mgmt_init_event_callback(&wifi_conn_cb, on_connect_result,
				     NET_EVENT_WIFI_CONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_conn_cb);

	net_mgmt_init_event_callback(&wifi_disc_cb, on_disconnected,
				     NET_EVENT_WIFI_DISCONNECT_COMPLETE |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_disc_cb);

	net_mgmt_init_event_callback(&ipv4_cb, on_ipv4_add,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	atomic_set(&wifi_initialized, 1);
	LOG_INF("WiFi service initialized");
}

void wifi_connect(void)
{
	if (!wifi_credentials_configured()) {
		LOG_WRN("WiFi connection skipped because credentials are not configured");
		return;
	}
	if (!atomic_get(&wifi_initialized) || !iface) {
		LOG_ERR("WiFi not initialized");
		return;
	}
	struct wifi_event evt = { .type = WIFI_EVT_CONNECT_CMD };
	if (k_msgq_put(&wifi_msgq, &evt, K_FOREVER) != 0) {
		LOG_ERR("Failed to enqueue CONNECT_CMD");
	}
}

void wifi_disconnect(void)
{
	if (!atomic_get(&wifi_initialized) || !iface) {
		LOG_ERR("WiFi not initialized");
		return;
	}
	struct wifi_event evt = { .type = WIFI_EVT_DISCONNECT_CMD };
	if (k_msgq_put(&wifi_msgq, &evt, K_FOREVER) != 0) {
		LOG_ERR("Failed to enqueue DISCONNECT_CMD");
	}
}

enum wifi_conn_state wifi_get_state(void)
{
	return atomic_get(&conn_state);
}

int wifi_get_snapshot(struct wifi_snapshot *snapshot)
{
	k_spinlock_key_t key;

	if (snapshot == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&wifi_snapshot_lock);
	*snapshot = wifi_runtime_snapshot;
	k_spin_unlock(&wifi_snapshot_lock, key);

	return 0;
}
