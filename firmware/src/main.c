/*
 * ALEC NB-IoT Sensor Demo — nRF9151
 *
 * Sends a simulated Milesight EM500-CO2 reading (battery, temperature,
 * humidity, CO2, pressure) over NB-IoT / MQTT every PUBLISH_INTERVAL_S
 * seconds. Uses ALEC v1.3.5 fixed-channel encoding with an 11-byte
 * LoRaWAN-style ceiling; falls back to raw struct if the encoded frame
 * exceeds that ceiling (notably cold-start and keyframe messages).
 *
 * Publishes to two topics:
 *   alec/sensor/demo — ALEC-compressed payload (or raw on fallback)
 *   alec/sensor/raw  — uncompressed raw struct (always, for comparison)
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

// #define MQTT_BROKER_HOSTNAME "test.mosquitto.org"
#define MQTT_BROKER_HOSTNAME "broker.hivemq.com"
#define MQTT_BROKER_PORT 1883
#define MQTT_TOPIC_DEMO "alec/sensor/demo"
#define MQTT_TOPIC_RAW "alec/sensor/raw"
#define MQTT_CLIENT_ID_PFX "alec_nrf9151_"
#define PUBLISH_INTERVAL_S 5

/* ------------------------------------------------------------------ */
/*  Sensor payload — 5× f64 EM500-CO2 channels (matches ALEC values[]) */
/* ------------------------------------------------------------------ */

struct sensor_payload
{
	double battery;     /* % — always 100.0 in sim */
	double temperature; /* °C — range 24.0-27.0    */
	double humidity;    /* %RH — range 55.0-65.0   */
	double co2;         /* ppm — range 400.0-650.0 */
	double pressure;    /* hPa — range 1005.0-1010.0 */
};

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */

static struct mqtt_client client;
static struct sockaddr_storage broker_addr;
static uint8_t rx_buf[256];
static uint8_t tx_buf[256];
static struct zsock_pollfd fds;
static bool mqtt_connected;
static uint32_t seq_num;
static AlecEncoder *alec_enc;

