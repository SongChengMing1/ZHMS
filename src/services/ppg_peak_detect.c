#include "ppg_peak_detect.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>

#include <zephyr/sys/util.h>

#define PPG_THRESHOLD_FRACTION 0.45f
#define PPG_LEVEL_DECAY 0.875f
#define PPG_LEVEL_GAIN 0.125f
#define PPG_MIN_PEAK_DISTANCE_MS 360U
#define PPG_MIN_IBI_MS 400U
#define PPG_MAX_IBI_MS 1500U
#define PPG_IBI_DEVIATION_PCT 25U
#define PPG_STARTUP_IBI_DEVIATION_PCT 30U
#define PPG_INVALID_CANDIDATE_LIMIT 4U
#define PPG_MIN_PEAK_AMPLITUDE 110
#define PPG_CONFIRMATION_WINDOW_MS 180U
#define PPG_INITIAL_SIGNAL_LEVEL 8.0f
#define PPG_INITIAL_NOISE_LEVEL 0.0f

/* 用指数平滑方式更新信号或噪声电平估计。 */
static float blend_level(float old_value, float sample)
{
	return (PPG_LEVEL_DECAY * old_value) + (PPG_LEVEL_GAIN * sample);
}

/* 将浮点值按四舍六入五成双规则转换为整数阈值。 */
static int round_to_int(float value)
{
	float floor_value = floorf(value);
	float fraction = value - floor_value;
	int lower = (int)floor_value;

	if (fraction < 0.5f) {
		return lower;
	}

	if (fraction > 0.5f) {
		return lower + 1;
	}

	if ((lower & 1) == 0) {
		return lower;
	}

	return lower + 1;
}

/* 根据当前信号和噪声电平计算自适应判峰阈值。 */
static int current_threshold(const struct ppg_peak_detect_state *state)
{
	float threshold = state->noise_level +
			  (PPG_THRESHOLD_FRACTION *
			   (state->signal_level - state->noise_level));

	return round_to_int(threshold);
}

/* 计算三个 IBI 历史样本的中位值，作为稳定节律参考。 */
static uint16_t median3_u16(const uint16_t values[3])
{
	uint16_t a = values[0];
	uint16_t b = values[1];
	uint16_t c = values[2];

	if (a > b) {
		uint16_t tmp = a;

		a = b;
		b = tmp;
	}

	if (b > c) {
		uint16_t tmp = b;

		b = c;
		c = tmp;
	}

	if (a > b) {
		uint16_t tmp = a;

		a = b;
		b = tmp;
	}

	return b;
}

/* 判断数值是否落在参考值给定百分比偏差范围内。 */
static bool within_percent_u16(uint16_t value, uint16_t reference, uint16_t percent)
{
	uint32_t deviation;
	uint32_t limit;

	if (value >= reference) {
		deviation = (uint32_t)value - reference;
	} else {
		deviation = (uint32_t)reference - value;
	}

	limit = ((uint32_t)reference * percent) / 100U;

	return deviation <= limit;
}

/* 通过前后相邻样本关系判断当前历史点是否形成局部峰值。 */
static bool has_local_peak(const struct ppg_peak_detect_state *state, int current_filtered_ir)
{
	return state->prev1_filtered_ir > state->prev2_filtered_ir &&
	       state->prev1_filtered_ir >= current_filtered_ir;
}

/* 在启动阶段使用更宽松的参考规则筛选早期 IBI。 */
static bool startup_ibi_is_valid(const struct ppg_peak_detect_state *state,
				 uint16_t ibi_ms)
{
	if (!state->have_startup_reference_ibi) {
		return true;
	}

	if (!within_percent_u16(ibi_ms, state->startup_reference_ibi_ms,
				PPG_STARTUP_IBI_DEVIATION_PCT)) {
		return false;
	}

	if (state->recent_ibi_count == 1U) {
		return true;
	}

	return state->have_last_accepted_ibi &&
	       within_percent_u16(ibi_ms, state->last_accepted_ibi_ms,
				  PPG_STARTUP_IBI_DEVIATION_PCT);
}

