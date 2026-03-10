///! ALEC Decoder Service
///!
///! Subscribes to MQTT topics:
///!   - alec/sensor/demo  — ALEC-compressed payloads (decoded with alec crate)
///!   - alec/sensor/raw   — raw binary payloads (for comparison)
///!
///! Exposes Prometheus metrics on :9100/metrics.

use alec::{Context, Decoder};
use byteorder::{LittleEndian, ReadBytesExt};
use log::{info, warn};
use prometheus::{Gauge, IntCounter, Registry, TextEncoder};
use std::io::Cursor;
use std::sync::Arc;
use std::time::Duration;

// ---------------------------------------------------------------------------
//  Topics
// ---------------------------------------------------------------------------

const TOPIC_DEMO: &str = "alec/sensor/demo";
const TOPIC_RAW: &str = "alec/sensor/raw";

// ---------------------------------------------------------------------------
//  Sensor payload (mirrors firmware struct sensor_payload, 20 bytes packed)
// ---------------------------------------------------------------------------

#[derive(Debug)]
struct SensorPayload {
    temperature_c: f64,  // °C
    humidity_rh: f64,    // %RH
    pressure_pa: u32,    // Pa
    timestamp_ms: i64,   // Unix epoch ms
    sequence: u32,
}

const RAW_PAYLOAD_SIZE: usize = 20; // i16(2)+u16(2)+u32(4)+i64(8)+u32(4)

fn parse_raw(data: &[u8]) -> Option<SensorPayload> {
    if data.len() < RAW_PAYLOAD_SIZE {
        return None;
    }
    let mut cur = Cursor::new(data);
    let temp_x100 = cur.read_i16::<LittleEndian>().ok()?;
    let hum_x100 = cur.read_u16::<LittleEndian>().ok()?;
    let pressure = cur.read_u32::<LittleEndian>().ok()?;
    let ts = cur.read_i64::<LittleEndian>().ok()?;
    let seq = cur.read_u32::<LittleEndian>().ok()?;
    Some(SensorPayload {
        temperature_c: temp_x100 as f64 / 100.0,
        humidity_rh: hum_x100 as f64 / 100.0,
        pressure_pa: pressure,
        timestamp_ms: ts,
        sequence: seq,
    })
}

// ---------------------------------------------------------------------------
//  Metrics
// ---------------------------------------------------------------------------

struct Metrics {
    raw_bytes: Gauge,
    compressed_bytes: Gauge,
    compression_ratio: Gauge,
    json_bytes: Gauge,
    temperature: Gauge,
    humidity: Gauge,
    pressure: Gauge,
    message_count: IntCounter,
}

impl Metrics {
    fn new(registry: &Registry) -> Self {
        let m = Metrics {
            raw_bytes: Gauge::new("alec_payload_raw_bytes", "Raw payload size in bytes").unwrap(),
            compressed_bytes: Gauge::new(
                "alec_payload_compressed_bytes",
                "Compressed payload size in bytes",
            )
            .unwrap(),
            compression_ratio: Gauge::new(
                "alec_compression_ratio",
                "Compression ratio (raw / compressed)",
            )
            .unwrap(),
            json_bytes: Gauge::new(
                "alec_payload_json_bytes",
                "Equivalent JSON payload size in bytes",
            )
            .unwrap(),
            temperature: Gauge::new("alec_temperature", "Decoded temperature in °C").unwrap(),
            humidity: Gauge::new("alec_humidity", "Decoded humidity in %RH").unwrap(),
            pressure: Gauge::new("alec_pressure", "Decoded pressure in Pa").unwrap(),
            message_count: IntCounter::new("alec_message_count", "Total messages received")
                .unwrap(),
        };
        registry.register(Box::new(m.raw_bytes.clone())).unwrap();
        registry
            .register(Box::new(m.compressed_bytes.clone()))
            .unwrap();
        registry
            .register(Box::new(m.compression_ratio.clone()))
            .unwrap();
        registry
            .register(Box::new(m.json_bytes.clone()))
            .unwrap();
        registry
            .register(Box::new(m.temperature.clone()))
            .unwrap();
        registry.register(Box::new(m.humidity.clone())).unwrap();
        registry.register(Box::new(m.pressure.clone())).unwrap();
        registry
            .register(Box::new(m.message_count.clone()))
            .unwrap();
        m
    }
}

// ---------------------------------------------------------------------------
//  HTTP metrics server (tiny_http)
// ---------------------------------------------------------------------------

fn start_metrics_server(port: u16, registry: Arc<Registry>) {
    std::thread::spawn(move || {
        let addr = format!("0.0.0.0:{}", port);
        let server = tiny_http::Server::http(&addr).expect("failed to bind metrics server");
        info!("Metrics server listening on {}", addr);
        let encoder = TextEncoder::new();
        for req in server.incoming_requests() {
            let metric_families = registry.gather();
            let mut buf = Vec::new();
            encoder.encode(&metric_families, &mut buf).unwrap();
            let resp = tiny_http::Response::from_data(buf)
                .with_header("Content-Type: text/plain; charset=utf-8".parse().unwrap());
            let _ = req.respond(resp);
        }
    });
}

