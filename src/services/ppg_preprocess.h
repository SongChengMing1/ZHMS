#ifndef PPG_PREPROCESS_H_
#define PPG_PREPROCESS_H_

#include <stdbool.h>
#include <stddef.h>

struct ppg_preprocess_state {
	bool initialized;
	int median_buf[3];
	size_t median_count;
	size_t median_index;
	float baseline_ir;
	float lp_x1;
	float lp_x2;
	float lp_y1;
	float lp_y2;
	int last_filtered_ir;
};

struct ppg_preprocess_output {
	int filtered_ir;
};

void ppg_preprocess_reset(struct ppg_preprocess_state *state);
int ppg_preprocess_apply(struct ppg_preprocess_state *state, int raw_ir,
			 struct ppg_preprocess_output *output);

#endif /* PPG_PREPROCESS_H_ */
