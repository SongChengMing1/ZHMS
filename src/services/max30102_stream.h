#ifndef MAX30102_STREAM_H_
#define MAX30102_STREAM_H_

#include <stdint.h>

enum max30102_stream_item_type {
	MAX30102_ITEM_SAMPLE = 0,
	MAX30102_ITEM_STREAM_GAP,
	MAX30102_ITEM_DRIVER_ERROR,
};

enum max30102_stream_gap_reason {
	MAX30102_STREAM_GAP_QUEUE_OVERFLOW = 0,
	MAX30102_STREAM_GAP_FIFO_OVERFLOW,
	MAX30102_STREAM_GAP_DRIVER_ERROR,
};

struct max30102_raw_sample {
	uint32_t timestamp_ms;
	uint32_t red;
	uint32_t ir;
	uint16_t seq;
	uint8_t source_flags;
};

struct max30102_stream_gap {
	uint32_t timestamp_ms;
	uint16_t dropped_samples;
	uint8_t reason;
};

struct max30102_driver_error {
	uint32_t timestamp_ms;
	int err;
	uint8_t stage;
};

struct max30102_stream_item {
	enum max30102_stream_item_type type;
	union {
		struct max30102_raw_sample sample;
		struct max30102_stream_gap gap;
		struct max30102_driver_error driver_error;
	};
};

#endif /* MAX30102_STREAM_H_ */
