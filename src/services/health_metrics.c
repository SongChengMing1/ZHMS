#include "health_metrics.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/sys/util.h>

#define BPM_MIN_MS 300U
#define BPM_MAX_MS 2000U
#define HRV_MIN_MS 400U
#define HRV_MAX_MS 1500U
#define HRV_REFERENCE_DEVIATION_PERCENT 30U
#define HRV_STABLE_DEVIATION_PERCENT 25U
#define STRESS_HIGH_THRESHOLD_MS_X10 250U
#define STRESS_LOW_THRESHOLD_MS_X10 500U
#define HEALTH_METRICS_MEDIAN_MAX_SAMPLES \
	HEALTH_METRICS_HRV_REFERENCE_SAMPLE_WINDOW

BUILD_ASSERT(HEALTH_METRICS_BPM_ACCEPTED_WINDOW <=
		     HEALTH_METRICS_MEDIAN_MAX_SAMPLES,
	     "median_u16 scratch buffer must cover BPM accepted samples");
BUILD_ASSERT(HEALTH_METRICS_HRV_REFERENCE_SAMPLE_WINDOW <=
		     HEALTH_METRICS_MEDIAN_MAX_SAMPLES,
	     "median_u16 scratch buffer must cover HRV reference samples");

/* 将绝对时间映射到当前测量会话的相对时间轴，并初始化摘要节拍。 */
static uint32_t relative_time_ms(struct health_metrics_state *state, uint32_t timestamp_ms)
{
	if (!state->have_time_origin) {
		state->have_time_origin = true;
		state->time_origin_ms = timestamp_ms;
		state->next_summary_ms = HEALTH_METRICS_HRV_REFRESH_MS;
	}

	return timestamp_ms - state->time_origin_ms;
}

/* 将浮点结果按四舍六入五成双规则收敛为无符号 16 位整数。 */
static uint16_t round_half_to_even_u16(float value)
{
	uint32_t lower = (uint32_t)value;
	float fraction = value - (float)lower;

	if (fraction < 0.5f) {
		return (uint16_t)MIN(lower, UINT16_MAX);
	}

	if (fraction > 0.5f) {
		return (uint16_t)MIN(lower + 1U, UINT16_MAX);
	}

	if ((lower & 1U) == 0U) {
		return (uint16_t)MIN(lower, UINT16_MAX);
	}

	return (uint16_t)MIN(lower + 1U, UINT16_MAX);
}

/* 对无符号 64 位整数执行带四舍五入的整除。 */
static uint64_t round_div_u64(uint64_t numerator, uint32_t denominator)
{
	if (denominator == 0U) {
		return 0U;
	}

	return (numerator + (uint64_t)(denominator / 2U)) / (uint64_t)denominator;
}

/* 计算无符号 64 位值的整数平方根，用于 RMSSD 和 SDNN 结果收敛。 */
static uint16_t isqrt_u64(uint64_t value)
{
	uint64_t result = 0U;
	uint64_t bit = 1ULL << 62;

	while (bit > value) {
		bit >>= 2;
	}

	while (bit != 0U) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}

		bit >>= 2;
	}

	return (uint16_t)MIN(result, (uint64_t)UINT16_MAX);
}

/* 对短整型样本数组执行原地升序排序。 */
static void sort_u16(uint16_t *values, uint8_t count)
{
	for (uint8_t i = 1U; i < count; ++i) {
		uint16_t current = values[i];
		int j = (int)i - 1;

		while (j >= 0 && values[j] > current) {
			values[j + 1] = values[j];
			--j;
		}

		values[j + 1] = current;
	}
}

