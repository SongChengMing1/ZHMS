#ifndef PPG_PEAK_DETECT_H_
#define PPG_PEAK_DETECT_H_

#include <stdbool.h>
#include <stdint.h>

struct ppg_peak_detect_state {
	bool primed;
	uint8_t sample_count;
	int prev2_filtered_ir;
	int prev1_filtered_ir;
	uint32_t prev2_timestamp_ms;
	uint32_t prev1_timestamp_ms;
	float signal_level;
	float noise_level;
	uint32_t last_peak_timestamp_ms;
	bool have_last_peak;
	uint16_t recent_ibi_ms[3];
	uint8_t recent_ibi_count;
	uint8_t recent_ibi_index;
	bool have_startup_reference_ibi;
	uint16_t startup_reference_ibi_ms;
	bool have_last_accepted_ibi;
	uint16_t last_accepted_ibi_ms;
	uint8_t invalid_candidate_count;
	bool signal_valid;
	bool current_min_amplitude_ok;
	bool have_pending_peak;
	uint32_t pending_peak_timestamp_ms;
	int pending_peak_value;
	uint32_t pending_window_start_ms;
};

struct ppg_peak_detect_output {
	bool peak_detected;
	bool ibi_valid;
	bool signal_valid;
	uint32_t peak_timestamp_ms;
	uint32_t confirmed_peak_timestamp_ms;
	uint32_t ibi_ms;
	int threshold;
};

void ppg_peak_detect_reset(struct ppg_peak_detect_state *state);
int ppg_peak_detect_push(struct ppg_peak_detect_state *state, uint32_t timestamp_ms,
			 int filtered_ir, struct ppg_peak_detect_output *output);

#endif /* PPG_PEAK_DETECT_H_ */
