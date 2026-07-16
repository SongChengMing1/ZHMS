#include "ppg_preprocess.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>

#define PPG_BASELINE_ALPHA 0.03093f
#define PPG_LOWPASS_B0 0.016581932f
#define PPG_LOWPASS_B1 0.033163864f
#define PPG_LOWPASS_B2 0.016581932f
#define PPG_LOWPASS_A1 -1.604130149f
#define PPG_LOWPASS_A2 0.670457900f
#define PPG_MAX_STEP_DELTA 12000

/* 计算三个整数样本的中位值，用于抑制单点毛刺。 */
static int median3_int(int first, int second, int third)
{
	if (first > second) {
		int tmp = first;

		first = second;
		second = tmp;
	}

	if (second > third) {
		int tmp = second;

		second = third;
		third = tmp;
	}

	if (first > second) {
		int tmp = first;

		first = second;
		second = tmp;
	}

	return second;
}

/* 限制相邻滤波输出的跳变量，避免异常尖峰直接传到下游。 */
static int clamp_filtered_step(int current, int previous)
{
	int delta = current - previous;

	if (delta > PPG_MAX_STEP_DELTA) {
		return previous + PPG_MAX_STEP_DELTA;
	}

	if (delta < -PPG_MAX_STEP_DELTA) {
		return previous - PPG_MAX_STEP_DELTA;
	}

	return current;
}

/* 清空预处理状态机，重新开始中值、基线和低通滤波计算。 */
void ppg_preprocess_reset(struct ppg_preprocess_state *state)
{
	if (state == NULL) {
		return;
	}

	state->initialized = false;
	state->median_buf[0] = 0;
	state->median_buf[1] = 0;
	state->median_buf[2] = 0;
	state->median_count = 0;
	state->median_index = 0;
	state->baseline_ir = 0.0f;
	state->lp_x1 = 0.0f;
	state->lp_x2 = 0.0f;
	state->lp_y1 = 0.0f;
	state->lp_y2 = 0.0f;
	state->last_filtered_ir = 0;
}

/* 对原始 IR 样本执行中值去毛刺、基线去除和低通滤波处理。 */
int ppg_preprocess_apply(struct ppg_preprocess_state *state, int raw_ir,
			 struct ppg_preprocess_output *output)
{
	int median_ir;
	float median_value;
	float ac_ir;
	float lp_out;
	int filtered_ir;

	if (state == NULL || output == NULL) {
		return -EINVAL;
	}

	state->median_buf[state->median_index] = raw_ir;
	state->median_index = (state->median_index + 1U) % 3U;
	if (state->median_count < 3U) {
		state->median_count++;
	}

	if (state->median_count < 3U) {
		median_ir = raw_ir;
	} else {
		median_ir = median3_int(state->median_buf[0], state->median_buf[1],
					state->median_buf[2]);
	}

	median_value = (float)median_ir;

	if (!state->initialized) {
		state->initialized = true;
		state->baseline_ir = median_value;
		state->lp_x1 = 0.0f;
		state->lp_x2 = 0.0f;
		state->lp_y1 = 0.0f;
		state->lp_y2 = 0.0f;
		state->last_filtered_ir = 0;
		output->filtered_ir = 0;
		return 0;
	}

	state->baseline_ir += PPG_BASELINE_ALPHA *
			       (median_value - state->baseline_ir);
	ac_ir = median_value - state->baseline_ir;

	lp_out = (PPG_LOWPASS_B0 * ac_ir) +
		 (PPG_LOWPASS_B1 * state->lp_x1) +
		 (PPG_LOWPASS_B2 * state->lp_x2) -
		 (PPG_LOWPASS_A1 * state->lp_y1) -
		 (PPG_LOWPASS_A2 * state->lp_y2);

	state->lp_x2 = state->lp_x1;
	state->lp_x1 = ac_ir;
	state->lp_y2 = state->lp_y1;
	state->lp_y1 = lp_out;

	filtered_ir = (int)lroundf(lp_out);
	filtered_ir = clamp_filtered_step(filtered_ir,
				 state->last_filtered_ir);

	state->last_filtered_ir = filtered_ir;
	output->filtered_ir = filtered_ir;
	return 0;
}
