///! ALEC Decoder Service
///!
///! Subscribes to MQTT topic `alec/sensor/demo`, decodes incoming payloads
///! (ALEC-compressed or raw), and exposes Prometheus metrics on :9100/metrics.

use alec::{Context, Decoder, EncodedMessage};
use byteorder::{LittleEndian, ReadBytesExt};
use log::{info, warn};
use prometheus::{Gauge, IntCounter, Registry, TextEncoder};
use std::io::Cursor;
use std::sync::Arc;
use std::time::Duration;

// ---------------------------------------------------------------------------
//  Sensor payload (mirrors firmware struct sensor_payload, 18 bytes packed)
//  Used as fallback when receiving uncompressed (raw) messages.
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
//  ALEC decoding helpers
// ---------------------------------------------------------------------------

struct AlecState {
    decoder: Decoder,
    context: Context,
}

impl AlecState {
    fn new() -> Self {
        Self {
            decoder: Decoder::new(),
            context: Context::new(),
        }
    }

    /// Try to decode an ALEC-encoded message from wire bytes.
    /// Returns (value, timestamp) on success.
    fn try_decode(&mut self, wire: &[u8]) -> Option<(f64, u64)> {
        let msg = EncodedMessage::from_bytes(wire).ok()?;
        let decoded = self.decoder.decode(&msg, &self.context).ok()?;
        Some((decoded.value, decoded.timestamp))
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

    // Per-field ALEC decoders (firmware encodes each sensor field separately)
    let mut alec_temp = AlecState::new();
    let mut alec_hum = AlecState::new();
    let mut alec_pres = AlecState::new();

    for msg in rx.iter() {
        if let Some(msg) = msg {
            let payload = msg.payload();
            let wire_len = payload.len();
            metrics.message_count.inc();
            metrics.compressed_bytes.set(wire_len as f64);

            // Try ALEC decode first: firmware sends 3 concatenated ALEC messages
            // (temp + humidity + pressure), each prefixed with a 2-byte length.
            if let Some(reading) = try_alec_decode(
                payload,
                &mut alec_temp,
                &mut alec_hum,
                &mut alec_pres,
            ) {
                let raw_size = RAW_PAYLOAD_SIZE as f64;
                metrics.raw_bytes.set(raw_size);
                if wire_len > 0 {
                    metrics.compression_ratio.set(raw_size / wire_len as f64);
                }
                metrics.temperature.set(reading.temperature_c);
                metrics.humidity.set(reading.humidity_rh);
                metrics.pressure.set(reading.pressure_pa as f64);
                info!(
                    "ALEC seq=- t={:.2}°C rh={:.2}% pa={}Pa wire={}B raw={}B",
                    reading.temperature_c,
                    reading.humidity_rh,
                    reading.pressure_pa,
                    wire_len,
                    RAW_PAYLOAD_SIZE
                );
            } else if let Some(reading) = parse_raw(payload) {
                // Fallback: uncompressed raw binary struct
                metrics.raw_bytes.set(wire_len as f64);
                metrics.compression_ratio.set(1.0);
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
            } else {
                warn!(
                    "Could not parse payload ({} bytes) — skipping",
                    wire_len
                );
            }
        } else if !cli.is_connected() {
            warn!("Lost connection — waiting for reconnect...");
            std::thread::sleep(Duration::from_secs(1));
        }
    }
}

/// Attempt to decode an ALEC multi-field payload.
///
/// Wire format from firmware:
///   [u16 len_a][alec_msg_a][u16 len_b][alec_msg_b][u16 len_c][alec_msg_c]
///
/// Each alec_msg is the output of alec_encode_value() (EncodedMessage::to_bytes()).
/// Fields in order: temperature (°C×100 as f64), humidity (%RH×100 as f64), pressure (Pa as f64).
fn try_alec_decode(
    payload: &[u8],
    alec_temp: &mut AlecState,
    alec_hum: &mut AlecState,
    alec_pres: &mut AlecState,
) -> Option<SensorPayload> {
    let mut cur = Cursor::new(payload);

    // Read temperature message
    let len_t = cur.read_u16::<LittleEndian>().ok()? as usize;
    let pos = cur.position() as usize;
    if pos + len_t > payload.len() {
        return None;
    }
    let (temp_val, _) = alec_temp.try_decode(&payload[pos..pos + len_t])?;
    cur.set_position((pos + len_t) as u64);

    // Read humidity message
    let len_h = cur.read_u16::<LittleEndian>().ok()? as usize;
    let pos = cur.position() as usize;
    if pos + len_h > payload.len() {
        return None;
    }
    let (hum_val, _) = alec_hum.try_decode(&payload[pos..pos + len_h])?;
    cur.set_position((pos + len_h) as u64);

    // Read pressure message
    let len_p = cur.read_u16::<LittleEndian>().ok()? as usize;
    let pos = cur.position() as usize;
    if pos + len_p > payload.len() {
        return None;
    }
    let (pres_val, _) = alec_pres.try_decode(&payload[pos..pos + len_p])?;

    Some(SensorPayload {
        temperature_c: temp_val / 100.0,
        humidity_rh: hum_val / 100.0,
        pressure_pa: pres_val as u32,
        timestamp_ms: 0,
        sequence: 0,
    })
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

    info!("ALEC Decoder Service (alec crate v{})", alec::VERSION);

    let registry = Arc::new(Registry::new());
    let metrics = Metrics::new(&registry);

    start_metrics_server(port, registry);

    run(&broker, &topic, &metrics);
}
