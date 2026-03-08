/*
 * ALEC NB-IoT Sensor Demo — nRF9151
 *
 * Sends a simulated sensor reading (temperature, humidity, pressure,
 * timestamp, sequence number) over NB-IoT / MQTT every 60 seconds.
 *
 * Publishes to two topics:
 *   alec/sensor/demo — ALEC-compressed payload
 *   alec/sensor/raw  — uncompressed raw struct (for comparison)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>

#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <date_time.h>

#include <string.h>
#include <stdio.h>

#include <alec.h>

LOG_MODULE_REGISTER(alec_demo, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */

#define MQTT_BROKER_HOSTNAME "test.mosquitto.org"
#define MQTT_BROKER_PORT     1883
#define MQTT_TOPIC_DEMO      "alec/sensor/demo"
#define MQTT_TOPIC_RAW       "alec/sensor/raw"
#define MQTT_CLIENT_ID_PFX   "alec_nrf9151_"
#define PUBLISH_INTERVAL_S   60

/* ------------------------------------------------------------------ */
/*  Sensor payload (binary struct, packed)                             */
/* ------------------------------------------------------------------ */

struct sensor_payload {
	int16_t  temperature_c_x100;   /* °C × 100   (e.g. 2350 = 23.50°C) */
	uint16_t humidity_rh_x100;     /* %RH × 100  (e.g. 5520 = 55.20%)  */
	uint32_t pressure_pa;          /* Pa          (e.g. 101325)         */
	int64_t  timestamp_ms;         /* Unix epoch ms                     */
	uint32_t sequence;             /* Monotonic counter                 */
} __packed;

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */

static struct mqtt_client        client;
static struct sockaddr_storage   broker_addr;
static uint8_t                   rx_buf[256];
static uint8_t                   tx_buf[256];
static struct zsock_pollfd       fds;
static bool                      mqtt_connected;
static uint32_t                  seq_num;
static AlecEncoder              *alec_enc;

/* ------------------------------------------------------------------ */
/*  LTE helpers                                                        */
/* ------------------------------------------------------------------ */

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
		    evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			LOG_INF("LTE registered");
		}
		break;
	default:
		break;
	}
}

static int lte_connect(void)
{
	int err;

	LOG_INF("Initialising modem library...");
	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init failed: %d", err);
		return err;
	}

	LOG_INF("Connecting to LTE network (NB-IoT preferred)...");
	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async failed: %d", err);
		return err;
	}

	/* Block until registered (simplistic wait). */
	enum lte_lc_nw_reg_status status;

	do {
		k_sleep(K_SECONDS(2));
		lte_lc_nw_reg_status_get(&status);
	} while (status != LTE_LC_NW_REG_REGISTERED_HOME &&
		 status != LTE_LC_NW_REG_REGISTERED_ROAMING);

	LOG_INF("LTE connected");
	return 0;
}

/* ------------------------------------------------------------------ */
/*  MQTT helpers                                                       */
/* ------------------------------------------------------------------ */

static void mqtt_evt_handler(struct mqtt_client *const c,
			     const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			mqtt_connected = true;
			LOG_INF("MQTT connected");
		} else {
			LOG_ERR("MQTT CONNACK error: %d", evt->result);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		mqtt_connected = false;
		LOG_WRN("MQTT disconnected");
		break;
	case MQTT_EVT_PUBACK:
		LOG_INF("MQTT PUBACK id=%u", evt->param.puback.message_id);
		break;
	default:
		break;
	}
}

static int broker_resolve(void)
{
	struct zsock_addrinfo *ai = NULL;
	struct zsock_addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	int err;

	err = zsock_getaddrinfo(MQTT_BROKER_HOSTNAME, NULL, &hints, &ai);
	if (err || !ai) {
		LOG_ERR("DNS resolve failed: %d", err);
		return -EFAULT;
	}

	struct sockaddr_in *broker = (struct sockaddr_in *)&broker_addr;

	broker->sin_family = AF_INET;
	broker->sin_port   = htons(MQTT_BROKER_PORT);
	broker->sin_addr   = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;
	zsock_freeaddrinfo(ai);

	LOG_INF("Broker resolved");
	return 0;
}

static int mqtt_setup_and_connect(void)
{
	int err;
	static char client_id[48];

	/* Unique-ish client id from k_cycle_get_32 */
	snprintf(client_id, sizeof(client_id), "%s%08x",
		 MQTT_CLIENT_ID_PFX, k_cycle_get_32());

	mqtt_client_init(&client);

	client.broker        = (struct sockaddr *)&broker_addr;
	client.evt_cb        = mqtt_evt_handler;
	client.client_id.utf8     = (uint8_t *)client_id;
	client.client_id.size     = strlen(client_id);
	client.protocol_version   = MQTT_VERSION_3_1_1;
	client.rx_buf        = rx_buf;
	client.rx_buf_size   = sizeof(rx_buf);
	client.tx_buf        = tx_buf;
	client.tx_buf_size   = sizeof(tx_buf);
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	err = mqtt_connect(&client);
	if (err) {
		LOG_ERR("mqtt_connect failed: %d", err);
		return err;
	}

	fds.fd     = client.transport.tcp.sock;
	fds.events = ZSOCK_POLLIN;

	/* Process CONNACK */
	for (int i = 0; i < 10 && !mqtt_connected; i++) {
		zsock_poll(&fds, 1, 1000);
		mqtt_input(&client);
	}

	if (!mqtt_connected) {
		LOG_ERR("MQTT did not connect in time");
		return -ETIMEDOUT;
	}

	return 0;
}