/* 计算样本数组的中位数，供 BPM 和 HRV 参考窗口复用。 */
static float median_u16(const uint16_t *values, uint8_t count)
{
	uint16_t sorted[HEALTH_METRICS_MEDIAN_MAX_SAMPLES];

	if (count == 0U || count > ARRAY_SIZE(sorted)) {
		return 0.0f;
	}

	for (uint8_t i = 0U; i < count; ++i) {
		sorted[i] = values[i];
	}

	sort_u16(sorted, count);

	if ((count & 1U) != 0U) {
		return (float)sorted[count / 2U];
	}

	return ((float)sorted[(count / 2U) - 1U] + (float)sorted[count / 2U]) * 0.5f;
}

/* 判断一个 NN 间期是否落在 BPM 允许的时间范围内。 */
static bool is_bpm_valid_candidate(uint16_t nn_ms)
{
	return nn_ms >= BPM_MIN_MS && nn_ms <= BPM_MAX_MS;
}

/* 判断一个 NN 间期是否落在 HRV 允许的时间范围内。 */
static bool is_hrv_range_candidate(uint16_t nn_ms)
{
	return nn_ms >= HRV_MIN_MS && nn_ms <= HRV_MAX_MS;
}

/* 检查输入时间戳是否倒退，保护状态机只接受单调时间。 */
static bool timestamp_is_regressing(const struct health_metrics_state *state,
				    uint32_t timestamp_ms)
{
	return state->have_last_timestamp_ms &&
	       timestamp_ms < state->last_timestamp_ms;
}

/* 记录最近一次处理过的绝对时间戳，供单调性校验复用。 */
static void remember_timestamp(struct health_metrics_state *state, uint32_t timestamp_ms)
{
	state->have_last_timestamp_ms = true;
	state->last_timestamp_ms = timestamp_ms;
}

/* 清空 BPM 已接受样本窗口及其显示节拍计数。 */
static void clear_accepted_samples(struct health_metrics_state *state)
{
	state->accepted_sample_count = 0U;
	state->accepted_sample_index = 0U;
	state->accepted_since_last_display = 0U;
	memset(state->accepted_samples, 0, sizeof(state->accepted_samples));
}

/* 使 BPM 输出失效，并重置相关显示与 accepted 样本状态。 */
static void invalidate_bpm(struct health_metrics_state *state)
{
	clear_accepted_samples(state);
	state->have_bpm_last_accepted_timestamp = false;
	state->have_bpm_last_display_timestamp = false;
	state->bpm_value = 0U;
	state->bpm_valid = false;
	state->bpm_mode = HEALTH_METRICS_BPM_MODE_INVALID;
}

/* 根据超时规则撤销长时间未更新的 BPM 结果。 */
static void reconcile_bpm_timeout(struct health_metrics_state *state, uint32_t timestamp_ms)
{
	if (!state->have_bpm_last_accepted_timestamp) {
		return;
	}

	if (timestamp_ms - state->bpm_last_accepted_timestamp_ms >=
	    HEALTH_METRICS_BPM_INVALIDATE_TIMEOUT_MS) {
		invalidate_bpm(state);
	}
}

/* 把新的 BPM 有效样本写入环形 accepted 窗口。 */
static void append_accepted_sample(struct health_metrics_state *state,
				   uint32_t timestamp_ms, uint16_t nn_ms)
{
	state->accepted_samples[state->accepted_sample_index] =
		(struct health_metrics_timed_value){
			.timestamp_ms = timestamp_ms,
			.nn_ms = nn_ms,
		};
	state->accepted_sample_index =
		(state->accepted_sample_index + 1U) %
		HEALTH_METRICS_BPM_ACCEPTED_WINDOW;

	if (state->accepted_sample_count < HEALTH_METRICS_BPM_ACCEPTED_WINDOW) {
		state->accepted_sample_count++;
	}
}

