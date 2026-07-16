#ifndef MAX30102_SERVICE_INTERNAL_H_
#define MAX30102_SERVICE_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "max30102_stream.h"

#ifndef MAX30102_STREAM_QUEUE_CAPACITY
#define MAX30102_STREAM_QUEUE_CAPACITY 128U
#endif

struct max30102_stream_queue {
	struct max30102_stream_item items[MAX30102_STREAM_QUEUE_CAPACITY];
	uint16_t read_index;
	uint16_t write_index;
	uint16_t count;
	struct k_spinlock lock;
};

struct max30102_pending_stream_gap {
	bool active;
	uint8_t reason;
	uint16_t dropped_samples;
};

struct max30102_service_runtime {
	const struct device *dev;
	atomic_t initialized;
	atomic_t enabled;
	struct k_sem stream_sem;
	struct max30102_stream_queue queue;
	uint32_t logical_timestamp_ms;
	bool logical_timestamp_anchored;
	uint16_t next_seq;
	struct max30102_pending_stream_gap pending_stream_gap;
	uint32_t queue_drop_count;
	uint32_t fifo_overflow_count;
	uint32_t driver_error_count;
	uint32_t irq_wakeup_count;
	uint32_t max_queue_depth_seen;
	uint16_t last_stream_gap_reason;
};

extern struct max30102_service_runtime max30102_runtime;

bool max30102_stream_queue_push(const struct max30102_stream_item *item);
bool max30102_stream_queue_pop(struct max30102_stream_item *item);
uint32_t max30102_acquisition_next_timestamp_ms(void);
void max30102_service_set_health_snapshot(bool bpm_valid, uint16_t bpm);
void max30102_service_clear_health_snapshot(void);
void max30102_acquisition_thread(void *arg1, void *arg2, void *arg3);
void max30102_algorithm_thread(void *arg1, void *arg2, void *arg3);

#endif /* MAX30102_SERVICE_INTERNAL_H_ */