static void mqtt_process(void)
{
	int rc = zsock_poll(&fds, 1, 0);

	if (rc > 0 && (fds.revents & ZSOCK_POLLIN)) {
		mqtt_input(&client);
	}
	mqtt_live(&client);
}

/* ------------------------------------------------------------------ */
/*  Sensor simulation                                                  */
/* ------------------------------------------------------------------ */

static void simulate_reading(struct sensor_payload *p)
{
	/* Temperature: 20.00–30.00 °C */
	p->temperature_c_x100 = 2000 + (int16_t)(sys_rand32_get() % 1001);

	/* Humidity: 40.00–70.00 %RH */
	p->humidity_rh_x100 = 4000 + (uint16_t)(sys_rand32_get() % 3001);

	/* Pressure: 99000–103000 Pa */
	p->pressure_pa = 99000 + (sys_rand32_get() % 4001);

	/* Timestamp (ms since epoch, or 0 if time not available) */
	int64_t ts = 0;

	date_time_now(&ts);
	p->timestamp_ms = ts;

	p->sequence = seq_num++;
}

/* ------------------------------------------------------------------ */
/*  Publish helpers                                                    */
/* ------------------------------------------------------------------ */

static int mqtt_pub(const char *topic, const uint8_t *data, size_t len,
		    uint16_t msg_id)
{
	struct mqtt_publish_param param = {
		.message = {
			.topic = {
				.topic = {
					.utf8 = (uint8_t *)topic,
					.size = strlen(topic),
				},
				.qos = MQTT_QOS_1_AT_LEAST_ONCE,
			},
			.payload = {
				.data = (uint8_t *)data,
				.len  = len,
			},
		},
		.message_id = msg_id,
	};

	return mqtt_publish(&client, &param);
}

/* ------------------------------------------------------------------ */
/*  Publish reading (ALEC-compressed + raw)                            */
/* ------------------------------------------------------------------ */

#define ALEC_OUTPUT_CAP 128

static int publish_reading(const struct sensor_payload *p)
{
	int err;
	const uint8_t *raw = (const uint8_t *)p;
	size_t raw_len = sizeof(*p);

	/* ALEC-compress the raw struct bytes */
	uint8_t compressed[ALEC_OUTPUT_CAP];
	size_t compressed_len = 0;

	AlecResult rc = alec_encode(alec_enc,
				    raw, raw_len,
				    compressed, sizeof(compressed),
				    &compressed_len);

	if (rc == ALEC_OK && compressed_len > 0) {
		LOG_INF("ALEC %u→%uB (%.0f%%) seq=%u t=%d rh=%u pa=%u",
			(unsigned)raw_len, (unsigned)compressed_len,
			100.0f * (1.0f - (float)compressed_len / (float)raw_len),
			p->sequence,
			p->temperature_c_x100, p->humidity_rh_x100,
			p->pressure_pa);

		err = mqtt_pub(MQTT_TOPIC_DEMO, compressed, compressed_len,
			       seq_num * 2);
		if (err) {
			LOG_ERR("Publish compressed failed: %d", err);
			return err;
		}
	} else {
		LOG_WRN("ALEC encode failed (%s) — sending raw only",
			alec_result_to_string(rc));
	}

	/* Always publish raw struct for comparison */
	err = mqtt_pub(MQTT_TOPIC_RAW, raw, raw_len, seq_num * 2 + 1);
	if (err) {
		LOG_ERR("Publish raw failed: %d", err);
		return err;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
	int err;

	/* Initialize ALEC heap — required for bare-metal (alec-ffi 1.2.1) */
	alec_heap_init();

	LOG_INF("ALEC NB-IoT Sensor Demo starting (alec-ffi %s)",
		alec_version());

	/* 0. ALEC encoder */
	alec_enc = alec_encoder_new();
	if (!alec_enc) {
		LOG_ERR("Failed to create ALEC encoder — halting");
		return -ENOMEM;
	}

	/* 1. LTE */
	err = lte_connect();
	if (err) {
		LOG_ERR("LTE connection failed — halting");
		return err;
	}

	/* 2. Resolve broker */
	err = broker_resolve();
	if (err) {
		return err;
	}

	/* 3. MQTT */
	err = mqtt_setup_and_connect();
	if (err) {
		return err;
	}

	/* 4. Publish loop */
	while (1) {
		struct sensor_payload reading;

		simulate_reading(&reading);

		err = publish_reading(&reading);
		if (err) {
			LOG_ERR("Publish failed: %d — reconnecting", err);
			mqtt_disconnect(&client);
			mqtt_connected = false;
			k_sleep(K_SECONDS(5));
			mqtt_setup_and_connect();
			continue;
		}

		/* Keep-alive + receive processing while we wait */
		for (int i = 0; i < PUBLISH_INTERVAL_S; i++) {
			mqtt_process();
			k_sleep(K_SECONDS(1));
		}
	}

	return 0;
}