/* ------------------------------------------------------------------ */
/*  LTE helpers                                                        */
/* ------------------------------------------------------------------ */

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type)
	{
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)
		{
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
	if (err)
	{
		LOG_ERR("nrf_modem_lib_init failed: %d", err);
		return err;
	}

	LOG_INF("Connecting to LTE network (NB-IoT preferred)...");
	err = lte_lc_connect_async(lte_handler);
	if (err)
	{
		LOG_ERR("lte_lc_connect_async failed: %d", err);
		return err;
	}

	/* Block until registered (simplistic wait). */
	enum lte_lc_nw_reg_status status;

	do
	{
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
	switch (evt->type)
	{
	case MQTT_EVT_CONNACK:
		if (evt->result == 0)
		{
			mqtt_connected = true;
			LOG_INF("MQTT connected");
		}
		else
		{
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
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	int err;

	err = zsock_getaddrinfo(MQTT_BROKER_HOSTNAME, NULL, &hints, &ai);
	if (err || !ai)
	{
		LOG_ERR("DNS resolve failed: %d", err);
		return -EFAULT;
	}

	struct sockaddr_in *broker = (struct sockaddr_in *)&broker_addr;

	broker->sin_family = AF_INET;
	broker->sin_port = htons(MQTT_BROKER_PORT);
	broker->sin_addr = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;
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

	client.broker = (struct sockaddr *)&broker_addr;
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = (uint8_t *)client_id;
	client.client_id.size = strlen(client_id);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	err = mqtt_connect(&client);
	if (err)
	{
		LOG_ERR("mqtt_connect failed: %d", err);
		return err;
	}

	fds.fd = client.transport.tcp.sock;
	fds.events = ZSOCK_POLLIN;

	/* Process CONNACK */
	for (int i = 0; i < 10 && !mqtt_connected; i++)
	{
		zsock_poll(&fds, 1, 1000);
		mqtt_input(&client);
	}

	if (!mqtt_connected)
	{
		LOG_ERR("MQTT did not connect in time");
		return -ETIMEDOUT;
	}

	return 0;
}

static void mqtt_process(void)
{
	int rc = zsock_poll(&fds, 1, 0);

	if (rc > 0 && (fds.revents & ZSOCK_POLLIN))
	{
		mqtt_input(&client);
	}
	mqtt_live(&client);
}

/* ------------------------------------------------------------------ */
/*  Sensor simulation                                                  */
/* ------------------------------------------------------------------ */

/* Milesight EM500-CO2 profile — slow drift, highly periodic. */
static double sim_battery  = 100.0;  /* %, constant */
static double sim_temp     = 26.9;   /* °C */
static double sim_humidity = 58.5;   /* %RH */
static double sim_co2      = 641.0;  /* ppm */
static double sim_pressure = 1007.7; /* hPa */

/* Deterministic drift tables — no rand32, repeating 16-step cycle. */
/* Temperature: ±0.1°C random walk. */
static const double drift_temp[] = {
	 0.1, -0.1,  0.0,  0.1,  0.0, -0.1,  0.1,  0.0,
	-0.1,  0.0,  0.1, -0.1,  0.0,  0.0,  0.1, -0.1};
/* Humidity: ±0.5%RH random walk. */
static const double drift_rh[] = {
	 0.5, -0.5,  0.0, -0.5,  0.5,  0.0,  0.5, -0.5,
	-0.5,  0.0,  0.5,  0.5, -0.5,  0.0, -0.5,  0.5};
/* CO2: trend -1.5/step + noise ±1.0 → range [-2.5, -0.5]. */
static const double drift_co2[] = {
	-1.0, -2.0, -1.5, -0.5, -1.5, -2.5, -1.0, -1.5,
	-2.0, -1.0, -1.5, -0.5, -1.5, -2.5, -1.0, -2.0};
/* Pressure: trend -0.1/step + noise ±0.05 → range [-0.15, -0.05]. */
static const double drift_press[] = {
	-0.10, -0.05, -0.10, -0.15, -0.10, -0.05, -0.10, -0.10,
	-0.15, -0.10, -0.05, -0.10, -0.10, -0.15, -0.05, -0.10};
static uint8_t drift_idx = 0;

static void simulate_reading(struct sensor_payload *p)
{
	uint8_t i = drift_idx % 16;

	/* Battery: constant. */
	sim_battery = 100.0;

	/* Temperature: random walk, clamp [24.0, 27.0]. */
	sim_temp += drift_temp[i];
	if (sim_temp < 24.0)
		sim_temp = 24.0;
	if (sim_temp > 27.0)
		sim_temp = 27.0;

	/* Humidity: random walk, clamp [55.0, 65.0]. */
	sim_humidity += drift_rh[i];
	if (sim_humidity < 55.0)
		sim_humidity = 55.0;
	if (sim_humidity > 65.0)
		sim_humidity = 65.0;

	/* CO2: slow downward drift, clamp [400.0, 700.0]. */
	sim_co2 += drift_co2[i];
	if (sim_co2 < 400.0)
		sim_co2 = 400.0;
	if (sim_co2 > 700.0)
		sim_co2 = 700.0;

	/* Pressure: slow downward drift, clamp [1000.0, 1015.0]. */
	sim_pressure += drift_press[i];
	if (sim_pressure < 1000.0)
		sim_pressure = 1000.0;
	if (sim_pressure > 1015.0)
		sim_pressure = 1015.0;

	drift_idx++;

	p->battery     = sim_battery;
	p->temperature = sim_temp;
	p->humidity    = sim_humidity;
	p->co2         = sim_co2;
	p->pressure    = sim_pressure;

	seq_num++;
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
				.len = len,
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
#define ALEC_N_CHANNELS 5

static int publish_reading(const struct sensor_payload *p)
{
	int err;
	const uint8_t *raw = (const uint8_t *)p;
	size_t raw_len = sizeof(*p);

	/*
	 * v1.3.5 fixed-channel encode: compact header (marker + seq + ctx_ver +
	 * bitmap) followed by per-channel Repeated/Delta8/Delta16/Raw32. No
	 * source_ids, priorities, or timestamps — channel count is agreed
	 * out-of-band (ALEC_N_CHANNELS). Fallback to raw struct if encoded
	 * output exceeds the 11-byte LoRaWAN ceiling.
	 */

	double values[ALEC_N_CHANNELS];
	values[0] = p->battery;
	values[1] = p->temperature;
	values[2] = p->humidity;
	values[3] = p->co2;
	values[4] = p->pressure;

	uint8_t compressed[32];
	size_t compressed_len = 0;

	AlecResult rc = alec_encode_multi_fixed(
		alec_enc,
		values, ALEC_N_CHANNELS,
		compressed, sizeof(compressed),
		&compressed_len);

	bool used_alec = false;
	if (rc == ALEC_OK && compressed_len > 0 && compressed_len <= 11)
	{
		used_alec = true;
	}

	LOG_INF("────────────────────────────────────");
	LOG_INF("ALEC v1.3.5  seq=%-4u  "
			"bat=%.0f%%  t=%.1f°C  "
			"rh=%.1f%%  co2=%.0fppm  p=%.1fhPa",
			seq_num,
			p->battery, p->temperature,
			p->humidity, p->co2, p->pressure);
	if (rc == ALEC_OK && compressed_len > 0)
	{
		LOG_INF("  Frame marker  : 0x%02X (%s)",
				compressed[0],
				compressed[0] == 0xA2 ? "KEYFRAME" : "data");
	}
	else
	{
		LOG_WRN("  ALEC encode failed: %s (rc=%d)",
				alec_result_to_string(rc), (int)rc);
	}
	LOG_INF("  Raw   struct  : %u B", (unsigned)raw_len);
	LOG_INF("  ALEC output   : %u B (%s)",
			(unsigned)compressed_len,
			used_alec ? "ALEC" : "fallback TLV>11B");
	LOG_INF("────────────────────────────────────");

	if (used_alec)
	{
		err = mqtt_pub(MQTT_TOPIC_DEMO, compressed, compressed_len,
					   seq_num * 2);
		if (err)
		{
			LOG_ERR("Publish compressed failed: %d", err);
			return err;
		}
	}
	else
	{
		LOG_WRN("fallback: frame exceeded 11B, sent raw");
		err = mqtt_pub(MQTT_TOPIC_DEMO, raw, raw_len, seq_num * 2);
		if (err)
		{
			LOG_ERR("Publish fallback raw failed: %d", err);
			return err;
		}
	}

	/* Always publish raw struct for comparison */
	err = mqtt_pub(MQTT_TOPIC_RAW, raw, raw_len, seq_num * 2 + 1);
	if (err)
	{
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
	printk("=== MAIN START ===\n");
	k_sleep(K_SECONDS(1));
	printk("=== AFTER SLEEP ===\n");

	printk("=== CALLING alec_version ===\n");
	const char *ver = alec_version();
	printk("=== alec_version: %s ===\n", ver ? ver : "NULL");

	printk("=== CALLING alec_encoder_new_with_config ===\n");
	AlecEncoderConfig alec_cfg = {
		.history_size      = 20,
		.max_patterns      = 256,
		.max_memory_bytes  = 2048,
		.keyframe_interval = 50,
		.smart_resync      = true,
	};
	alec_enc = alec_encoder_new_with_config(&alec_cfg);
	printk("=== alec_encoder_new_with_config: %p ===\n",
	       (void *)alec_enc);

	if (!alec_enc) {
		printk("=== ENCODER NULL — halting ===\n");
		return -ENOMEM;
	}
	printk("=== ENCODER OK ===\n");

	printk("=== CALLING alec_encode_multi_fixed self-test ===\n");
	double test_vals[5] = {100.0, 26.9, 58.5, 641.0, 1007.7};
	uint8_t test_out[32];
	size_t test_len = 0;
	AlecResult test_rc = alec_encode_multi_fixed(
		alec_enc,
		test_vals, 5,
		test_out, sizeof(test_out),
		&test_len);
	printk("=== self-test rc=%d len=%u marker=0x%02X ===\n",
	       (int)test_rc, (unsigned)test_len,
	       test_len > 0 ? test_out[0] : 0);

	printk("=== ENTERING LOG SYSTEM ===\n");

	int err;

	/*
	 * NOTE: alec_heap_init() crashes on Zephyr/nRF9151 — the alec-ffi
	 * internal heap conflicts with Zephyr's memory layout.  Skip it and
	 * let alec-ffi fall back to k_malloc instead.  Ensure
	 * CONFIG_HEAP_MEM_POOL_SIZE is large enough (>= 16384).
	 */
	// alec_heap_init();

	LOG_INF("ALEC NB-IoT Sensor Demo starting (alec-ffi %s)",
			alec_version());

	LOG_INF("Heap pool size: %d bytes", CONFIG_HEAP_MEM_POOL_SIZE);
	LOG_INF("ALEC encoder initialised OK");

	/* 1. LTE */
	err = lte_connect();
	if (err)
	{
		LOG_ERR("LTE connection failed — halting");
		return err;
	}

	/* 2. Resolve broker */
	err = broker_resolve();
	if (err)
	{
		return err;
	}

	/* 3. MQTT */
	err = mqtt_setup_and_connect();
	if (err)
	{
		return err;
	}

	/* 4. Publish loop */
	while (1)
	{
		struct sensor_payload reading;

		simulate_reading(&reading);

		err = publish_reading(&reading);
		if (err)
		{
			LOG_ERR("Publish failed: %d — reconnecting", err);
			mqtt_disconnect(&client);
			mqtt_connected = false;
			k_sleep(K_SECONDS(5));
			mqtt_setup_and_connect();
			continue;
		}

		/* Keep-alive + receive processing while we wait */
		for (int i = 0; i < PUBLISH_INTERVAL_S; i++)
		{
			mqtt_process();
			k_sleep(K_SECONDS(1));
		}
	}

	return 0;
}