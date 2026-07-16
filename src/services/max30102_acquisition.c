#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "../../drivers/sensor/max30102/max30102.h"
#include "max30102_service_internal.h"

#define MAX30102_NODE DT_NODELABEL(max30102)
#define MAX30102_SAMPLE_RATE_HZ DT_PROP(MAX30102_NODE, smp_sr)
#define MAX30102_SAMPLE_INTERVAL_MS \
	DIV_ROUND_CLOSEST(1000U, MAX30102_SAMPLE_RATE_HZ)
#define MAX30102_ACQUISITION_POLL_PERIOD_MS 2U
#define MAX30102_ACQUISITION_EMPTY_FETCH_THRESHOLD 100U

BUILD_ASSERT(MAX30102_SAMPLE_INTERVAL_MS > 0,
	     "MAX30102 sample interval must be greater than zero");

static uint32_t max30102_empty_fetch_count;

/* 按采样周期生成单调递增的逻辑时间戳，作为后续处理链统一时间基准。 */
uint32_t max30102_acquisition_next_timestamp_ms(void)
{
	if (!max30102_runtime.logical_timestamp_anchored) {
		max30102_runtime.logical_timestamp_ms = k_uptime_get_32();
		max30102_runtime.logical_timestamp_anchored = true;
	} else {
		max30102_runtime.logical_timestamp_ms +=
			MAX30102_SAMPLE_INTERVAL_MS;
	}

	return max30102_runtime.logical_timestamp_ms;
}

/* 在流队列暂时写不进去时缓存缺口事件，等待后续补发。 */
static void max30102_queue_pending_stream_gap(uint8_t reason,
					      uint16_t dropped_samples)
{
	if (!max30102_runtime.pending_stream_gap.active) {
		max30102_runtime.pending_stream_gap.active = true;
		max30102_runtime.pending_stream_gap.reason = reason;
		max30102_runtime.pending_stream_gap.dropped_samples =
			dropped_samples;
	} else {
		max30102_runtime.pending_stream_gap.reason = reason;
		max30102_runtime.pending_stream_gap.dropped_samples +=
			dropped_samples;
	}

	max30102_runtime.last_stream_gap_reason = reason;
}

/* 尝试把之前累计的缺口事件重新压入流队列。 */
static bool max30102_try_flush_pending_stream_gap(void)
{
	struct max30102_stream_item gap;

	if (!max30102_runtime.pending_stream_gap.active) {
		return true;
	}

	gap.type = MAX30102_ITEM_STREAM_GAP;
	gap.gap.timestamp_ms = k_uptime_get_32();
	gap.gap.dropped_samples = max30102_runtime.pending_stream_gap.dropped_samples;
	gap.gap.reason = max30102_runtime.pending_stream_gap.reason;

	if (!max30102_stream_queue_push(&gap)) {
		return false;
	}

	max30102_runtime.pending_stream_gap.active = false;
	max30102_runtime.pending_stream_gap.reason = 0U;
	max30102_runtime.pending_stream_gap.dropped_samples = 0U;
	return true;
}

/* 立即生成一条流缺口事件，若队列拥塞则退化为挂起缓存。 */
static bool max30102_emit_stream_gap(uint32_t timestamp_ms, uint8_t reason,
				     uint16_t dropped_samples)
{
	struct max30102_stream_item gap = {
		.type = MAX30102_ITEM_STREAM_GAP,
		.gap = {
			.timestamp_ms = timestamp_ms,
			.dropped_samples = dropped_samples,
			.reason = reason,
		},
	};

	max30102_runtime.last_stream_gap_reason = reason;

	if (max30102_stream_queue_push(&gap)) {
		return true;
	}

	max30102_queue_pending_stream_gap(reason, dropped_samples);
	return false;
}

/* 为被丢弃的样本推进逻辑时间戳，保持后续样本时间连续。 */
static void max30102_consume_dropped_sample_timestamps(size_t dropped_samples)
{
	while (dropped_samples-- > 0U) {
		(void)max30102_acquisition_next_timestamp_ms();
	}
}

