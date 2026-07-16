#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>

#include "max30102_service.h"
#include "mqtt_service.h"
#include "wifi.h"

LOG_MODULE_REGISTER(mqtt_service, CONFIG_LOG_DEFAULT_LEVEL);

#define MQTT_SERVICE_STACK_SIZE 3072
#define MQTT_SERVICE_PRIORITY 8
#define MQTT_SERVICE_WAIT_PERIOD K_MSEC(100)
#define MQTT_SERVICE_WIFI_WAIT_PERIOD K_SECONDS(1)
#define MQTT_SERVICE_RECONNECT_PERIOD_MS 5000
#define MQTT_SERVICE_RECONNECT_PERIOD K_MSEC(MQTT_SERVICE_RECONNECT_PERIOD_MS)
#define MQTT_SERVICE_CONNECT_TIMEOUT_MS 3000
#define MQTT_SERVICE_PUBLISH_PERIOD_MS 5000
#define MQTT_SERVICE_BUFFER_SIZE 256
#define MQTT_SERVICE_PAYLOAD_SIZE 96

#define MQTT_SERVICE_BROKER_ADDR CONFIG_ZHMS_MQTT_BROKER_ADDR
#define MQTT_SERVICE_BROKER_PORT CONFIG_ZHMS_MQTT_BROKER_PORT
#define MQTT_SERVICE_CLIENT_ID CONFIG_ZHMS_MQTT_CLIENT_ID
#define MQTT_SERVICE_TOPIC CONFIG_ZHMS_MQTT_TOPIC

static struct mqtt_client mqtt_client_ctx;
static struct sockaddr_storage mqtt_broker;
static uint8_t mqtt_rx_buffer[MQTT_SERVICE_BUFFER_SIZE];
static uint8_t mqtt_tx_buffer[MQTT_SERVICE_BUFFER_SIZE];
static atomic_t mqtt_initialized = ATOMIC_INIT(0);
static atomic_t mqtt_enabled = ATOMIC_INIT(0);
static atomic_t mqtt_connected = ATOMIC_INIT(0);
static uint16_t mqtt_next_message_id = 1U;

static int mqtt_service_socket_fd(const struct mqtt_client *client)
{
	if (client == NULL) {
		return -1;
	}

	if (client->transport.type == MQTT_TRANSPORT_NON_SECURE) {
		return client->transport.tcp.sock;
	}

	return -1;
}

static void mqtt_service_evt_handler(struct mqtt_client *const client,
				     const struct mqtt_evt *evt)
{
	ARG_UNUSED(client);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			atomic_set(&mqtt_connected, 1);
			LOG_INF("MQTT broker connected");
		} else {
			LOG_WRN("MQTT CONNACK failed (%d)", evt->result);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		LOG_WRN("MQTT disconnected (%d)", evt->result);
		atomic_set(&mqtt_connected, 0);
		break;
	case MQTT_EVT_PINGRESP:
		LOG_INF("MQTT ping response");
		break;
	default:
		break;
	}
}

static int mqtt_service_broker_init(void)
{
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&mqtt_broker;

	memset(&mqtt_broker, 0, sizeof(mqtt_broker));
	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(MQTT_SERVICE_BROKER_PORT);

	if (zsock_inet_pton(AF_INET, MQTT_SERVICE_BROKER_ADDR,
			    &broker4->sin_addr) != 1) {
		LOG_ERR("Invalid MQTT broker address: %s", MQTT_SERVICE_BROKER_ADDR);
		return -EINVAL;
	}

	return 0;
}

static void mqtt_service_client_init(struct mqtt_client *client)
{
	mqtt_client_init(client);

	client->broker = &mqtt_broker;
	client->evt_cb = mqtt_service_evt_handler;
	client->client_id.utf8 = (uint8_t *)MQTT_SERVICE_CLIENT_ID;
	client->client_id.size = strlen(MQTT_SERVICE_CLIENT_ID);
	client->password = NULL;
	client->user_name = NULL;
	client->protocol_version = MQTT_VERSION_3_1_1;
	client->rx_buf = mqtt_rx_buffer;
	client->rx_buf_size = sizeof(mqtt_rx_buffer);
	client->tx_buf = mqtt_tx_buffer;
	client->tx_buf_size = sizeof(mqtt_tx_buffer);
	client->transport.type = MQTT_TRANSPORT_NON_SECURE;
	client->clean_session = 1U;
}

