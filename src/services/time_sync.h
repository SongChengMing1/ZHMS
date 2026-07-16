#ifndef TIME_SYNC_H_
#define TIME_SYNC_H_

enum time_sync_state {
	TIME_SYNC_UNSYNCED,
	TIME_SYNC_IN_PROGRESS,
	TIME_SYNC_SYNCED,
};

void time_sync_init(void);
void time_sync_enable(void);
void time_sync_notify_wifi_ready(void);
enum time_sync_state time_sync_get_state(void);

#endif /* TIME_SYNC_H_ */