/* 记录一次驱动错误，并把它转换为算法线程可见的缺口事件。 */
static void max30102_emit_driver_error_gap(int err)
{
	max30102_runtime.driver_error_count++;
	(void)max30102_emit_stream_gap(max30102_acquisition_next_timestamp_ms(),
				       MAX30102_STREAM_GAP_DRIVER_ERROR, 1U);
	ARG_UNUSED(err);
}

/* 持续从 MAX30102 驱动批量取样，并转换为带时间戳的流队列条目。 */
void max30102_acquisition_thread(void *arg1, void *arg2, void *arg3)
{
	struct max30102_fifo_sample samples[16];
	struct max30102_stream_item item;
	size_t sample_count = 0U;
	int ret;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		bool wake_fifo_overflowed = false;
		bool overflow_gap_emitted = false;

		if (!atomic_get(&max30102_runtime.enabled)) {
			k_sleep(K_MSEC(100));
			continue;
		}

		if (max30102_runtime.pending_stream_gap.active) {
			if (!max30102_try_flush_pending_stream_gap()) {
				k_sleep(K_MSEC(MAX30102_ACQUISITION_POLL_PERIOD_MS));
				continue;
			}

			continue;
		}

		if (max30102_wait_data_ready(max30102_runtime.dev, K_MSEC(20U)) == 0) {
			max30102_runtime.irq_wakeup_count++;
		}

		do {
			bool batch_overflowed = false;

			sample_count = 0U;
			ret = max30102_read_fifo_batch(max30102_runtime.dev, samples,
						       ARRAY_SIZE(samples), &sample_count,
						       &batch_overflowed);
			if (ret == -EAGAIN) {
				if (++max30102_empty_fetch_count >=
				    MAX30102_ACQUISITION_EMPTY_FETCH_THRESHOLD) {
					max30102_runtime.driver_error_count++;
					max30102_queue_pending_stream_gap(
						MAX30102_STREAM_GAP_DRIVER_ERROR, 1U);
					max30102_empty_fetch_count = 0U;
				}

				k_sleep(K_MSEC(MAX30102_ACQUISITION_POLL_PERIOD_MS));
				break;
			}
			if (ret != 0) {
				max30102_emit_driver_error_gap(ret);
				max30102_empty_fetch_count = 0U;
				k_sleep(K_MSEC(MAX30102_ACQUISITION_POLL_PERIOD_MS));
				break;
			}

			max30102_empty_fetch_count = 0U;

			if (batch_overflowed) {
				wake_fifo_overflowed = true;
				if (!overflow_gap_emitted) {
					max30102_runtime.fifo_overflow_count++;
					if (!max30102_emit_stream_gap(
						    max30102_acquisition_next_timestamp_ms(),
						    MAX30102_STREAM_GAP_FIFO_OVERFLOW, 0U)) {
						k_sleep(K_MSEC(MAX30102_ACQUISITION_POLL_PERIOD_MS));
						break;
					}
					overflow_gap_emitted = true;
				}
			}

			for (size_t i = 0U; i < sample_count; ++i) {
				item.type = MAX30102_ITEM_SAMPLE;
				item.sample.timestamp_ms = max30102_acquisition_next_timestamp_ms();
				item.sample.red = samples[i].red;
				item.sample.ir = samples[i].ir;
				item.sample.seq = max30102_runtime.next_seq++;
				item.sample.source_flags = wake_fifo_overflowed ? 1U : 0U;

				if (!max30102_stream_queue_push(&item)) {
					const size_t dropped_samples = sample_count - i;

					max30102_runtime.queue_drop_count += dropped_samples;
					max30102_emit_stream_gap(item.sample.timestamp_ms,
							 MAX30102_STREAM_GAP_QUEUE_OVERFLOW,
							 dropped_samples);
					if (dropped_samples > 1U) {
						max30102_consume_dropped_sample_timestamps(
							dropped_samples - 1U);
					}
					break;
				}
			}

			if (max30102_runtime.pending_stream_gap.active) {
				break;
			}
		} while ((ret == 0) && (sample_count == ARRAY_SIZE(samples)));
	}
}