static void mqtt_service_abort_connection(struct mqtt_client *client)
{
	if (mqtt_service_socket_fd(client) >= 0) {
		(void)mqtt_abort(client);
	}

	atomic_set(&mqtt_connected, 0);
}

static int mqtt_service_poll_and_process(struct mqtt_client *client, int timeout_ms)
{
	struct zsock_pollfd fds[1];
	int rc;

	fds[0].fd = mqtt_service_socket_fd(client);
	fds[0].events = ZSOCK_POLLIN;
	fds[0].revents = 0;

	if (fds[0].fd < 0) {
		return -ENOTCONN;
	}

	rc = zsock_poll(fds, ARRAY_SIZE(fds), timeout_ms);
	if (rc < 0) {
		LOG_WRN("MQTT poll failed (%d)", errno);
		return -errno;
	}

	if (rc > 0) {
		if ((fds[0].revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP |
				       ZSOCK_POLLNVAL)) != 0) {
			LOG_WRN("MQTT socket closed (revents=0x%x)", fds[0].revents);
			return -ENOTCONN;
		}

		if ((fds[0].revents & ZSOCK_POLLIN) != 0) {
			rc = mqtt_input(client);
			if (rc != 0) {
				LOG_WRN("mqtt_input failed (%d)", rc);
				return rc;
			}
		}
	}

	rc = mqtt_live(client);
	if (rc != 0 && rc != -EAGAIN) {
		LOG_WRN("mqtt_live failed (%d)", rc);
		return rc;
	}

	if (rc == 0) {
		rc = mqtt_input(client);
		if (rc != 0) {
			LOG_WRN("mqtt_input after ping failed (%d)", rc);
			return rc;
		}
	}

	return 0;
}

static int mqtt_service_wait_for_connack(struct mqtt_client *client)
{
	int64_t deadline_ms = k_uptime_get() + MQTT_SERVICE_CONNECT_TIMEOUT_MS;

	while (!atomic_get(&mqtt_connected)) {
		struct zsock_pollfd fds[1];
		int64_t remaining_ms = deadline_ms - k_uptime_get();
		int rc;

		if (remaining_ms <= 0) {
			break;
		}

		fds[0].fd = mqtt_service_socket_fd(client);
		fds[0].events = ZSOCK_POLLIN;
		fds[0].revents = 0;

		if (fds[0].fd < 0) {
			break;
		}

		rc = zsock_poll(fds, ARRAY_SIZE(fds), (int)remaining_ms);
		if (rc < 0) {
			LOG_WRN("MQTT connect poll failed (%d)", errno);
			break;
		}

		if (rc > 0 && (fds[0].revents & ZSOCK_POLLIN) != 0) {
			rc = mqtt_input(client);
			if (rc != 0) {
				LOG_WRN("mqtt_input during connect failed (%d)", rc);
				break;
			}
		}
	}

	if (!atomic_get(&mqtt_connected)) {
		LOG_WRN("MQTT connect timeout");
		mqtt_service_abort_connection(client);
		return -ETIMEDOUT;
	}

	return 0;
}

static int mqtt_service_connect(struct mqtt_client *client)
{
	int rc;

	if (MQTT_SERVICE_BROKER_ADDR[0] == '\0') {
		LOG_ERR("MQTT broker is not configured; set "
			"CONFIG_ZHMS_MQTT_BROKER_ADDR");
		return -EINVAL;
	}

	rc = mqtt_service_broker_init();
	if (rc != 0) {
		return rc;
	}

	mqtt_service_client_init(client);
	atomic_set(&mqtt_connected, 0);

	rc = mqtt_connect(client);
	if (rc != 0) {
		LOG_WRN("mqtt_connect failed (%d)", rc);
		return rc;
	}

	return mqtt_service_wait_for_connack(client);
}

