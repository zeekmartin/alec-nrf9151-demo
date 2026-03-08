# ALEC × Nordic nRF9151 — NB-IoT Compression Demo

A live demonstration of [ALEC](https://alec-codec.com) streaming compression on a Nordic Semiconductor nRF9151 development kit over NB-IoT.

This project shows that per-message payload compression can reduce IoT data transmission by **80–95%** on constrained cellular networks — with zero added power draw on the device.

---

## What This Demo Does

A Nordic nRF9151 SMA-DK board sends simulated sensor telemetry (temperature, humidity, pressure, timestamp) over NB-IoT every 60 seconds. Each payload is compressed with the ALEC encoder before transmission. A backend server receives, decodes, and forwards metrics to a Grafana dashboard showing real-time compression performance.

```
nRF9151 (NB-IoT)
  └─ Sensor data (simulated)
  └─ ALEC Encoder (C FFI)
  └─ Compressed payload → NB-IoT → MQTT broker
                                        └─ ALEC Decoder
                                        └─ Prometheus bridge
                                        └─ Grafana dashboard
```

---

## Hardware

- **Nordic nRF9151 SMA-DK** development kit
- LTE/NB-IoT antenna (included)
- SIM card (NB-IoT enabled)

---

## Repository Structure

```
alec-nrf9151-demo/
├── firmware/           # Zephyr/nRF Connect SDK application (C)
│   ├── src/
│   │   └── main.c
│   ├── CMakeLists.txt
│   └── prj.conf
├── backend/            # MQTT broker + ALEC decoder + Prometheus bridge
│   ├── decoder/
│   ├── mqtt_bridge/
│   └── docker-compose.yml
├── grafana/            # Dashboard provisioning
│   └── dashboards/
├── docs/               # Architecture notes, setup guides
└── README.md
```

---

## Stack

| Layer | Technology |
|---|---|
| Device firmware | Zephyr RTOS / nRF Connect SDK |
| Compression (device) | ALEC C FFI (`alec-ffi`) |
| Transport | NB-IoT / LTE-M → MQTT |
| Compression (server) | ALEC Rust decoder |
| Metrics | Prometheus |
| Dashboard | Grafana |
| Local dev | Docker Desktop (Windows) |

---

## Demo Data

Simulated IoT sensor payload sent every 60 seconds:

| Field | Type | Raw size |
|---|---|---|
| Temperature | f32 | 4 bytes |
| Humidity | f32 | 4 bytes |
| Pressure | f32 | 4 bytes |
| Timestamp | u32 | 4 bytes |
| Sequence counter | u16 | 2 bytes |
| **Total** | | **18 bytes** |

Expected compressed size with ALEC: **2–4 bytes** (~85–90% reduction).

---

## Setup

> Step-by-step guides will be added in `/docs` as the project progresses.

### Prerequisites

- [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop)
- [nRF Connect SDK](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/getting_started.html)
- Docker Desktop
- Git

### Quick Start (backend only)

```bash
git clone https://github.com/zeekmartin/alec-nrf9151-demo.git
cd alec-nrf9151-demo/backend
docker compose up
```

Grafana available at `http://localhost:3000`

---

## Related Resources

- [ALEC Platform](https://alec-codec.com)
- [ALEC Documentation](https://alec-codec.com/docs)
- [ALEC on crates.io](https://crates.io/crates/alec-codec)
- [ALEC C FFI](https://crates.io/crates/alec-ffi)
- [Live Demo](https://demo.alec-codec.com)
- [Nordic nRF9151 Product Page](https://www.nordicsemi.com/Products/nRF9151)

---

## License

MIT — see [LICENSE](LICENSE)

---

*Built by [David Martin](https://alec-codec.com) — ALEC Platform*