/* 用 accepted 样本中位数判断当前 NN 是否仍属于稳定 BPM 节律。 */
static bool is_bpm_rhythm_candidate(const struct health_metrics_state *state,
				    uint16_t nn_ms)
{
	uint16_t values[HEALTH_METRICS_BPM_ACCEPTED_WINDOW];
	float reference;
	uint32_t delta;

	if (!is_bpm_valid_candidate(nn_ms)) {
		return false;
	}

	if (state->accepted_sample_count == 0U) {
		return true;
	}

	for (uint8_t i = 0U; i < state->accepted_sample_count; ++i) {
		values[i] = state->accepted_samples[i].nn_ms;
	}

	reference = median_u16(values, state->accepted_sample_count);
	delta = (nn_ms > (uint16_t)reference) ? (nn_ms - (uint16_t)reference) :
						((uint16_t)reference - nn_ms);
	return ((uint64_t)delta * 100ULL) <=
	       ((uint64_t)((uint16_t)reference) *
		HEALTH_METRICS_BPM_ACCEPTED_DEVIATION_PERCENT);
}

/* 依据 accepted 窗口的中位 NN 估算当前候选 BPM 值。 */
static uint16_t candidate_bpm_from_accepted_samples(
	const struct health_metrics_state *state)
{
	uint16_t values[HEALTH_METRICS_BPM_ACCEPTED_WINDOW];

	if (state->accepted_sample_count < HEALTH_METRICS_BPM_ACCEPTED_WINDOW) {
		return 0U;
	}

	for (uint8_t i = 0U; i < HEALTH_METRICS_BPM_ACCEPTED_WINDOW; ++i) {
		values[i] = state->accepted_samples[i].nn_ms;
	}

	return round_half_to_even_u16(
		60000.0f / median_u16(values, HEALTH_METRICS_BPM_ACCEPTED_WINDOW));
}

/* 按刷新节拍平滑更新对外展示的 BPM 数值。 */
static void reconcile_bpm_display(struct health_metrics_state *state,
				  uint32_t timestamp_ms)
{
	uint16_t candidate_bpm;
	int32_t delta;

	if (!state->bpm_valid || !state->have_bpm_last_display_timestamp) {
		return;
	}

	if (timestamp_ms - state->bpm_last_display_timestamp_ms <
	    HEALTH_METRICS_BPM_DISPLAY_REFRESH_MS) {
		return;
	}

	if (state->accepted_since_last_display <
	    HEALTH_METRICS_BPM_MIN_NEW_ACCEPTED_PER_REFRESH) {
		state->accepted_since_last_display = 0U;
		state->bpm_last_display_timestamp_ms = timestamp_ms;
		return;
	}

	candidate_bpm = candidate_bpm_from_accepted_samples(state);
	if (candidate_bpm == 0U) {
		return;
	}

	delta = (int32_t)candidate_bpm - (int32_t)state->bpm_value;
	if (((delta >= 0) ? delta : -delta) <=
	    (int32_t)HEALTH_METRICS_BPM_DISPLAY_STEP_LIMIT) {
		state->bpm_value = candidate_bpm;
	} else if (delta > 0) {
		state->bpm_value += HEALTH_METRICS_BPM_DISPLAY_STEP_LIMIT;
	} else {
		state->bpm_value -= HEALTH_METRICS_BPM_DISPLAY_STEP_LIMIT;
	}

	state->accepted_since_last_display = 0U;
	state->bpm_last_display_timestamp_ms = timestamp_ms;
	state->bpm_mode = HEALTH_METRICS_BPM_MODE_NORMAL;
}

/* 追加一条 HRV 候选样本，并在窗口满时丢弃最旧数据。 */
static void append_hrv_sample(struct health_metrics_state *state, uint32_t timestamp_ms,
			      uint16_t nn_ms)
{
	if (state->hrv_sample_count == HEALTH_METRICS_MAX_WINDOW_SAMPLES) {
		memmove(&state->hrv_samples[0], &state->hrv_samples[1],
			sizeof(state->hrv_samples[0]) *
				(HEALTH_METRICS_MAX_WINDOW_SAMPLES - 1U));
		state->hrv_sample_count--;
	}

	state->hrv_samples[state->hrv_sample_count++] =
		(struct health_metrics_timed_value){
			.timestamp_ms = timestamp_ms,
			.nn_ms = nn_ms,
		};
}

