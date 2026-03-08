///! ALEC Decoder Service
///!
///! Subscribes to MQTT topic `alec/sensor/demo`, decodes incoming payloads
///! (ALEC-compressed or raw), and exposes Prometheus metrics on :9100/metrics.

use byteorder::{LittleEndian, ReadBytesExt};
use log::{error, info, warn};
use prometheus::{Gauge, IntCounter, Registry, TextEncoder};
use std::io::Cursor;
use std::sync::Arc;
use std::time::Duration;

// ---------------------------------------------------------------------------
//  Sensor payload (mirrors firmware struct sensor_payload, 18 bytes packed)
// ---------------------------------------------------------------------------

#[derive(Debug)]
struct SensorPayload {
    temperature_c: f64,  // °C
    humidity_rh: f64,    // %RH
    pressure_pa: u32,    // Pa
    timestamp_ms: i64,   // Unix epoch ms
    sequence: u32,
}

const RAW_PAYLOAD_SIZE: usize = 18; // i16+u16+u32+i64+u32

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
//  ALEC codec stub
//
//  When the real `alec-codec` crate is published on crates.io, replace this
//  module with:
//      use alec_codec::decode;
//  The function signature is kept identical for a drop-in swap.
// ---------------------------------------------------------------------------

mod alec_codec {
    /// Attempt to ALEC-decode `compressed` into a raw byte buffer.
    /// Returns `None` if the data is not ALEC-compressed.
    ///
    /// Stub: always returns None so we fall through to raw parsing.
    /// Replace with the real crate once available.
    pub fn decode(compressed: &[u8]) -> Option<Vec<u8>> {
        // ALEC frames are expected to start with a magic byte (0xA1).
        // If present, a real decoder would decompress here.
        if compressed.first().copied() == Some(0xA1) {
            // TODO: replace with real alec_codec::decode(compressed)
            log::warn!("ALEC magic detected but stub decoder active — returning raw fallback");
        }
        None
    }
}

// ---------------------------------------------------------------------------
//  Metrics
// ---------------------------------------------------------------------------

struct Metrics {
    raw_bytes: Gauge,
    compressed_bytes: Gauge,
    compression_ratio: Gauge,
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

fn run(broker: &str, topic: &str, metrics: &Metrics) {
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

    cli.subscribe(topic, 1)
        .expect("failed to subscribe to topic");
    info!("Subscribed to {}", topic);

    for msg in rx.iter() {
        if let Some(msg) = msg {
            let payload = msg.payload();
            let wire_len = payload.len();
            metrics.message_count.inc();
            metrics.compressed_bytes.set(wire_len as f64);

            // Try ALEC decode first, fallback to raw
            let raw = match alec_codec::decode(payload) {
                Some(decoded) => {
                    metrics.raw_bytes.set(decoded.len() as f64);
                    if wire_len > 0 {
                        metrics
                            .compression_ratio
                            .set(decoded.len() as f64 / wire_len as f64);
                    }
                    decoded
                }
                None => {
                    // Payload is already raw (uncompressed)
                    metrics.raw_bytes.set(wire_len as f64);
                    metrics.compression_ratio.set(1.0);
                    payload.to_vec()
                }
            };

            match parse_raw(&raw) {
                Some(reading) => {
                    metrics.temperature.set(reading.temperature_c);
                    metrics.humidity.set(reading.humidity_rh);
                    metrics.pressure.set(reading.pressure_pa as f64);
                    info!(
                        "seq={} t={:.2}°C rh={:.2}% pa={}Pa wire={}B raw={}B",
                        reading.sequence,
                        reading.temperature_c,
                        reading.humidity_rh,
                        reading.pressure_pa,
                        wire_len,
                        raw.len()
                    );
                }
                None => {
                    warn!(
                        "Could not parse payload ({} bytes) — skipping",
                        raw.len()
                    );
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

    let broker = std::env::var("MQTT_BROKER").unwrap_or_else(|_| "tcp://localhost:1883".into());
    let topic = std::env::var("MQTT_TOPIC").unwrap_or_else(|_| "alec/sensor/demo".into());
    let port: u16 = std::env::var("METRICS_PORT")
        .unwrap_or_else(|_| "9100".into())
        .parse()
        .expect("METRICS_PORT must be a u16");

    let registry = Arc::new(Registry::new());
    let metrics = Metrics::new(&registry);

    start_metrics_server(port, registry);

    run(&broker, &topic, &metrics);
}
