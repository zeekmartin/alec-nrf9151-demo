# ALEC × Nordic nRF9151 — NB-IoT Compression Demo

A live demonstration of [ALEC](https://alec-codec.com) streaming compression on a Nordic Semiconductor nRF9151 development kit over NB-IoT.

This project shows that per-message payload compression can reduce IoT data transmission by **80–95%** on constrained cellular networks — with zero added power draw on the device.

> **Status**: Live on NB-IoT. alec-ffi v1.3.5 validated on-device. Compact fixed-channel mode active — steady-state payload ~11B for 5 channels.

---

## What This Demo Does

A Nordic nRF9151 SMA-DK board sends a simulated 5-channel sensor reading (battery, temperature, humidity, CO2, pressure) over NB-IoT every 5 seconds. Each payload is compressed with the ALEC encoder before transmission. A backend server receives, decodes, and forwards metrics to a Grafana dashboard showing real-time compression performance.

```
nRF9151 (NB-IoT)
  └─ Sensor data (simulated)
  └─ ALEC Encoder (alec-ffi 1.3.5, zephyr feature)
  └─ Compressed payload → NB-IoT → MQTT broker
                                        └─ ALEC Decoder (Rust, alec 1.3.5)
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
│   │       ├── libalec_ffi.a   # Pre-built: alec-ffi 1.3.5 (thumbv8m.main-none-eabi)
│   │       ├── include/
│   │       │   └── alec.h      # C FFI declarations
│   │       └── CMakeLists.txt
│   ├── CMakeLists.txt
│   └── prj.conf
├── backend/                    # MQTT broker + ALEC decoder + Prometheus bridge
│   ├── decoder/                # Rust: alec 1.3.5 decoder + Prometheus metrics
│   └── docker-compose.yml      # mosquitto + decoder + prometheus + grafana
└── README.md
```

---

## Stack

| Layer | Technology |
|---|---|
| Device firmware | Zephyr RTOS / nRF Connect SDK v2.9.2 |
| Compression (device) | alec-ffi 1.3.5 (`zephyr` feature) |
| Transport | NB-IoT → MQTT |
| Compression (server) | alec 1.3.5 Rust decoder |
| Metrics | Prometheus |
| Dashboard | Grafana |
| Local dev | Docker Desktop (Windows) |

---

## Demo Payload

Simulated EM500-CO2 profile sent every 5 seconds:

| Channel | Type | Range |
|---|---|---|
| Battery | double (%) | 100.0 (constant) |
| Temperature | double (°C) | 24.0 – 27.0 |
| Humidity | double (%RH) | 55.0 – 65.0 |
| CO2 | double (ppm) | 400.0 – 700.0 |
| Pressure | double (hPa) | 1000.0 – 1015.0 |

**Wire format (ALEC v1.3.5 compact fixed-channel):**

| Message | Marker | Size | Notes |
|---|---|---|---|
| #1 (cold start) | 0xA2 | 27B → fallback | Raw32 all channels |
| #2–7 (convergence) | 0xA1 | 11–15B → fallback | Context building |
| #8+ (steady state) | 0xA1 | ~11B | Delta8 dominant |
| #50, #100… (keyframe) | 0xA2 | 27B → fallback | Periodic resync |

Fallback rule: if encoded output > 11B, raw struct sent to `alec/sensor/demo` instead.

MQTT topics:
- `alec/sensor/demo` — ALEC frame or raw fallback
- `alec/sensor/raw` — uncompressed raw (always sent)

---

## Building libalec_ffi.a

The pre-built `libalec_ffi.a` in `firmware/lib/alec/` targets `thumbv8m.main-none-eabi` with the `zephyr` feature (alec-ffi 1.3.5). To rebuild:

```bash
# In the alec-codec repository
rustup target add thumbv8m.main-none-eabi
cd alec-ffi

# For Zephyr targets (nRF9151):
cargo build --release \
  --target thumbv8m.main-none-eabi \
  --no-default-features --features zephyr

# For bare-metal targets (no OS):
cargo build --release \
  --target thumbv8m.main-none-eabi \
  --no-default-features --features bare-metal

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
[00:00:03] <inf> alec_demo: ALEC NB-IoT Sensor Demo starting (alec-ffi 1.3.5)
[00:00:03] <inf> alec_demo: Heap pool size: 98304 bytes
[00:00:03] <inf> alec_demo: ALEC self-test OK: 27B output, marker=0xA2
[00:00:03] <inf> alec_demo: LTE registered
[00:00:xx] <inf> alec_demo: MQTT connected
[00:00:xx] <inf> alec_demo: ────────────────────────
[00:00:xx] <inf> alec_demo: ALEC v1.3.5  seq=0  bat=100%  t=26.9°C  rh=58.5%  co2=641ppm  p=1007.7hPa
[00:00:xx] <inf> alec_demo:   Frame marker : 0xA2 (KEYFRAME)
[00:00:xx] <inf> alec_demo:   ALEC output  : 27B (fallback)
[00:00:xx] <wrn> alec_demo:   fallback: frame exceeded 11B, sent raw
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

### alec-ffi 1.3.5 — Compact fixed-channel mode ✅

New in this release:
- `alec_encoder_new_with_config()` — configurable `history_size`, `keyframe_interval`, `smart_resync`
- `alec_encode_multi_fixed()` — positional encoding, no name_ids, no timestamps, 4B header + 2B bitmap
- Wire format: `0xA1` (data) / `0xA2` (keyframe)
- Packet loss recovery: periodic keyframe (default N=50) + sequence gap detection + `reset_to_baseline()`
- LoRaWAN downlink smart resync: `0xFF` command via `alec_downlink_handler()` — worst-case drift reduced from N×interval to 1×interval
- Context persistence: `alec_decoder_export_state()` / `alec_decoder_import_state()` — ALCS binary format, ~1.5KB per context, CRC32 verified
- Multi-arch verified: M3 / M4 / M4F / M0+
- Fallback: if encoded output > 11B → caller sends raw payload (no silent corruption)

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