/* 剪裁超出稳定 HRV 统计窗口的历史样本。 */
static void trim_hrv_samples(struct health_metrics_state *state, uint32_t timestamp_ms)
{
	while (state->hrv_sample_count > 0U &&
	       timestamp_ms - state->hrv_samples[0].timestamp_ms >
		       HEALTH_METRICS_HRV_STABLE_WINDOW_MS) {
		memmove(&state->hrv_samples[0], &state->hrv_samples[1],
			sizeof(state->hrv_samples[0]) *
				(state->hrv_sample_count - 1U));
		state->hrv_sample_count--;
	}
}

/* 判断当前 HRV 样本是否已经覆盖指定统计时间窗口。 */
static bool window_has_coverage(const struct health_metrics_state *state,
				uint32_t timestamp_ms, uint32_t window_ms)
{
	uint32_t cutoff_ms = (timestamp_ms > window_ms) ? (timestamp_ms - window_ms) : 0U;
	bool have_sample_at_or_before_cutoff = false;
	uint32_t oldest_in_window_ms = 0U;
	uint32_t newest_in_window_ms = 0U;
	bool have_window_sample = false;

	for (uint16_t i = 0U; i < state->hrv_sample_count; ++i) {
		uint32_t sample_ts = state->hrv_samples[i].timestamp_ms;

		if (sample_ts <= cutoff_ms) {
			have_sample_at_or_before_cutoff = true;
		}

		if (sample_ts >= cutoff_ms) {
			if (!have_window_sample) {
				oldest_in_window_ms = sample_ts;
				have_window_sample = true;
			}

			newest_in_window_ms = sample_ts;
		}
	}

	if (!have_window_sample || state->hrv_sample_count < 2U) {
		return false;
	}

	if (have_sample_at_or_before_cutoff) {
		return true;
	}

	return newest_in_window_ms - oldest_in_window_ms >= window_ms;
}

/* 依据参考窗口中位数和覆盖情况筛选可用于 HRV 的 NN 样本。 */
static bool is_hrv_valid_candidate(const struct health_metrics_state *state,
				   uint32_t timestamp_ms, uint16_t nn_ms)
{
	uint16_t reference_values[HEALTH_METRICS_HRV_REFERENCE_SAMPLE_WINDOW];
	uint8_t reference_count;
	uint32_t deviation_percent;
	float reference_median;
	uint16_t reference_u16;
	uint32_t delta;

	if (!is_hrv_range_candidate(nn_ms)) {
		return false;
	}

	reference_count = (uint8_t)MIN(
		(uint16_t)HEALTH_METRICS_HRV_REFERENCE_SAMPLE_WINDOW,
		state->hrv_sample_count);
	if (reference_count == 0U) {
		return true;
	}

	for (uint8_t i = 0U; i < reference_count; ++i) {
		reference_values[i] =
			state->hrv_samples[state->hrv_sample_count - reference_count + i]
				.nn_ms;
	}

	deviation_percent =
		window_has_coverage(state, timestamp_ms,
				    HEALTH_METRICS_HRV_STABLE_WINDOW_MS) ?
			HRV_STABLE_DEVIATION_PERCENT :
			HRV_REFERENCE_DEVIATION_PERCENT;
	reference_median = median_u16(reference_values, reference_count);
	reference_u16 = (uint16_t)reference_median;
	delta = (nn_ms > reference_u16) ? (nn_ms - reference_u16) :
					  (reference_u16 - nn_ms);

	return ((uint64_t)delta * 100ULL) <=
	       ((uint64_t)reference_u16 * deviation_percent);
}

