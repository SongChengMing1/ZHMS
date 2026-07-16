#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include "../ui/lcd_display.h"
#include "max30102_service.h"

LOG_MODULE_REGISTER(max30102_service, CONFIG_LOG_DEFAULT_LEVEL);

#define MAX30102_NODE DT_NODELABEL(max30102)
#define MAX30102_SAMPLE_RATE_HZ DT_PROP(MAX30102_NODE, smp_sr)
#define MAX30102_SAMPLE_INTERVAL_MS \
	DIV_ROUND_CLOSEST(1000U, MAX30102_SAMPLE_RATE_HZ)
#define MAX30102_STREAM_QUEUE_CAPACITY 128U
#include "max30102_service_internal.h"
#define MAX30102_ACQUISITION_STACK_SIZE 2048
#define MAX30102_ALGORITHM_STACK_SIZE 3072
#define MAX30102_ACQUISITION_PRIORITY 3
#define MAX30102_ALGORITHM_PRIORITY 4

BUILD_ASSERT(MAX30102_SAMPLE_INTERVAL_MS > 0,
	     "MAX30102 sample interval must be greater than zero");

struct max30102_service_runtime max30102_runtime = {
	.dev = DEVICE_DT_GET(MAX30102_NODE),
};

static K_THREAD_STACK_DEFINE(max30102_acquisition_stack,
				     MAX30102_ACQUISITION_STACK_SIZE);
static K_THREAD_STACK_DEFINE(max30102_algorithm_stack,
				     MAX30102_ALGORITHM_STACK_SIZE);
static struct k_thread max30102_acquisition_thread_data;
static struct k_thread max30102_algorithm_thread_data;
static struct max30102_health_snapshot max30102_health_snapshot;
static struct k_spinlock max30102_health_snapshot_lock;

/* 创建 MAX30102 采集线程和算法线程，并设置便于调试的线程名。 */
static void max30102_service_start_threads(void)
{
	k_thread_create(&max30102_acquisition_thread_data,
			max30102_acquisition_stack,
			K_THREAD_STACK_SIZEOF(max30102_acquisition_stack),
			max30102_acquisition_thread, NULL, NULL, NULL,
			MAX30102_ACQUISITION_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&max30102_acquisition_thread_data,
			  "max30102_acquisition");

	k_thread_create(&max30102_algorithm_thread_data,
			max30102_algorithm_stack,
			K_THREAD_STACK_SIZEOF(max30102_algorithm_stack),
			max30102_algorithm_thread, NULL, NULL, NULL,
			MAX30102_ALGORITHM_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&max30102_algorithm_thread_data,
			  "max30102_algorithm");
}

/* 向跨线程流队列写入一条数据，必要时唤醒等待中的算法线程。 */
bool max30102_stream_queue_push(const struct max30102_stream_item *item)
{
	struct max30102_stream_queue *queue = &max30102_runtime.queue;
	k_spinlock_key_t key;
	bool wake_stream_thread = false;

	key = k_spin_lock(&queue->lock);
	if (queue->count >= MAX30102_STREAM_QUEUE_CAPACITY) {
		k_spin_unlock(&queue->lock, key);
		return false;
	}

	queue->items[queue->write_index] = *item;
	queue->write_index =
		(queue->write_index + 1U) % MAX30102_STREAM_QUEUE_CAPACITY;
	queue->count++;
	if (queue->count > max30102_runtime.max_queue_depth_seen) {
		max30102_runtime.max_queue_depth_seen = queue->count;
	}
	if (queue->count == 1U) {
		wake_stream_thread = true;
	}
	k_spin_unlock(&queue->lock, key);
	if (wake_stream_thread) {
		k_sem_give(&max30102_runtime.stream_sem);
	}

	return true;
}

/* 从跨线程流队列弹出一条数据，供下游算法线程继续处理。 */
bool max30102_stream_queue_pop(struct max30102_stream_item *item)
{
	struct max30102_stream_queue *queue = &max30102_runtime.queue;
	k_spinlock_key_t key;

	key = k_spin_lock(&queue->lock);
	if (queue->count == 0U) {
		k_spin_unlock(&queue->lock, key);
		return false;
	}

	*item = queue->items[queue->read_index];
	queue->read_index =
		(queue->read_index + 1U) % MAX30102_STREAM_QUEUE_CAPACITY;
	queue->count--;
	k_spin_unlock(&queue->lock, key);

	return true;
}