static int mqtt_service_publish_bpm(struct mqtt_client *client)
{
	struct max30102_health_snapshot snapshot = {0};
	struct mqtt_publish_param param = {0};
	char payload[MQTT_SERVICE_PAYLOAD_SIZE];
	int payload_len;

	max30102_service_get_health_snapshot(&snapshot);

	payload_len = snprintk(payload, sizeof(payload),
			       "{\"bpm\":%u,\"valid\":%s,\"uptime_ms\":%lld}",
			       snapshot.bpm_valid ? snapshot.bpm : 0U,
			       snapshot.bpm_valid ? "true" : "false",
			       (long long)k_uptime_get());
	if (payload_len < 0 || payload_len >= (int)sizeof(payload)) {
		return -EMSGSIZE;
	}

	param.message.topic.topic.utf8 = (uint8_t *)MQTT_SERVICE_TOPIC;
	param.message.topic.topic.size = strlen(MQTT_SERVICE_TOPIC);
	param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	param.message.payload.data = (uint8_t *)payload;
	param.message.payload.len = payload_len;
	param.message_id = mqtt_next_message_id++;
	param.dup_flag = 0U;
	param.retain_flag = 0U;

	return mqtt_publish(client, &param);
}

static void mqtt_service_thread(void *arg1, void *arg2, void *arg3)
{
	int64_t next_connect_attempt_ms = 0;
	int64_t next_publish_ms = 0;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		int64_t now_ms = k_uptime_get();

		if (!atomic_get(&mqtt_enabled)) {
			k_sleep(MQTT_SERVICE_WAIT_PERIOD);
			continue;
		}

		if (wifi_get_state() != WIFI_READY) {
			if (atomic_get(&mqtt_connected)) {
				mqtt_service_abort_connection(&mqtt_client_ctx);
			}
			next_connect_attempt_ms = now_ms;
			k_sleep(MQTT_SERVICE_WIFI_WAIT_PERIOD);
			continue;
		}

		if (!atomic_get(&mqtt_connected)) {
			if (now_ms < next_connect_attempt_ms) {
				k_sleep(K_MSEC((int32_t)(next_connect_attempt_ms - now_ms)));
				continue;
			}

			if (mqtt_service_connect(&mqtt_client_ctx) != 0) {
				next_connect_attempt_ms =
					k_uptime_get() + MQTT_SERVICE_RECONNECT_PERIOD_MS;
				k_sleep(MQTT_SERVICE_RECONNECT_PERIOD);
				continue;
			}

			next_publish_ms = k_uptime_get();
			continue;
		}

		{
			int32_t wait_ms = mqtt_keepalive_time_left(&mqtt_client_ctx);

			if (wait_ms < 0) {
				wait_ms = 0;
			}

			if (next_publish_ms > now_ms) {
				int64_t publish_wait_ms = next_publish_ms - now_ms;

				if (publish_wait_ms < wait_ms || wait_ms == 0) {
					wait_ms = (int32_t)publish_wait_ms;
				}
			} else {
				wait_ms = 0;
			}

				if (mqtt_service_poll_and_process(&mqtt_client_ctx, wait_ms) != 0) {
					mqtt_service_abort_connection(&mqtt_client_ctx);
					next_connect_attempt_ms =
						k_uptime_get() + MQTT_SERVICE_RECONNECT_PERIOD_MS;
					k_sleep(MQTT_SERVICE_RECONNECT_PERIOD);
					continue;
				}
		}

		now_ms = k_uptime_get();
		if (now_ms >= next_publish_ms) {
			int rc = mqtt_service_publish_bpm(&mqtt_client_ctx);

				if (rc != 0) {
					LOG_WRN("MQTT publish failed (%d)", rc);
					mqtt_service_abort_connection(&mqtt_client_ctx);
					next_connect_attempt_ms =
						k_uptime_get() + MQTT_SERVICE_RECONNECT_PERIOD_MS;
					k_sleep(MQTT_SERVICE_RECONNECT_PERIOD);
					continue;
				}

			next_publish_ms = now_ms + MQTT_SERVICE_PUBLISH_PERIOD_MS;
		}
	}
}

K_THREAD_DEFINE(mqtt_service_thread_id, MQTT_SERVICE_STACK_SIZE,
		mqtt_service_thread, NULL, NULL, NULL,
		MQTT_SERVICE_PRIORITY, 0, 0);

void mqtt_service_init(void)
{
	atomic_set(&mqtt_connected, 0);
	atomic_set(&mqtt_enabled, 0);
	atomic_set(&mqtt_initialized, 1);
	LOG_INF("MQTT service initialized");
}

void mqtt_service_enable(void)
{
	if (!atomic_get(&mqtt_initialized)) {
		LOG_ERR("MQTT service not initialized");
		return;
	}

	atomic_set(&mqtt_enabled, 1);
	LOG_INF("MQTT service enabled");
}
