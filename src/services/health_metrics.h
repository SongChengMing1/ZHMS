#ifndef HEALTH_METRICS_H_
#define HEALTH_METRICS_H_

#include <stdbool.h>
#include <stdint.h>

#define HEALTH_METRICS_BPM_ACCEPTED_WINDOW 5U
#define HEALTH_METRICS_BPM_REFERENCE_WINDOW 7U
#define HEALTH_METRICS_BPM_DISPLAY_REFRESH_MS 5000U
#define HEALTH_METRICS_BPM_INVALIDATE_TIMEOUT_MS 10000U
#define HEALTH_METRICS_BPM_ACCEPTED_DEVIATION_PERCENT 18U
#define HEALTH_METRICS_BPM_DISPLAY_STEP_LIMIT 3U
#define HEALTH_METRICS_BPM_MIN_NEW_ACCEPTED_PER_REFRESH 2U
#define HEALTH_METRICS_HRV_REFERENCE_SAMPLE_WINDOW 7U
/* Compatibility aliases for older Task 5 naming. */
#define HEALTH_METRICS_BPM_RECENT_WINDOW HEALTH_METRICS_BPM_ACCEPTED_WINDOW
#define HEALTH_METRICS_BPM_TIMEOUT_HOLD_MS HEALTH_METRICS_BPM_INVALIDATE_TIMEOUT_MS
#define HEALTH_METRICS_HRV_REFERENCE_WINDOW_MS 30000U
#define HEALTH_METRICS_HRV_STABLE_WINDOW_MS 60000U
#define HEALTH_METRICS_HRV_REFRESH_MS 5000U
#define HEALTH_METRICS_HRV_HOLD_MS 15000U
#define HEALTH_METRICS_MAX_WINDOW_SAMPLES 208U

enum health_metrics_bpm_mode {
	HEALTH_METRICS_BPM_MODE_COLD_START = 0,
	HEALTH_METRICS_BPM_MODE_NORMAL,
	HEALTH_METRICS_BPM_MODE_HOLD,
	HEALTH_METRICS_BPM_MODE_INVALID,
};

enum health_metrics_hrv_mode {
	HEALTH_METRICS_HRV_MODE_INVALID = 0,
	HEALTH_METRICS_HRV_MODE_REFERENCE,
	HEALTH_METRICS_HRV_MODE_STABLE,
	HEALTH_METRICS_HRV_MODE_HOLD,
};

enum health_metrics_stress_level {
	HEALTH_METRICS_STRESS_LEVEL_UNKNOWN = 0,
	HEALTH_METRICS_STRESS_LEVEL_HIGH,
	HEALTH_METRICS_STRESS_LEVEL_MEDIUM,
	HEALTH_METRICS_STRESS_LEVEL_LOW,
};

struct health_metrics_timed_value {
	uint32_t timestamp_ms;
	uint16_t nn_ms;
};

struct health_metrics_output {
	uint32_t timestamp_ms;
	uint16_t bpm;
	bool bpm_valid;
	enum health_metrics_bpm_mode bpm_mode;
	uint16_t hrv_rmssd_ms_x10;
	uint16_t hrv_sdnn_ms_x10;
	bool hrv_valid;
	enum health_metrics_hrv_mode hrv_mode;
	enum health_metrics_stress_level stress_level;
	bool nn_bpm_valid;
	bool nn_hrv_valid;
	bool summary_due;
};

struct health_metrics_state {
	bool have_time_origin;
	uint32_t time_origin_ms;
	uint32_t next_summary_ms;
	bool have_last_timestamp_ms;
	uint32_t last_timestamp_ms;

	struct health_metrics_timed_value
		accepted_samples[HEALTH_METRICS_BPM_ACCEPTED_WINDOW];
	uint8_t accepted_sample_count;
	uint8_t accepted_sample_index;
	uint8_t accepted_since_last_display;
	bool have_bpm_last_accepted_timestamp;
	uint32_t bpm_last_accepted_timestamp_ms;
	bool have_bpm_last_display_timestamp;
	uint32_t bpm_last_display_timestamp_ms;

	bool latest_nn_bpm_valid;
	bool latest_nn_hrv_valid;

	enum health_metrics_bpm_mode bpm_mode;
	uint16_t bpm_value;
	bool bpm_valid;

	struct health_metrics_timed_value
		hrv_samples[HEALTH_METRICS_MAX_WINDOW_SAMPLES];
	uint16_t hrv_sample_count;

	enum health_metrics_hrv_mode hrv_mode;
	uint16_t hrv_rmssd_ms_x10;
	uint16_t hrv_sdnn_ms_x10;
	bool hrv_valid;
	enum health_metrics_stress_level stress_level;
	bool have_hrv_last_good_timestamp;
	uint32_t hrv_last_good_timestamp_ms;
};

void health_metrics_reset(struct health_metrics_state *state);
int health_metrics_push(struct health_metrics_state *state, uint32_t timestamp_ms,
			uint32_t ibi_ms, struct health_metrics_output *output);
int health_metrics_advance(struct health_metrics_state *state, uint32_t timestamp_ms,
			   struct health_metrics_output *output);
const char *health_metrics_stress_level_str(enum health_metrics_stress_level level);

#endif /* HEALTH_METRICS_H_ */