/* 按量程和节律稳定性规则判断一个 IBI 是否可接受。 */
static bool ibi_is_valid(struct ppg_peak_detect_state *state, uint32_t ibi_ms)
{
	uint16_t history[ARRAY_SIZE(state->recent_ibi_ms)];
	uint16_t reference;

	if (ibi_ms < PPG_MIN_IBI_MS || ibi_ms > PPG_MAX_IBI_MS) {
		return false;
	}

	if (state->recent_ibi_count < ARRAY_SIZE(state->recent_ibi_ms)) {
		return startup_ibi_is_valid(state, (uint16_t)ibi_ms);
	}

	for (size_t i = 0U; i < ARRAY_SIZE(history); ++i) {
		history[i] = state->recent_ibi_ms[i];
	}

	reference = median3_u16(history);

	return within_percent_u16((uint16_t)ibi_ms, reference,
				  PPG_IBI_DEVIATION_PCT);
}

/* 追加一条已通过校验的 IBI 历史，用于后续节律参考。 */
static void append_valid_ibi(struct ppg_peak_detect_state *state, uint16_t ibi_ms)
{
	state->recent_ibi_ms[state->recent_ibi_index] = ibi_ms;
	state->recent_ibi_index = (state->recent_ibi_index + 1U) %
				  ARRAY_SIZE(state->recent_ibi_ms);
	if (state->recent_ibi_count < ARRAY_SIZE(state->recent_ibi_ms)) {
		state->recent_ibi_count++;
	}
}

/* 记录一条被接受的 IBI，并更新启动参考和最近有效节律。 */
static void record_accepted_ibi(struct ppg_peak_detect_state *state, uint16_t ibi_ms)
{
	if (!state->have_startup_reference_ibi) {
		state->have_startup_reference_ibi = true;
		state->startup_reference_ibi_ms = ibi_ms;
	}

	state->have_last_accepted_ibi = true;
	state->last_accepted_ibi_ms = ibi_ms;
	append_valid_ibi(state, ibi_ms);
}

/* 清除确认窗口内待决峰值的缓存状态。 */
static void clear_pending_peak(struct ppg_peak_detect_state *state)
{
	state->have_pending_peak = false;
	state->pending_peak_timestamp_ms = 0U;
	state->pending_peak_value = 0;
	state->pending_window_start_ms = 0U;
}

/* 将不合格候选峰计入噪声统计，并在连续失败过多时标记信号失效。 */
static void note_invalid_candidate(struct ppg_peak_detect_state *state,
				   int candidate_value)
{
	state->noise_level = blend_level(state->noise_level,
					 (float)candidate_value);
	state->invalid_candidate_count++;
	if (state->invalid_candidate_count > PPG_INVALID_CANDIDATE_LIMIT) {
		state->signal_valid = false;
	}
}

/* 确认等待窗口内的最终峰值，并在可能时输出对应 IBI。 */
static void confirm_pending_peak(struct ppg_peak_detect_state *state,
				 struct ppg_peak_detect_output *output)
{
	uint32_t ibi_ms = 0U;

	state->signal_level = blend_level(state->signal_level,
					 (float)state->pending_peak_value);
	state->invalid_candidate_count = 0U;
	state->signal_valid = true;
	output->peak_detected = true;
	output->signal_valid = true;
	output->peak_timestamp_ms = state->pending_peak_timestamp_ms;
	output->confirmed_peak_timestamp_ms = state->pending_peak_timestamp_ms;

	if (state->have_last_peak) {
		ibi_ms = state->pending_peak_timestamp_ms -
			 state->last_peak_timestamp_ms;
		output->ibi_ms = ibi_ms;
		if (ibi_is_valid(state, ibi_ms)) {
			output->ibi_valid = true;
			record_accepted_ibi(state, (uint16_t)ibi_ms);
		}
	}

	state->last_peak_timestamp_ms = state->pending_peak_timestamp_ms;
	state->have_last_peak = true;
	clear_pending_peak(state);
}

