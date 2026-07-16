#ifndef MAX30102_SERVICE_H_
#define MAX30102_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

struct max30102_service_telemetry {
	uint32_t queue_drop_count;
	uint32_t fifo_overflow_count;
	uint32_t driver_error_count;
	uint32_t irq_wakeup_count;
	uint32_t max_queue_depth_seen;
	uint16_t last_stream_gap_reason;
};

struct max30102_health_snapshot {
	bool bpm_valid;
	uint16_t bpm;
};

void max30102_service_init(void);
void max30102_service_enable(void);
void max30102_service_get_telemetry(struct max30102_service_telemetry *telemetry);
void max30102_service_get_health_snapshot(
	struct max30102_health_snapshot *snapshot);

#endif /* MAX30102_SERVICE_H_ */