// ---------------------------------------------------------------------------
//  MQTT loop
// ---------------------------------------------------------------------------

fn run(broker: &str, metrics: &Metrics) {
    let create_opts = paho_mqtt::CreateOptionsBuilder::new()
        .server_uri(broker)
        .client_id("alec-decoder")
        .finalize();

    let cli = paho_mqtt::Client::new(create_opts).expect("failed to create MQTT client");

    let conn_opts = paho_mqtt::ConnectOptionsBuilder::new()
        .keep_alive_interval(Duration::from_secs(30))
        .clean_session(true)
        .automatic_reconnect(Duration::from_secs(1), Duration::from_secs(30))
        .finalize();

    let rx = cli.start_consuming();

    info!("Connecting to MQTT broker at {} ...", broker);
    loop {
        match cli.connect(conn_opts.clone()) {
            Ok(_) => break,
            Err(e) => {
                warn!("MQTT connect failed: {} — retrying in 3s", e);
                std::thread::sleep(Duration::from_secs(3));
            }
        }
    }
    info!("Connected to MQTT broker");

    cli.subscribe_many(&[TOPIC_DEMO, TOPIC_RAW], &[1, 1])
        .expect("failed to subscribe to topics");
    info!("Subscribed to {} and {}", TOPIC_DEMO, TOPIC_RAW);

    // Single shared ALEC context maintained across all messages (server-side state)
    let mut ctx = Context::new();
    let mut decoder = Decoder::new();

    for msg in rx.iter() {
        if let Some(msg) = msg {
            let topic = msg.topic();
            let payload = msg.payload();
            let wire_len = payload.len();
            metrics.message_count.inc();

            match topic {
                TOPIC_DEMO => {
                    // ALEC-compressed payload
                    metrics.compressed_bytes.set(wire_len as f64);

                    let raw = decoder.decode(payload, &mut ctx);
                    let raw_len = raw.len();
                    metrics.raw_bytes.set(raw_len as f64);
                    if wire_len > 0 {
                        metrics
                            .compression_ratio
                            .set(raw_len as f64 / wire_len as f64);
                    }

                    match parse_raw(&raw) {
                        Some(reading) => {
                            metrics.temperature.set(reading.temperature_c);
                            metrics.humidity.set(reading.humidity_rh);
                            metrics.pressure.set(reading.pressure_pa as f64);

                            let json_equiv = format!(
                                "{{\"t\":{:.2},\"rh\":{:.2},\"p\":{},\"ts\":{},\"seq\":{}}}",
                                reading.temperature_c,
                                reading.humidity_rh,
                                reading.pressure_pa,
                                reading.timestamp_ms,
                                reading.sequence,
                            );
                            metrics.json_bytes.set(json_equiv.len() as f64);

                            info!(
                                "ALEC t={:.2}°C rh={:.2}% pa={}Pa wire={}B raw={}B json={}B ratio={:.1}x",
                                reading.temperature_c,
                                reading.humidity_rh,
                                reading.pressure_pa,
                                wire_len,
                                raw_len,
                                json_equiv.len(),
                                raw_len as f64 / wire_len as f64
                            );
                        }
                        None => {
                            warn!(
                                "ALEC decoded {} bytes but could not parse sensor struct",
                                raw_len
                            );
                        }
                    }
                }
                TOPIC_RAW => {
                    // Uncompressed raw binary struct (for comparison)
                    metrics.raw_bytes.set(wire_len as f64);
                    metrics.compressed_bytes.set(wire_len as f64);
                    metrics.compression_ratio.set(1.0);

                    match parse_raw(payload) {
                        Some(reading) => {
                            metrics.temperature.set(reading.temperature_c);
                            metrics.humidity.set(reading.humidity_rh);
                            metrics.pressure.set(reading.pressure_pa as f64);
                            info!(
                                "RAW  seq={} t={:.2}°C rh={:.2}% pa={}Pa wire={}B",
                                reading.sequence,
                                reading.temperature_c,
                                reading.humidity_rh,
                                reading.pressure_pa,
                                wire_len
                            );
                        }
                        None => {
                            warn!(
                                "Could not parse raw payload ({} bytes) — skipping",
                                wire_len
                            );
                        }
                    }
                }
                _ => {
                    warn!("Unexpected topic: {}", topic);
                }
            }
        } else if !cli.is_connected() {
            warn!("Lost connection — waiting for reconnect...");
            std::thread::sleep(Duration::from_secs(1));
        }
    }
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------

fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let broker = std::env::var("MQTT_BROKER_EXTERNAL")
        .or_else(|_| std::env::var("MQTT_BROKER"))
        .unwrap_or_else(|_| "tcp://localhost:1883".into());
    let port: u16 = std::env::var("METRICS_PORT")
        .unwrap_or_else(|_| "9100".into())
        .parse()
        .expect("METRICS_PORT must be a u16");

    info!("ALEC Decoder Service starting");

    let registry = Arc::new(Registry::new());
    let metrics = Metrics::new(&registry);

    start_metrics_server(port, registry);

    run(&broker, &metrics);
}