/* 清空判峰状态，恢复到冷启动观测阶段。 */
void ppg_peak_detect_reset(struct ppg_peak_detect_state *state)
{
	if (state == NULL) {
		return;
	}

	state->primed = false;
	state->sample_count = 0U;
	state->prev2_filtered_ir = 0;
	state->prev1_filtered_ir = 0;
	state->prev2_timestamp_ms = 0U;
	state->prev1_timestamp_ms = 0U;
	state->signal_level = PPG_INITIAL_SIGNAL_LEVEL;
	state->noise_level = PPG_INITIAL_NOISE_LEVEL;
	state->last_peak_timestamp_ms = 0U;
	state->have_last_peak = false;
	state->recent_ibi_ms[0] = 0U;
	state->recent_ibi_ms[1] = 0U;
	state->recent_ibi_ms[2] = 0U;
	state->recent_ibi_count = 0U;
	state->recent_ibi_index = 0U;
	state->have_startup_reference_ibi = false;
	state->startup_reference_ibi_ms = 0U;
	state->have_last_accepted_ibi = false;
	state->last_accepted_ibi_ms = 0U;
	state->invalid_candidate_count = 0U;
	state->signal_valid = false;
	state->current_min_amplitude_ok = false;
	clear_pending_peak(state);
}

/* 向判峰器推入一个滤波样本，并更新阈值、候选峰和 IBI 输出。 */
int ppg_peak_detect_push(struct ppg_peak_detect_state *state, uint32_t timestamp_ms,
			 int filtered_ir, struct ppg_peak_detect_output *output)
{
	int threshold = 0;
	int candidate_value;
	uint32_t candidate_timestamp_ms;
	bool candidate_qualifies = false;

	if (state == NULL || output == NULL) {
		return -EINVAL;
	}

	*output = (struct ppg_peak_detect_output){0};

	if (!state->primed) {
		state->primed = true;
		state->prev2_filtered_ir = filtered_ir;
		state->prev1_filtered_ir = filtered_ir;
		state->prev2_timestamp_ms = timestamp_ms;
		state->prev1_timestamp_ms = timestamp_ms;
		state->sample_count = 1U;
		return 0;
	}

	if (state->sample_count < 2U) {
		state->prev2_filtered_ir = state->prev1_filtered_ir;
		state->prev1_filtered_ir = filtered_ir;
		state->prev2_timestamp_ms = state->prev1_timestamp_ms;
		state->prev1_timestamp_ms = timestamp_ms;
		state->sample_count++;
		return 0;
	}

	if (state->have_last_peak &&
	    timestamp_ms - state->last_peak_timestamp_ms > PPG_MAX_IBI_MS) {
		state->signal_valid = false;
	}

	if (state->have_pending_peak &&
	    timestamp_ms - state->pending_window_start_ms >= PPG_CONFIRMATION_WINDOW_MS) {
		confirm_pending_peak(state, output);
	}

	threshold = current_threshold(state);
	output->threshold = threshold;
	output->signal_valid = state->signal_valid;

	if (!has_local_peak(state, filtered_ir)) {
		state->current_min_amplitude_ok = false;
		goto update_history;
	}

	candidate_value = state->prev1_filtered_ir;
	candidate_timestamp_ms = state->prev1_timestamp_ms;
	state->current_min_amplitude_ok = candidate_value >= PPG_MIN_PEAK_AMPLITUDE;

	candidate_qualifies = state->current_min_amplitude_ok &&
			     candidate_value > threshold &&
			     (!state->have_last_peak ||
			      candidate_timestamp_ms - state->last_peak_timestamp_ms >=
				      PPG_MIN_PEAK_DISTANCE_MS);

	if (state->have_pending_peak) {
		if (candidate_qualifies &&
		    candidate_value > state->pending_peak_value) {
			state->pending_peak_timestamp_ms = candidate_timestamp_ms;
			state->pending_peak_value = candidate_value;
		}
		goto update_history;
	}

	if (!candidate_qualifies) {
		note_invalid_candidate(state, candidate_value);
		goto update_history;
	}

	if (!state->have_pending_peak) {
		state->have_pending_peak = true;
		state->pending_peak_timestamp_ms = candidate_timestamp_ms;
		state->pending_peak_value = candidate_value;
		state->pending_window_start_ms = candidate_timestamp_ms;
	}

update_history:
	state->prev2_filtered_ir = state->prev1_filtered_ir;
	state->prev1_filtered_ir = filtered_ir;
	state->prev2_timestamp_ms = state->prev1_timestamp_ms;
	state->prev1_timestamp_ms = timestamp_ms;
	output->signal_valid = state->signal_valid;
	return 0;
}
