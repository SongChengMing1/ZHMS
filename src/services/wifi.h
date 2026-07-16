#ifndef WIFI_H_
#define WIFI_H_

#include <stdbool.h>
#include <zephyr/autoconf.h>

/*
 * Deployment-specific values come from an untracked EXTRA_CONF_FILE.
 * Never put real Wi-Fi credentials in this header or in prj.conf.
 */
#define WIFI_SSID CONFIG_ZHMS_WIFI_SSID
#define WIFI_PSK  CONFIG_ZHMS_WIFI_PSK
#define WIFI_SNAPSHOT_IP_ADDR_LEN 16

enum wifi_conn_state {
	WIFI_DISCONNECTED,   /* Initial or user-disconnected */
	WIFI_CONNECTING,     /* Connecting to AP, awaiting CONNECT_RESULT */
	WIFI_OBTAINING_IP,   /* WiFi connected, awaiting DHCP */
	WIFI_READY,          /* IP obtained, working */
	WIFI_RECONNECTING,   /* Backoff wait after disconnect */
	WIFI_ERROR,          /* 6 retries exhausted, waiting for wifi_connect() */
};

struct wifi_snapshot {
	enum wifi_conn_state state;
	bool ip_valid;
	char ip_addr[WIFI_SNAPSHOT_IP_ADDR_LEN];
	bool rssi_valid;
	int rssi_dbm;
};

void wifi_init(void);
void wifi_connect(void);
void wifi_disconnect(void);
enum wifi_conn_state wifi_get_state(void);
int wifi_get_snapshot(struct wifi_snapshot *snapshot);

#endif /* WIFI_H_ */
