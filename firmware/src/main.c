/*
 * ALEC NB-IoT Sensor Demo — nRF9151
 *
 * Sends a simulated sensor reading (temperature, humidity, pressure,
 * timestamp, sequence number) over NB-IoT / MQTT every 60 seconds.
 *
 * Payload is raw binary (uncompressed).  ALEC compression will be
 * inserted at the marked TODO sites in a follow-up step.
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
#define MQTT_TOPIC           "alec/sensor/demo"
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
static AlecEncoder              *alec_enc_temp;
static AlecEncoder              *alec_enc_hum;
static AlecEncoder              *alec_enc_pres;

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
/*  Publish                                                            */
/* ------------------------------------------------------------------ */

/*
 * ALEC wire format:
 *   [u16 len_temp][alec_msg_temp][u16 len_hum][alec_msg_hum][u16 len_pres][alec_msg_pres]
 *
 * Each alec_msg is the output of alec_encode_value() for one sensor field.
 * The backend decoder mirrors this layout to decompress.
 */
#define ALEC_BUF_CAP  128   /* per-field encode buffer */
#define ALEC_TOTAL_CAP (3 * (2 + ALEC_BUF_CAP))

static int publish_reading(const struct sensor_payload *p)
{
	uint8_t wire[ALEC_TOTAL_CAP];
	size_t  wire_len = 0;
	uint64_t ts = (p->timestamp_ms > 0) ? (uint64_t)p->timestamp_ms : 0;

	/* Encode each sensor field individually */
	struct {
		AlecEncoder *enc;
		double       value;
		const char  *name;
	} fields[] = {
		{ alec_enc_temp, (double)p->temperature_c_x100, "temp" },
		{ alec_enc_hum,  (double)p->humidity_rh_x100,   "hum"  },
		{ alec_enc_pres, (double)p->pressure_pa,         "pres" },
	};

	for (int i = 0; i < 3; i++) {
		uint8_t buf[ALEC_BUF_CAP];
		size_t  enc_len = 0;

		AlecResult rc = alec_encode_value(
			fields[i].enc,
			fields[i].value,
			ts,
			fields[i].name,
			buf, sizeof(buf), &enc_len);

		if (rc != ALEC_OK) {
			LOG_ERR("ALEC encode %s failed: %s",
				fields[i].name, alec_result_to_string(rc));
			/* Fallback: publish raw uncompressed */
			goto publish_raw;
		}

		/* Write length prefix (little-endian u16) + encoded bytes */
		if (wire_len + 2 + enc_len > sizeof(wire)) {
			LOG_ERR("ALEC wire buffer overflow");
			goto publish_raw;
		}
		wire[wire_len]     = (uint8_t)(enc_len & 0xFF);
		wire[wire_len + 1] = (uint8_t)((enc_len >> 8) & 0xFF);
		memcpy(&wire[wire_len + 2], buf, enc_len);
		wire_len += 2 + enc_len;
	}

	LOG_INF("ALEC publish %u bytes (raw=%u)  seq=%u  t=%d  rh=%u  pa=%u",
		(unsigned)wire_len, (unsigned)sizeof(*p), p->sequence,
		p->temperature_c_x100, p->humidity_rh_x100, p->pressure_pa);

	goto do_publish;

publish_raw:
	/* Fallback: send the raw struct if ALEC encoding fails */
	memcpy(wire, p, sizeof(*p));
	wire_len = sizeof(*p);

	LOG_INF("RAW publish %u bytes  seq=%u  t=%d  rh=%u  pa=%u",
		(unsigned)wire_len, p->sequence,
		p->temperature_c_x100, p->humidity_rh_x100, p->pressure_pa);

do_publish:
	;
	struct mqtt_publish_param param = {
		.message = {
			.topic = {
				.topic = {
					.utf8 = (uint8_t *)MQTT_TOPIC,
					.size = strlen(MQTT_TOPIC),
				},
				.qos = MQTT_QOS_1_AT_LEAST_ONCE,
			},
			.payload = {
				.data = wire,
				.len  = wire_len,
			},
		},
		.message_id = seq_num,
	};

	return mqtt_publish(&client, &param);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
	int err;

	LOG_INF("ALEC NB-IoT Sensor Demo starting (alec-ffi %s)",
		alec_version());

	/* 0. ALEC encoders (one per sensor field for independent context) */
	alec_enc_temp = alec_encoder_new();
	alec_enc_hum  = alec_encoder_new();
	alec_enc_pres = alec_encoder_new();
	if (!alec_enc_temp || !alec_enc_hum || !alec_enc_pres) {
		LOG_ERR("Failed to create ALEC encoders — halting");
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
