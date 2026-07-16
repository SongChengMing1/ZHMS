#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "health_metrics.h"
#include "max30102_service_internal.h"
#include "ppg_peak_detect.h"
#include "ppg_preprocess.h"

#define MAX30102_FINGER_ON_THRESHOLD 20000
#define MAX30102_FINGER_OFF_THRESHOLD 5000
#define MAX30102_FINGER_ON_DEBOUNCE_MS 400U
#define MAX30102_FINGER_OFF_DEBOUNCE_MS 200U
#define MAX30102_FINGER_WARMUP_MS 2000U
#define MAX30102_FINGER_ACTIVE_MIN_P2P_1S 180
#define MAX30102_FINGER_P2P_WINDOW_MS 1000U
#define MAX30102_FINGER_P2P_WINDOW_SAMPLES 100U

enum max30102_finger_contact_state {
	MAX30102_FINGER_STATE_NO_FINGER = 0,
	MAX30102_FINGER_STATE_WARMUP,
	MAX30102_FINGER_STATE_ACTIVE,
};

static struct health_metrics_state health_metrics_state;
static struct ppg_preprocess_state ppg_ir_state;
static struct ppg_peak_detect_state ppg_peak_state;
static enum max30102_finger_contact_state max30102_finger_state;
static uint32_t max30102_finger_state_since_ms;
static uint32_t max30102_finger_on_candidate_since_ms;
static uint32_t max30102_finger_off_candidate_since_ms;
static int max30102_filtered_window[MAX30102_FINGER_P2P_WINDOW_SAMPLES];
static uint8_t max30102_filtered_window_count;
static uint8_t max30102_filtered_window_index;
static bool max30102_algorithm_initialized;

/* 清空用于判断手指接触稳定性的滤波窗口。 */
static void max30102_reset_filtered_window(void)
{
	memset(max30102_filtered_window, 0, sizeof(max30102_filtered_window));
	max30102_filtered_window_count = 0U;
	max30102_filtered_window_index = 0U;
}

/* 把最新滤波样本写入环形窗口，供接触状态机计算幅值范围。 */
static void max30102_push_filtered_window(int filtered_ir)
{
	max30102_filtered_window[max30102_filtered_window_index] = filtered_ir;
	max30102_filtered_window_index =
		(max30102_filtered_window_index + 1U) %
		MAX30102_FINGER_P2P_WINDOW_SAMPLES;

	if (max30102_filtered_window_count < MAX30102_FINGER_P2P_WINDOW_SAMPLES) {
		max30102_filtered_window_count++;
	}
}

/* 计算当前滤波窗口的峰峰值，用于判断信号是否进入稳定按压状态。 */
static int max30102_filtered_window_p2p(void)
{
	int min_value;
	int max_value;

	if (max30102_filtered_window_count == 0U) {
		return 0;
	}

	min_value = max30102_filtered_window[0];
	max_value = max30102_filtered_window[0];

	for (uint8_t i = 1U; i < max30102_filtered_window_count; ++i) {
		int sample = max30102_filtered_window[i];

		if (sample < min_value) {
			min_value = sample;
		}

		if (sample > max_value) {
			max_value = sample;
		}
	}

	return max_value - min_value;
}

/* 切换到未接触状态，并重置整条信号处理链的会话状态。 */
static void max30102_enter_no_finger(uint32_t timestamp_ms)
{
	max30102_finger_state = MAX30102_FINGER_STATE_NO_FINGER;
	max30102_finger_state_since_ms = timestamp_ms;
	max30102_finger_on_candidate_since_ms = 0U;
	max30102_finger_off_candidate_since_ms = 0U;
	ppg_preprocess_reset(&ppg_ir_state);
	ppg_peak_detect_reset(&ppg_peak_state);
	health_metrics_reset(&health_metrics_state);
	max30102_reset_filtered_window();
	max30102_service_clear_health_snapshot();
}

/* 以未接触状态重新初始化算法侧所有运行时上下文。 */
static void max30102_reset_full_pipeline(void)
{
	max30102_enter_no_finger(0U);
}

/* 进入按压预热状态，为后续 ACTIVE 判定重新建立观测窗口。 */
static void max30102_enter_warmup(uint32_t timestamp_ms)
{
	max30102_finger_state = MAX30102_FINGER_STATE_WARMUP;
	max30102_finger_state_since_ms = timestamp_ms;
	max30102_finger_on_candidate_since_ms = 0U;
	max30102_finger_off_candidate_since_ms = 0U;
	max30102_reset_filtered_window();
}

/* 进入稳定按压状态，并重置判峰器以开始正式输出 IBI。 */
static void max30102_enter_active(uint32_t timestamp_ms)
{
	max30102_finger_state = MAX30102_FINGER_STATE_ACTIVE;
	max30102_finger_state_since_ms = timestamp_ms;
	max30102_finger_on_candidate_since_ms = 0U;
	max30102_finger_off_candidate_since_ms = 0U;
	max30102_reset_filtered_window();
	ppg_peak_detect_reset(&ppg_peak_state);
}

