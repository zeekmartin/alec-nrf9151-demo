# ALEC × Nordic nRF9151 — NB-IoT Compression Demo

A live demonstration of [ALEC](https://alec-codec.com) streaming compression on a Nordic Semiconductor nRF9151 development kit over NB-IoT.

This project shows that per-message payload compression can reduce IoT data transmission by **80–95%** on constrained cellular networks — with zero added power draw on the device.

> **Status**: Firmware boots and initialises successfully (alec-ffi 1.2.3 confirmed on-device). NB-IoT connectivity pending SIM activation.

---

## What This Demo Does

A Nordic nRF9151 SMA-DK board sends simulated sensor telemetry (temperature, humidity, pressure, timestamp) over NB-IoT every 60 seconds. Each payload is compressed with the ALEC encoder before transmission. A backend server receives, decodes, and forwards metrics to a Grafana dashboard showing real-time compression performance.

```
nRF9151 (NB-IoT)
  └─ Sensor data (simulated)
  └─ ALEC Encoder (alec-ffi 1.2.3, zephyr feature)
  └─ Compressed payload → NB-IoT → MQTT broker
                                        └─ ALEC Decoder (Rust, alec 1.2.3)
                                        └─ Prometheus bridge
                                        └─ Grafana dashboard
```

---

## Hardware

- **Nordic nRF9151 SMA-DK** development kit (REV3)
- LTE/NB-IoT antenna (SMA, included in kit)
- NB-IoT SIM card (Deutsche Telekom / Monogoto / Onomondo)

**Board connections:**
- USB-C **DEBUG port** → PC (J-Link, RTT, flashing) — requires a data-capable USB-C cable
- LTE antenna → **LTE connector** on board (not GNSS)
- SIM → SIM slot (power off board before inserting)

---

## Repository Structure

```
alec-nrf9151-demo/
├── firmware/                   # Zephyr/nRF Connect SDK application (C)
│   ├── src/
│   │   ├── main.c              # Main application
│   │   └── critical_section.c # Zephyr irq_lock shim for alec-ffi
│   ├── lib/
│   │   └── alec/
│   │       ├── libalec_ffi.a   # Pre-built: alec-ffi 1.2.3 (thumbv8m.main-none-eabi)
│   │       ├── include/
│   │       │   └── alec.h      # C FFI declarations
│   │       └── CMakeLists.txt
│   ├── CMakeLists.txt
│   └── prj.conf
├── backend/                    # MQTT broker + ALEC decoder + Prometheus bridge
│   ├── decoder/                # Rust: alec 1.2.3 decoder + Prometheus metrics
│   └── docker-compose.yml      # mosquitto + decoder + prometheus + grafana
└── README.md
```

---

## Stack

| Layer | Technology |
|---|---|
| Device firmware | Zephyr RTOS / nRF Connect SDK v2.9.2 |
| Compression (device) | alec-ffi 1.2.3 (`zephyr` feature) |
| Transport | NB-IoT → MQTT |
| Compression (server) | alec 1.2.3 Rust decoder |
| Metrics | Prometheus |
| Dashboard | Grafana |
| Local dev | Docker Desktop (Windows) |

---

## Demo Payload

Simulated IoT sensor payload sent every 60 seconds:

| Field | Type | Raw size |
|---|---|---|
| Temperature | int16 (°C × 100) | 2 bytes |
| Humidity | uint16 (%RH × 100) | 2 bytes |
| Pressure | uint32 (Pa) | 4 bytes |
| Timestamp | int64 (ms epoch) | 8 bytes |
| Sequence counter | uint32 | 4 bytes |
| **Total** | | **20 bytes** |

Expected compressed size with ALEC: **2–4 bytes** (~80–90% reduction).

MQTT topics:
- `alec/sensor/demo` — ALEC-compressed payload
- `alec/sensor/raw` — uncompressed raw struct (for comparison)

---

## Building libalec_ffi.a

The pre-built `libalec_ffi.a` in `firmware/lib/alec/` targets `thumbv8m.main-none-eabi` with the `zephyr` feature (alec-ffi 1.2.3). To rebuild:

```bash
# In the alec-codec repository
rustup target add thumbv8m.main-none-eabi
cd alec-ffi
cargo build --release --target thumbv8m.main-none-eabi --no-default-features --features zephyr
cp ../target/thumbv8m.main-none-eabi/release/libalec_ffi.a \
   /path/to/alec-nrf9151-demo/firmware/lib/alec/
```

**Important — target selection:**
- Use `thumbv8m.main-none-eabi` (**not** `eabihf`) — Zephyr's nRF9151 toolchain compiles in `nofp` (no hardware float). Using `eabihf` causes an ABI mismatch at link time.

---

## Firmware Setup

### Prerequisites

- [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop)
- nRF Connect SDK v2.9.2
- VS Code + nRF Connect extension
- Docker Desktop

### Build & Flash

1. Open `firmware/` as an application in VS Code nRF Connect panel
2. Add build configuration: board target `nrf9151dk/nrf9151/ns`
3. Click **Build**
4. Power off board → insert SIM → connect LTE antenna → power on
5. Click **Flash**
6. Open **RTT** in Connected Devices panel to view logs

### Expected RTT output on boot

```
*** Booting nRF Connect SDK v2.9.2 ***
[00:00:03] <inf> alec_demo: ALEC NB-IoT Sensor Demo starting (alec-ffi 1.2.3)
[00:00:03] <inf> alec_demo: Initialising modem library...
[00:00:03] <inf> alec_demo: Connecting to LTE network (NB-IoT preferred)...
[00:00:xx] <inf> alec_demo: LTE registered
```

---

## Backend Setup

```bash
cd backend
docker compose up
```

- Grafana: http://localhost:3000
- Prometheus: http://localhost:9090
- MQTT broker: localhost:1883

---

## Integration Notes — alec-ffi on Zephyr

This project drove several improvements to alec-ffi that are now part of the official releases:

### alec-ffi 1.2.0 — no_std support
Initial no_std port. Replaces `thiserror` with manual error handling, `crc32fast` with `crc` v3 (no_std compatible). Enables embedded targets in principle but requires allocator and panic handler.

### alec-ffi 1.2.1 — bare-metal support
Adds `bare-metal` feature: provides `embedded-alloc` (LlffHeap, 8KB) as global allocator and a `loop {}` panic handler. Suitable for bare-metal RTOS-less targets. **Not suitable for Zephyr** — Zephyr provides its own allocator and panic handler; using bare-metal causes duplicate symbol conflicts at link time.

### alec-ffi 1.2.2 — Zephyr allocator (intermediate)
First attempt at Zephyr support using `k_malloc`/`k_free`. Caused the `ZephyrAllocator` static initialiser to run before the Zephyr heap was ready when linked with `--whole-archive`, crashing the firmware at boot.

### alec-ffi 1.2.3 — Zephyr support (stable) ✅
Correct Zephyr integration:
- `zephyr` feature: `k_malloc`/`k_free` as global allocator, `loop {}` panic handler
- **No `--whole-archive`** in CMakeLists — linker pulls only referenced symbols, avoiding premature static initialisation
- `critical_section.c` in firmware provides `_critical_section_1_0_acquire/release` via Zephyr `irq_lock`/`irq_unlock`

**Usage in Zephyr firmware:**
```toml
alec-ffi = { version = "1.2.3", default-features = false, features = ["zephyr"] }
```

```cmake
# firmware/lib/alec/CMakeLists.txt — no --whole-archive
add_library(alec_ffi STATIC IMPORTED GLOBAL)
set_target_properties(alec_ffi PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libalec_ffi.a
)
target_include_directories(alec_ffi INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
```

---

## Related Resources

- [ALEC Platform](https://alec-codec.com)
- [ALEC Documentation](https://alec-codec.com/docs)
- [alec on crates.io](https://crates.io/crates/alec)
- [alec-ffi on crates.io](https://crates.io/crates/alec-ffi)
- [Nordic nRF9151 Product Page](https://www.nordicsemi.com/Products/nRF9151)

---

## License

MIT — see [LICENSE](LICENSE)

---

*Built by [David Martin](https://alec-codec.com) — ALEC Platform*