/* 收集指定时间窗口内的 HRV 样本，并回报窗口覆盖是否成立。 */
static uint16_t collect_hrv_window(const struct health_metrics_state *state,
				   uint32_t timestamp_ms, uint32_t window_ms,
				   uint16_t *values, bool *coverage)
{
	uint32_t cutoff_ms = (timestamp_ms > window_ms) ? (timestamp_ms - window_ms) : 0U;
	bool have_sample_at_or_before_cutoff = false;
	uint32_t oldest_in_window_ms = 0U;
	uint32_t newest_in_window_ms = 0U;
	bool have_window_sample = false;
	uint16_t count = 0U;

	for (uint16_t i = 0U; i < state->hrv_sample_count; ++i) {
		const struct health_metrics_timed_value *sample = &state->hrv_samples[i];

		if (sample->timestamp_ms <= cutoff_ms) {
			have_sample_at_or_before_cutoff = true;
		}

		if (sample->timestamp_ms >= cutoff_ms) {
			if (!have_window_sample) {
				oldest_in_window_ms = sample->timestamp_ms;
				have_window_sample = true;
			}

			newest_in_window_ms = sample->timestamp_ms;
			values[count++] = sample->nn_ms;
		}
	}

	*coverage = false;
	if (count < 2U) {
		return count;
	}

	if (have_sample_at_or_before_cutoff) {
		*coverage = true;
		return count;
	}

	*coverage = newest_in_window_ms - oldest_in_window_ms >= window_ms;
	return count;
}

/* 根据窗口内相邻 NN 差值计算 RMSSD，结果保留到 0.1 ms。 */
static uint16_t compute_rmssd_ms_x10(const uint16_t *values, uint16_t count)
{
	uint64_t sum_sq = 0U;

	if (count < 2U) {
		return 0U;
	}

	for (uint16_t i = 1U; i < count; ++i) {
		int32_t diff = (int32_t)values[i] - (int32_t)values[i - 1U];

		sum_sq += (uint64_t)(diff * diff);
	}

	return isqrt_u64(round_div_u64(sum_sq * 100ULL, count - 1U));
}

/* 根据窗口内 NN 方差计算 SDNN，结果保留到 0.1 ms。 */
static uint16_t compute_sdnn_ms_x10(const uint16_t *values, uint16_t count)
{
	uint64_t sum = 0U;
	uint64_t mean_x10;
	uint64_t sum_sq = 0U;

	if (count < 2U) {
		return 0U;
	}

	for (uint16_t i = 0U; i < count; ++i) {
		sum += values[i];
	}

	mean_x10 = round_div_u64(sum * 10ULL, count);

	for (uint16_t i = 0U; i < count; ++i) {
		int64_t diff_x10 = ((int64_t)values[i] * 10LL) - (int64_t)mean_x10;

		sum_sq += (uint64_t)(diff_x10 * diff_x10);
	}

	return isqrt_u64(round_div_u64(sum_sq, count));
}

/* 按 RMSSD 阈值把当前 HRV 结果映射到压力等级。 */
static enum health_metrics_stress_level stress_level_from_rmssd_ms_x10(uint16_t rmssd_ms_x10)
{
	if (rmssd_ms_x10 < STRESS_HIGH_THRESHOLD_MS_X10) {
		return HEALTH_METRICS_STRESS_LEVEL_HIGH;
	}

	if (rmssd_ms_x10 <= STRESS_LOW_THRESHOLD_MS_X10) {
		return HEALTH_METRICS_STRESS_LEVEL_MEDIUM;
	}

	return HEALTH_METRICS_STRESS_LEVEL_LOW;
}