/* 初始化服务运行时状态，确认设备可用后启动后台线程。 */
void max30102_service_init(void)
{
	if (atomic_get(&max30102_runtime.initialized)) {
		return;
	}

	k_sem_init(&max30102_runtime.stream_sem, 0, K_SEM_MAX_LIMIT);
	max30102_runtime.queue.read_index = 0U;
	max30102_runtime.queue.write_index = 0U;
	max30102_runtime.queue.count = 0U;
	max30102_runtime.logical_timestamp_ms = 0U;
	max30102_runtime.logical_timestamp_anchored = false;
	max30102_runtime.next_seq = 0U;
	max30102_runtime.pending_stream_gap.active = false;
	max30102_runtime.pending_stream_gap.reason = 0U;
	max30102_runtime.pending_stream_gap.dropped_samples = 0U;
	max30102_runtime.queue_drop_count = 0U;
	max30102_runtime.fifo_overflow_count = 0U;
	max30102_runtime.driver_error_count = 0U;
	max30102_runtime.irq_wakeup_count = 0U;
	max30102_runtime.max_queue_depth_seen = 0U;
	max30102_runtime.last_stream_gap_reason = 0U;
	max30102_health_snapshot = (struct max30102_health_snapshot){0};

	if (!device_is_ready(max30102_runtime.dev)) {
		LOG_ERR("MAX30102 device not ready");
		return;
	}

	max30102_service_start_threads();
	atomic_set(&max30102_runtime.enabled, 0);
	atomic_set(&max30102_runtime.initialized, 1);
}

/* 允许采集和算法线程开始工作，并输出串口指标表头。 */
void max30102_service_enable(void)
{
	if (!atomic_get(&max30102_runtime.initialized)) {
		return;
	}

	atomic_set(&max30102_runtime.enabled, 1);
	printk("bpm,hrv,stress_level\n");
}

/* 将当前服务遥测计数器复制给调用方，供诊断或上层展示使用。 */
void max30102_service_get_telemetry(struct max30102_service_telemetry *telemetry)
{
	if (telemetry == NULL) {
		return;
	}

	*telemetry = (struct max30102_service_telemetry) {
		.queue_drop_count = max30102_runtime.queue_drop_count,
		.fifo_overflow_count = max30102_runtime.fifo_overflow_count,
		.driver_error_count = max30102_runtime.driver_error_count,
		.irq_wakeup_count = max30102_runtime.irq_wakeup_count,
		.max_queue_depth_seen = max30102_runtime.max_queue_depth_seen,
		.last_stream_gap_reason = max30102_runtime.last_stream_gap_reason,
	};
}

void max30102_service_get_health_snapshot(
	struct max30102_health_snapshot *snapshot)
{
	k_spinlock_key_t key;

	if (snapshot == NULL) {
		return;
	}

	key = k_spin_lock(&max30102_health_snapshot_lock);
	*snapshot = max30102_health_snapshot;
	k_spin_unlock(&max30102_health_snapshot_lock, key);
}

void max30102_service_set_health_snapshot(bool bpm_valid, uint16_t bpm)
{
	k_spinlock_key_t key;
	bool changed = false;

	key = k_spin_lock(&max30102_health_snapshot_lock);
	if (max30102_health_snapshot.bpm_valid != bpm_valid ||
	    max30102_health_snapshot.bpm != bpm) {
		max30102_health_snapshot.bpm_valid = bpm_valid;
		max30102_health_snapshot.bpm = bpm;
		changed = true;
	}
	k_spin_unlock(&max30102_health_snapshot_lock, key);

	if (changed) {
		lcd_display_post_event(LCD_DISPLAY_EVENT_HEART);
	}
}

void max30102_service_clear_health_snapshot(void)
{
	max30102_service_set_health_snapshot(false, 0U);
}
