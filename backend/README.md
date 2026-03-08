# ALEC NB-IoT Demo — Backend

Backend stack for the nRF9151 ALEC compression demo. Receives MQTT messages
from the board, decodes ALEC-compressed (or raw) sensor payloads, and
visualises metrics in Grafana.

## Architecture

```
nRF9151 board  ──MQTT──▶  Mosquitto (:1883)
                              │
                     alec-decoder (Rust)
                         │         │
                   Prometheus    stdout logs
                    (:9090)
                         │
                      Grafana
                    (:3000)
```

## Quick Start

```bash
cd backend
docker compose up --build
```

- **Grafana**: http://localhost:3000 (login: `admin` / `admin`)
- **Prometheus**: http://localhost:9090
- **MQTT broker**: `localhost:1883` (no auth)

## Services

| Service        | Image / Build     | Port  | Purpose                              |
|----------------|-------------------|-------|--------------------------------------|
| mosquitto      | eclipse-mosquitto | 1883  | MQTT broker                          |
| alec-decoder   | `./decoder`       | 9100  | Decode payloads, expose metrics      |
| prometheus     | prom/prometheus   | 9090  | Scrapes decoder metrics every 5s     |
| grafana        | grafana/grafana   | 3000  | Dashboard with pre-provisioned panels|

## MQTT Topic

The board publishes to **`alec/sensor/demo`**. The decoder subscribes to the
same topic and expects either:

- **Raw binary** (18 bytes): the packed `sensor_payload` struct from firmware
- **ALEC-compressed**: will be decoded once `alec-codec` crate is integrated

## Metrics Exposed

| Metric                        | Type    | Description                    |
|-------------------------------|---------|--------------------------------|
| `alec_payload_raw_bytes`      | Gauge   | Raw (decoded) payload size     |
| `alec_payload_compressed_bytes`| Gauge  | Wire payload size              |
| `alec_compression_ratio`      | Gauge   | raw / compressed               |
| `alec_temperature`            | Gauge   | Decoded temperature (°C)       |
| `alec_humidity`               | Gauge   | Decoded humidity (%RH)         |
| `alec_pressure`               | Gauge   | Decoded pressure (Pa)          |
| `alec_message_count`          | Counter | Total messages received        |

## Notes

- The ALEC decoder currently uses a **stub** (`decoder/src/main.rs` →
  `mod alec_codec`). Replace with `use alec_codec::decode;` once the crate
  is published on crates.io.
- Mosquitto is configured with no authentication for development.
- Grafana dashboard is auto-provisioned on first boot.