/* 依据参考窗、稳定窗和保持策略刷新 HRV 与压力输出状态。 */
static void reconcile_hrv_state(struct health_metrics_state *state, uint32_t timestamp_ms)
{
	uint16_t window_values[HEALTH_METRICS_MAX_WINDOW_SAMPLES];
	uint16_t window_count;
	bool coverage;

	window_count = collect_hrv_window(state, timestamp_ms,
					  HEALTH_METRICS_HRV_STABLE_WINDOW_MS,
					  window_values, &coverage);
	if (coverage && window_count >= 2U) {
		state->hrv_rmssd_ms_x10 =
			compute_rmssd_ms_x10(window_values, window_count);
		state->hrv_sdnn_ms_x10 =
			compute_sdnn_ms_x10(window_values, window_count);
		state->hrv_valid = true;
		state->hrv_mode = HEALTH_METRICS_HRV_MODE_STABLE;
		state->stress_level = stress_level_from_rmssd_ms_x10(
			state->hrv_rmssd_ms_x10);
		state->have_hrv_last_good_timestamp = true;
		state->hrv_last_good_timestamp_ms = timestamp_ms;
		return;
	}

	window_count = collect_hrv_window(state, timestamp_ms,
					  HEALTH_METRICS_HRV_REFERENCE_WINDOW_MS,
					  window_values, &coverage);
	if (coverage && window_count >= 2U) {
		state->hrv_rmssd_ms_x10 =
			compute_rmssd_ms_x10(window_values, window_count);
		state->hrv_sdnn_ms_x10 =
			compute_sdnn_ms_x10(window_values, window_count);
		state->hrv_valid = true;
		state->hrv_mode = HEALTH_METRICS_HRV_MODE_REFERENCE;
		state->stress_level = HEALTH_METRICS_STRESS_LEVEL_UNKNOWN;
		state->have_hrv_last_good_timestamp = true;
		state->hrv_last_good_timestamp_ms = timestamp_ms;
		return;
	}

	if (state->have_hrv_last_good_timestamp &&
	    timestamp_ms - state->hrv_last_good_timestamp_ms <=
		    HEALTH_METRICS_HRV_HOLD_MS) {
		state->hrv_valid = true;
		state->hrv_mode = HEALTH_METRICS_HRV_MODE_HOLD;
		return;
	}

	state->hrv_rmssd_ms_x10 = 0U;
	state->hrv_sdnn_ms_x10 = 0U;
	state->hrv_valid = false;
	state->hrv_mode = HEALTH_METRICS_HRV_MODE_INVALID;
	state->stress_level = HEALTH_METRICS_STRESS_LEVEL_UNKNOWN;
}

/* 判断当前相对时间是否到达下一次 5 秒摘要输出节拍。 */
static bool summary_due(struct health_metrics_state *state, uint32_t timestamp_ms)
{
	bool due = false;

	while (timestamp_ms >= state->next_summary_ms) {
		due = true;
		state->next_summary_ms += HEALTH_METRICS_HRV_REFRESH_MS;
	}

	return due;
}

/* 根据内部状态生成一次对外健康指标输出快照。 */
static void update_output(const struct health_metrics_state *state,
			  struct health_metrics_output *output,
			  uint32_t timestamp_ms, bool is_summary_due)
{
	*output = (struct health_metrics_output){
		.timestamp_ms = state->time_origin_ms + timestamp_ms,
		.bpm = state->bpm_value,
		.bpm_valid = state->bpm_valid,
		.bpm_mode = state->bpm_mode,
		.hrv_rmssd_ms_x10 = state->hrv_rmssd_ms_x10,
		.hrv_sdnn_ms_x10 = state->hrv_sdnn_ms_x10,
		.hrv_valid = state->hrv_valid,
		.hrv_mode = state->hrv_mode,
		.stress_level = state->stress_level,
		.nn_bpm_valid = state->latest_nn_bpm_valid,
		.nn_hrv_valid = state->latest_nn_hrv_valid,
		.summary_due = is_summary_due,
	};
}

/* 重置健康指标状态机，恢复冷启动和无效 HRV 初始状态。 */
void health_metrics_reset(struct health_metrics_state *state)
{
	if (state == NULL) {
		return;
	}

	memset(state, 0, sizeof(*state));
	state->bpm_mode = HEALTH_METRICS_BPM_MODE_COLD_START;
	state->hrv_mode = HEALTH_METRICS_HRV_MODE_INVALID;
	state->stress_level = HEALTH_METRICS_STRESS_LEVEL_UNKNOWN;
	state->next_summary_ms = HEALTH_METRICS_HRV_REFRESH_MS;
}

