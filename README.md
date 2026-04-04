# 🐾 Hati Tracker Firmware

GPS tracker firmware for the **LilyGO T-A7670E** ESP32 board.
Built for tracking a dog (Hati, an Australian Cattle Dog) during canicross runs.

Live location is sent to a [Traccar](https://traccar.org) server via 4G over Telia Finland.

---

## Features

- **Live GPS tracking** — position, speed, altitude, satellite count
- **4G LTE** via Telia Finland (APN: `internet`)
- **Cell tower LBS fallback** — approximate position when GPS has no fix (indoors, dense forest)
- **Adaptive update rate** — every 10 seconds when moving, every 60 seconds when still
- **Battery monitoring** — voltage sent with every position update
- **GPS timestamps** — accurate UTC time sent to Traccar
- **Watchdog timer** — board auto-restarts if firmware hangs
- **LED status indicator** — single blink = OK, 5 blinks = error

---

## Hardware

| Component | Details |
|---|---|
| Board | LilyGO T-A7670E R2 (with GPS) |
| Modem | SIMCOM A7670E — 4G LTE Cat1, Europe bands |
| GPS | Built-in GNSS on A7670E module |
| Battery | 18650 Li-ion in onboard holder |
| SIM | Nano SIM (Telia Finland 200MB) |

### Pin Definitions

| Pin | GPIO | Function |
|---|---|---|
| Modem TX | 26 | ESP32 → A7670E |
| Modem RX | 27 | A7670E → ESP32 |
| PWRKEY | 4 | Modem power toggle |
| POWER_ON | 25 | Modem power rail |
| RST | 5 | Modem reset |
| BAT_ADC | 35 | Battery voltage ADC |
| LED | 12 | Status LED |

---

## Dependencies

Install via Arduino IDE Library Manager:

| Library | Version | Purpose |
|---|---|---|
| `TinyGSM` | latest | Modem AT command interface |
| `ArduinoHttpClient` | latest | HTTP requests to Traccar |

> **Note:** Use the standard TinyGSM with `#define TINY_GSM_MODEM_SIM7600` —
> the A7670E is AT-command compatible with SIM7600.

---

## Setup

### 1. Arduino IDE Configuration

- Board: `ESP32 Wrover Module`
- Upload speed: `921600`
- Flash size: `16MB`
- Partition scheme: `Huge APP`
- Port: `/dev/ttyACM0` (Linux) or `COM3` (Windows)

### 2. Configuration

Edit these values at the top of `hati_tracker.ino`:

```cpp
// Traccar server — change to your own server
const char TRACCAR_HOST[] = "demo.traccar.org";
const int  TRACCAR_PORT   = 5055;
const char DEVICE_ID[]    = "hati-tracker-001";

// Carrier APN — change if not Telia Finland
const char APN[] = "internet";
```

### 3. Traccar Setup

1. Register at [demo.traccar.org](https://demo.traccar.org) (for testing)
   or set up your own server (see `hati-tracker-backend`)
2. Add a new device with identifier: `hati-tracker-001`
3. Protocol: **OsmAnd** (port 5055)

---

## How It Works

```
Boot
 └─ Power on A7670E modem (1200ms PWRKEY pulse + 8s wait)
 └─ Sync AT communication
 └─ Connect to Telia 4G (APN: internet)
 └─ Enable GPS

Loop every 10s (moving) / 60s (still):
 └─ Try GPS fix (up to 2 min timeout)
     ├─ Got fix → send lat/lon/speed/alt/battery/timestamp to Traccar
     └─ No fix  → try LBS cell tower fallback → send approximate position

Watchdog: auto-restart if board hangs > 2 minutes
```

---

## LED Status

| Pattern | Meaning |
|---|---|
| Solid ON | Booting |
| 1 blink | Position sent successfully |
| 2 blinks | Waiting for network |
| 5 blinks | Send failed |

---

## Battery Life Estimates

| Update rate | Battery (2500mAh 18650) |
|---|---|
| Every 10s (active run) | ~6–8 hours |
| Every 60s (idle/still) | ~24–36 hours |
| Mixed use typical day | ~12–16 hours |

---

## Related Repositories

- [`hati-tracker-backend`](https://github.com/yourusername/hati-tracker-backend) — Traccar server on Docker
- [`hati-tracker-dashboard`](https://github.com/yourusername/hati-tracker-dashboard) — React/TypeScript canicross stats dashboard