/* 根据原始 IR 和滤波 IR 更新手指接触状态机及其防抖计时。 */
static void max30102_update_finger_contact_state(uint32_t timestamp_ms, int raw_ir,
						 int filtered_ir)
{
	int filtered_ir_p2p_1s;

	max30102_push_filtered_window(filtered_ir);
	filtered_ir_p2p_1s = max30102_filtered_window_p2p();

	if (raw_ir < MAX30102_FINGER_OFF_THRESHOLD) {
		if (max30102_finger_off_candidate_since_ms == 0U) {
			max30102_finger_off_candidate_since_ms = timestamp_ms;
		}

		max30102_finger_on_candidate_since_ms = 0U;

		if (max30102_finger_state != MAX30102_FINGER_STATE_NO_FINGER &&
		    timestamp_ms - max30102_finger_off_candidate_since_ms >=
			    MAX30102_FINGER_OFF_DEBOUNCE_MS) {
			max30102_enter_no_finger(timestamp_ms);
			return;
		}
	} else {
		max30102_finger_off_candidate_since_ms = 0U;
	}

	if (raw_ir > MAX30102_FINGER_ON_THRESHOLD) {
		if (max30102_finger_on_candidate_since_ms == 0U) {
			max30102_finger_on_candidate_since_ms = timestamp_ms;
		}

		if (max30102_finger_state == MAX30102_FINGER_STATE_NO_FINGER &&
		    timestamp_ms - max30102_finger_on_candidate_since_ms >=
			    MAX30102_FINGER_ON_DEBOUNCE_MS) {
			max30102_enter_warmup(timestamp_ms);
			return;
		}
	} else {
		max30102_finger_on_candidate_since_ms = 0U;
	}

	if (max30102_finger_state == MAX30102_FINGER_STATE_WARMUP &&
	    timestamp_ms - max30102_finger_state_since_ms >= MAX30102_FINGER_WARMUP_MS &&
	    filtered_ir_p2p_1s >= MAX30102_FINGER_ACTIVE_MIN_P2P_1S) {
		max30102_enter_active(timestamp_ms);
	}
}

/* 将当前健康指标按既定串口协议格式化输出为一行摘要。 */
static void max30102_print_metrics_summary(
	const struct health_metrics_output *metrics)
{
	char bpm_buf[12];
	char hrv_buf[16];

	if (metrics == NULL || !metrics->summary_due) {
		return;
	}

	if (metrics->bpm_valid) {
		snprintk(bpm_buf, sizeof(bpm_buf), "%u", metrics->bpm);
	} else {
		snprintk(bpm_buf, sizeof(bpm_buf), "na");
	}

	if (metrics->hrv_valid) {
		snprintk(hrv_buf, sizeof(hrv_buf), "%u.%u",
			 metrics->hrv_rmssd_ms_x10 / 10U,
			 metrics->hrv_rmssd_ms_x10 % 10U);
	} else {
		snprintk(hrv_buf, sizeof(hrv_buf), "na");
	}

	printk("%s,%s,%s\n", bpm_buf, hrv_buf,
	       health_metrics_stress_level_str(metrics->stress_level));
}

/* 处理采集侧上报的缺口事件，并把算法链重置到安全初始状态。 */
static void max30102_algorithm_handle_stream_gap(
	const struct max30102_stream_gap *gap)
{
	ARG_UNUSED(gap);

	ppg_preprocess_reset(&ppg_ir_state);
	ppg_peak_detect_reset(&ppg_peak_state);
	health_metrics_reset(&health_metrics_state);
	max30102_enter_no_finger(0U);
}

/* 对单个样本依次执行预处理、接触判定、判峰和健康指标更新。 */
static void max30102_process_sample(uint32_t timestamp_ms, int red, int ir)
{
	struct ppg_preprocess_output filtered;
	struct ppg_peak_detect_output peak;
	struct health_metrics_output metrics;
	int ret;

	ARG_UNUSED(red);

	ret = ppg_preprocess_apply(&ppg_ir_state, ir, &filtered);
	if (ret != 0) {
		return;
	}

	max30102_update_finger_contact_state(timestamp_ms, ir, filtered.filtered_ir);

	if (max30102_finger_state == MAX30102_FINGER_STATE_ACTIVE) {
		peak = (struct ppg_peak_detect_output){0};
		ret = ppg_peak_detect_push(&ppg_peak_state, timestamp_ms,
					   filtered.filtered_ir, &peak);
		if (ret != 0) {
			return;
		}

		metrics = (struct health_metrics_output){0};
		if (peak.peak_detected && peak.ibi_valid && peak.ibi_ms > 0U) {
			ret = health_metrics_push(&health_metrics_state,
						  timestamp_ms, peak.ibi_ms,
						  &metrics);
		} else {
			ret = health_metrics_advance(&health_metrics_state,
						     timestamp_ms, &metrics);
		}

		if (ret != 0) {
			return;
		}

		if (metrics.bpm_valid) {
			max30102_service_set_health_snapshot(true, metrics.bpm);
		} else {
			max30102_service_clear_health_snapshot();
		}

		max30102_print_metrics_summary(&metrics);
	}
}

/* 消费采集线程写入的流队列，并驱动整条 MAX30102 算法处理链。 */
void max30102_algorithm_thread(void *arg1, void *arg2, void *arg3)
{
	struct max30102_stream_item item;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	if (!max30102_algorithm_initialized) {
		max30102_reset_full_pipeline();
		max30102_algorithm_initialized = true;
	}

	while (1) {
		if (!atomic_get(&max30102_runtime.enabled)) {
			k_sleep(K_MSEC(100));
			continue;
		}

		if (k_sem_take(&max30102_runtime.stream_sem, K_MSEC(20)) != 0) {
			continue;
		}

		while (max30102_stream_queue_pop(&item)) {
			if (item.type == MAX30102_ITEM_STREAM_GAP ||
			    item.type == MAX30102_ITEM_DRIVER_ERROR) {
				max30102_algorithm_handle_stream_gap(&item.gap);
				continue;
			}

			if (item.type != MAX30102_ITEM_SAMPLE) {
				continue;
			}

			max30102_process_sample(item.sample.timestamp_ms,
						item.sample.red, item.sample.ir);
		}
	}
}