/* 推入一条新的有效 IBI，刷新 BPM、HRV 和压力状态并生成输出。 */
int health_metrics_push(struct health_metrics_state *state, uint32_t timestamp_ms,
			uint32_t ibi_ms, struct health_metrics_output *output)
{
	uint32_t relative_now;
	bool is_summary_due;
	uint16_t nn_ms;

	if (state == NULL || output == NULL || ibi_ms > UINT16_MAX) {
		return -EINVAL;
	}
	if (timestamp_is_regressing(state, timestamp_ms)) {
		return -EINVAL;
	}

	relative_now = relative_time_ms(state, timestamp_ms);
	state->latest_nn_bpm_valid = false;
	state->latest_nn_hrv_valid = false;

	reconcile_bpm_timeout(state, relative_now);
	trim_hrv_samples(state, relative_now);

	nn_ms = (uint16_t)ibi_ms;
	if (is_bpm_rhythm_candidate(state, nn_ms)) {
		append_accepted_sample(state, relative_now, nn_ms);
		state->have_bpm_last_accepted_timestamp = true;
		state->bpm_last_accepted_timestamp_ms = relative_now;
		state->latest_nn_bpm_valid = true;

		if (!state->bpm_valid &&
		    state->accepted_sample_count >=
			    HEALTH_METRICS_BPM_ACCEPTED_WINDOW) {
			state->bpm_value =
				candidate_bpm_from_accepted_samples(state);
			state->bpm_valid = (state->bpm_value > 0U);
			state->bpm_mode = state->bpm_valid ?
				HEALTH_METRICS_BPM_MODE_NORMAL :
				HEALTH_METRICS_BPM_MODE_COLD_START;
			state->have_bpm_last_display_timestamp =
				state->bpm_valid;
			state->bpm_last_display_timestamp_ms = relative_now;
			state->accepted_since_last_display = 0U;
		} else if (state->bpm_valid) {
			state->accepted_since_last_display++;
		}

		if (is_hrv_valid_candidate(state, relative_now, nn_ms)) {
			append_hrv_sample(state, relative_now, nn_ms);
			state->latest_nn_hrv_valid = true;
		}
	}

	reconcile_bpm_display(state, relative_now);
	reconcile_hrv_state(state, relative_now);
	is_summary_due = summary_due(state, relative_now);
	update_output(state, output, relative_now, is_summary_due);
	remember_timestamp(state, timestamp_ms);
	return 0;
}

/* 在没有新 IBI 时推进时间状态，并按当前窗口刷新输出。 */
int health_metrics_advance(struct health_metrics_state *state, uint32_t timestamp_ms,
			   struct health_metrics_output *output)
{
	uint32_t relative_now;
	bool is_summary_due;

	if (state == NULL || output == NULL) {
		return -EINVAL;
	}
	if (timestamp_is_regressing(state, timestamp_ms)) {
		return -EINVAL;
	}

	relative_now = relative_time_ms(state, timestamp_ms);
	state->latest_nn_bpm_valid = false;
	state->latest_nn_hrv_valid = false;

	reconcile_bpm_timeout(state, relative_now);
	trim_hrv_samples(state, relative_now);
	reconcile_bpm_display(state, relative_now);
	reconcile_hrv_state(state, relative_now);
	is_summary_due = summary_due(state, relative_now);
	update_output(state, output, relative_now, is_summary_due);
	remember_timestamp(state, timestamp_ms);
	return 0;
}

/* 将内部压力等级枚举转换为串口输出使用的中文字符串。 */
const char *health_metrics_stress_level_str(enum health_metrics_stress_level level)
{
	switch (level) {
	case HEALTH_METRICS_STRESS_LEVEL_HIGH:
		return "高";
	case HEALTH_METRICS_STRESS_LEVEL_MEDIUM:
		return "中";
	case HEALTH_METRICS_STRESS_LEVEL_LOW:
		return "低";
	default:
		return "na";
	}
}
