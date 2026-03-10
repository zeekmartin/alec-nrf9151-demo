//! ALEC Decoder Service
//!
//! Subscribes to MQTT topics:
//!   - alec/sensor/demo  — ALEC-compressed payloads (decoded with alec crate)
//!   - alec/sensor/raw   — raw binary payloads (for comparison)
//!
//! Exposes Prometheus metrics on :9100/metrics.

use alec::{Context, Decoder, EncodedMessage};
use byteorder::{LittleEndian, ReadBytesExt};
use log::{info, warn};
use prometheus::{Encoder as PromEncoder, Gauge, IntCounter, Registry, TextEncoder};
use rumqttc::{AsyncClient, Event, MqttOptions, Packet, QoS};
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

/// Reconstruct a SensorPayload from alec decode_multi output.
/// Field indices: 0=temp_x100, 1=hum_x100, 2=pressure_pa, 3=timestamp_ms, 4=sequence
fn payload_from_fields(fields: &[(u8, f64)]) -> Option<SensorPayload> {
    if fields.len() < 5 {
        return None;
    }
    Some(SensorPayload {
        temperature_c: fields[0].1 / 100.0,
        humidity_rh: fields[1].1 / 100.0,
        pressure_pa: fields[2].1 as u32,
        timestamp_ms: fields[3].1 as i64,
        sequence: fields[4].1 as u32,
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
        let addr = format!("0.0.0.0:{port}");
        let server = tiny_http::Server::http(&addr).expect("failed to bind metrics server");
        info!("Metrics server listening on {}", addr);
        let encoder = TextEncoder::new();
        for req in server.incoming_requests() {
            let metric_families = registry.gather();
            let mut buf = Vec::new();
            encoder.encode(&metric_families, &mut buf).unwrap();
            let resp = tiny_http::Response::from_data(buf)
                .with_header("Content-Type: text/plain; charset=utf-8".parse::<tiny_http::Header>().unwrap());
            let _ = req.respond(resp);
        }
    });
}

// ---------------------------------------------------------------------------
//  MQTT loop (rumqttc async)
// ---------------------------------------------------------------------------

fn parse_broker(env_val: &str) -> (String, u16) {
    let stripped = env_val
        .trim()
        .strip_prefix("tcp://")
        .unwrap_or(env_val.trim());
    match stripped.rsplit_once(':') {
        Some((host, port_str)) => {
            let port = port_str.parse::<u16>().unwrap_or(1883);
            (host.to_string(), port)
        }
        None => (stripped.to_string(), 1883),
    }
}

fn update_metrics_from_reading(metrics: &Metrics, reading: &SensorPayload, wire_len: usize) {
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
        "t={:.2}°C rh={:.2}% pa={}Pa wire={}B json={}B",
        reading.temperature_c,
        reading.humidity_rh,
        reading.pressure_pa,
        wire_len,
        json_equiv.len(),
    );
}

async fn run(host: &str, port: u16, metrics: &Metrics) {
    let mut mqttoptions = MqttOptions::new("alec-decoder", host, port);
    mqttoptions.set_keep_alive(Duration::from_secs(30));
    mqttoptions.set_clean_session(true);

    let (client, mut eventloop) = AsyncClient::new(mqttoptions, 64);

    info!("Connecting to MQTT broker at {}:{} ...", host, port);

    client
        .subscribe(TOPIC_DEMO, QoS::AtLeastOnce)
        .await
        .expect("failed to subscribe to demo topic");
    client
        .subscribe(TOPIC_RAW, QoS::AtLeastOnce)
        .await
        .expect("failed to subscribe to raw topic");
    info!("Subscribed to {} and {}", TOPIC_DEMO, TOPIC_RAW);

    let ctx = Context::new();
    let mut decoder = Decoder::new();

    loop {
        match eventloop.poll().await {
            Ok(Event::Incoming(Packet::Publish(publish))) => {
                let topic = &publish.topic;
                let payload = &publish.payload;
                let wire_len = payload.len();
                metrics.message_count.inc();

                match topic.as_str() {
                    TOPIC_DEMO => {
                        metrics.compressed_bytes.set(wire_len as f64);

                        let msg = match EncodedMessage::from_bytes(payload) {
                            Some(m) => m,
                            None => {
                                warn!("Could not parse ALEC message ({} bytes)", wire_len);
                                continue;
                            }
                        };

                        match decoder.decode_multi(&msg, &ctx) {
                            Ok(fields) => {
                                metrics.raw_bytes.set(RAW_PAYLOAD_SIZE as f64);
                                if wire_len > 0 {
                                    metrics
                                        .compression_ratio
                                        .set(RAW_PAYLOAD_SIZE as f64 / wire_len as f64);
                                }

                                match payload_from_fields(&fields) {
                                    Some(reading) => {
                                        info!(
                                            "ALEC decoded {} fields, ratio={:.1}x",
                                            fields.len(),
                                            RAW_PAYLOAD_SIZE as f64 / wire_len as f64
                                        );
                                        update_metrics_from_reading(metrics, &reading, wire_len);
                                    }
                                    None => {
                                        warn!(
                                            "ALEC decoded {} fields but expected >= 5",
                                            fields.len()
                                        );
                                    }
                                }
                            }
                            Err(e) => {
                                warn!("ALEC decode_multi failed: {} ({} bytes)", e, wire_len);
                            }
                        }
                    }
                    TOPIC_RAW => {
                        metrics.raw_bytes.set(wire_len as f64);
                        metrics.compressed_bytes.set(wire_len as f64);
                        metrics.compression_ratio.set(1.0);

                        match parse_raw(payload) {
                            Some(reading) => {
                                info!("RAW  seq={}", reading.sequence);
                                update_metrics_from_reading(metrics, &reading, wire_len);
                            }
                            None => {
                                warn!(
                                    "Could not parse raw payload ({} bytes) — skipping",
                                    wire_len
                                );
                            }
                        }
                    }
                    other => {
                        warn!("Unexpected topic: {}", other);
                    }
                }
            }
            Ok(_) => {}
            Err(e) => {
                warn!("MQTT error: {} — reconnecting in 3s", e);
                tokio::time::sleep(Duration::from_secs(3)).await;
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------

#[tokio::main]
async fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let broker_raw = std::env::var("MQTT_BROKER_EXTERNAL")
        .or_else(|_| std::env::var("MQTT_BROKER"))
        .unwrap_or_else(|_| "localhost:1883".into());
    let (host, mqtt_port) = parse_broker(&broker_raw);

    let metrics_port: u16 = std::env::var("METRICS_PORT")
        .unwrap_or_else(|_| "9100".into())
        .parse()
        .expect("METRICS_PORT must be a u16");

    info!("ALEC Decoder Service starting");

    let registry = Arc::new(Registry::new());
    let metrics = Metrics::new(&registry);

    start_metrics_server(metrics_port, registry);

    run(&host, mqtt_port, &metrics).await;
}
