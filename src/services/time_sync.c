#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/sntp.h>
#include <zephyr/sys/clock.h>

#include "../ui/lcd_display.h"
#include "time_sync.h"
#include "wifi.h"

LOG_MODULE_REGISTER(time_sync, CONFIG_LOG_DEFAULT_LEVEL);

#define TIME_SYNC_STACK_SIZE 2048
#define TIME_SYNC_PRIORITY   8
#define TIME_SYNC_SERVER     "pool.ntp.org"
#define TIME_SYNC_TIMEOUT_MS 4000
#define TIME_SYNC_PERIOD     K_HOURS(24)
#define TIME_SYNC_RETRY_PERIOD K_SECONDS(15)
#define TIME_SYNC_READY_GRACE_PERIOD K_SECONDS(2)
#define TIME_SYNC_PERIOD_MS (24LL * 60 * 60 * 1000)
#define TIME_SYNC_RETRY_PERIOD_MS (15LL * 1000)
#define TIME_SYNC_READY_GRACE_PERIOD_MS (2LL * 1000)
#define WIFI_WAIT_PERIOD     K_SECONDS(1)
#define TIME_SYNC_WAIT_PERIOD K_MSEC(100)
#define TIME_SYNC_NO_DEADLINE INT64_MAX

static atomic_t time_sync_initialized = ATOMIC_INIT(0);
static atomic_t time_sync_enabled = ATOMIC_INIT(0);
static atomic_t sync_state = ATOMIC_INIT(TIME_SYNC_UNSYNCED);
static K_SEM_DEFINE(time_sync_wakeup_sem, 0, 1);
static struct k_spinlock time_sync_request_lock;
static bool sync_requested;

static void set_sync_state(enum time_sync_state state)
{
	enum time_sync_state prev = (enum time_sync_state)atomic_get(&sync_state);

	atomic_set(&sync_state, state);
	if (prev != state) {
		lcd_display_post_event(LCD_DISPLAY_EVENT_CLOCK);
	}
}

static int set_system_time(uint64_t seconds)
{
	struct timespec ts = {
		.tv_sec = (time_t)seconds,
		.tv_nsec = 0,
	};

	return sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
}

static int sync_once(void)
{
	struct sntp_time sntp_time;
	int ret;

	set_sync_state(TIME_SYNC_IN_PROGRESS);

	ret = sntp_simple(TIME_SYNC_SERVER, TIME_SYNC_TIMEOUT_MS, &sntp_time);
	if (ret < 0) {
		LOG_WRN("SNTP query failed (%d)", ret);
		set_sync_state(TIME_SYNC_UNSYNCED);
		return ret;
	}

	ret = set_system_time(sntp_time.seconds);
	if (ret < 0) {
		LOG_WRN("Failed to set system time (%d)", ret);
		set_sync_state(TIME_SYNC_UNSYNCED);
		return ret;
	}

	LOG_INF("Time synced using SNTP");
	set_sync_state(TIME_SYNC_SYNCED);
	return 0;
}

static void request_sync(void)
{
	k_spinlock_key_t key = k_spin_lock(&time_sync_request_lock);

	sync_requested = true;
	k_spin_unlock(&time_sync_request_lock, key);
	k_sem_give(&time_sync_wakeup_sem);
}

static bool consume_sync_request(void)
{
	k_spinlock_key_t key = k_spin_lock(&time_sync_request_lock);
	bool requested = sync_requested;

	sync_requested = false;
	k_spin_unlock(&time_sync_request_lock, key);

	return requested;
}

static int wait_for_sync_event(k_timeout_t timeout)
{
	int ret = k_sem_take(&time_sync_wakeup_sem, timeout);

	if (ret != 0 && ret != -EAGAIN) {
		LOG_WRN("Failed to wait for sync event (%d)", ret);
	}

	return ret;
}

static void time_sync_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	int64_t next_attempt_ms = TIME_SYNC_NO_DEADLINE;

	while (1) {
		int64_t now_ms = k_uptime_get();

		if (!atomic_get(&time_sync_enabled)) {
			(void)wait_for_sync_event(TIME_SYNC_WAIT_PERIOD);
			continue;
		}

		if (consume_sync_request()) {
			next_attempt_ms = now_ms + TIME_SYNC_READY_GRACE_PERIOD_MS;
		}

		if (wifi_get_state() != WIFI_READY) {
			(void)wait_for_sync_event(WIFI_WAIT_PERIOD);
			continue;
		}

		if (next_attempt_ms == TIME_SYNC_NO_DEADLINE) {
			next_attempt_ms = now_ms;
		}

		if (now_ms < next_attempt_ms) {
			int32_t wait_ms = (int32_t)(next_attempt_ms - now_ms);

			(void)wait_for_sync_event(K_MSEC(wait_ms));
			continue;
		}

		if (sync_once() == 0) {
			next_attempt_ms = k_uptime_get() + TIME_SYNC_PERIOD_MS;
		} else {
			next_attempt_ms = k_uptime_get() + TIME_SYNC_RETRY_PERIOD_MS;
		}
	}
}

K_THREAD_DEFINE(time_sync_thread_id, TIME_SYNC_STACK_SIZE,
		time_sync_thread, NULL, NULL, NULL,
		TIME_SYNC_PRIORITY, 0, 0);

void time_sync_init(void)
{
	k_spinlock_key_t key = k_spin_lock(&time_sync_request_lock);

	sync_requested = false;
	k_spin_unlock(&time_sync_request_lock, key);
	set_sync_state(TIME_SYNC_UNSYNCED);
	atomic_set(&time_sync_enabled, 0);
	atomic_set(&time_sync_initialized, 1);
	LOG_INF("Time sync service initialized");
}

void time_sync_enable(void)
{
	if (!atomic_get(&time_sync_initialized)) {
		LOG_ERR("Time sync not initialized");
		return;
	}

	atomic_set(&time_sync_enabled, 1);
	request_sync();
	LOG_INF("Time sync service enabled");
}

void time_sync_notify_wifi_ready(void)
{
	if (!atomic_get(&time_sync_initialized) ||
	    !atomic_get(&time_sync_enabled)) {
		return;
	}

	request_sync();
}

enum time_sync_state time_sync_get_state(void)
{
	return (enum time_sync_state)atomic_get(&sync_state);
}
