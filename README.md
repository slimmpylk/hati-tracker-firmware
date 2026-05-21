# 🐾 Hati Tracker Firmware

Firmware for a DIY 4G GNSS tracker built on the **LilyGO T-A7670E / SIMCom A7670E** board.

The tracker was built for following Hati, an Australian Cattle Dog, during canicross runs. It sends live position data to a private [Traccar](https://traccar.org) server using the OsmAnd HTTP protocol over 4G.

---

## Current Status

Working end-to-end:

- Telia Finland 4G connection
- A7670E GNSS power-up and ready detection
- AGPS assistance download with `AT+CAGPS`
- Manual `AT+CGNSSINFO` parsing for latitude, longitude, speed, altitude, time and satellite data
- Traccar OsmAnd HTTP upload
- Adaptive update rate based on speed
- Watchdog protection
- Two-LED status indication

Important implementation note: this firmware does **not** use `TinyGSM::getGPS()`. On this A7670E firmware, TinyGSM's SIM7600 GPS parser returned invalid coordinates, while raw `AT+CGNSSINFO` returned valid fixes. The firmware therefore parses `AT+CGNSSINFO` directly.

---

## Features

- **Live GNSS tracking** — latitude, longitude, speed, altitude, timestamp, satellite count and estimated accuracy
- **4G LTE upload** — tested with Telia Finland APN `internet`
- **Private Traccar support** — server details are stored in `secrets.h`, not committed to Git
- **AGPS support** — uses `AT+CAGPS`, which significantly improves first-fix time on the tested A7670E
- **Adaptive send interval**
  - Walking / slow movement: every 5 seconds
  - Running / fast movement: every 1 second
- **OsmAnd protocol upload** — sends positions to Traccar port `5055`
- **Two LED status indicators**
  - RED1 solid = 4G connected
  - RED2 solid = GNSS fix active
  - Both solid = tracker is fully working
- **Watchdog timer** — restarts the board if the firmware hangs
- **Battery placeholder** — battery ADC is disabled by default until the correct board ADC calibration is verified

---

## Hardware

| Component | Details |
|---|---|
| Board | LilyGO T-A7670E R2 |
| Modem | SIMCom A7670E / A7670E-FASE |
| MCU | ESP32 |
| Cellular | 4G LTE Cat-1 |
| SIM | Nano SIM, tested with Telia Finland |
| GNSS antenna | External L1/L5 antenna; A7670E appears to use L1 |
| Battery | 18650 Li-ion in onboard holder, not yet ADC-calibrated |
| Use case | Canicross / dog tracking |

---

## Pin Definitions

| Pin | GPIO | Function |
|---|---:|---|
| MODEM_TX | 26 | ESP32 TX → modem RX |
| MODEM_RX | 27 | ESP32 RX ← modem TX |
| MODEM_PWRKEY | 4 | Modem power key |
| MODEM_POWER_ON | 25 | Modem power rail |
| MODEM_RST | 5 | Modem reset |
| BAT_ADC | 35 | Battery ADC, disabled by default |
| LED_RED1 | 12 | 4G status |
| LED_RED2 | 2 | GNSS status |

---

## Dependencies

Install these with the Arduino IDE Library Manager:

| Library | Purpose |
|---|---|
| `TinyGSM` | Modem/network interface |
| `ArduinoHttpClient` | HTTP upload to Traccar |

The code currently uses:

```cpp
#define TINY_GSM_MODEM_SIM7600
```

This works for the cellular side of the tested A7670E. GNSS is handled manually through AT commands because TinyGSM GPS parsing was not compatible with the tested modem firmware.

---

## Arduino IDE Setup

Recommended settings used during testing:

| Setting | Value |
|---|---|
| Board | ESP32 Wrover Module |
| Upload speed | 921600 |
| Flash size | 16MB |
| Partition scheme | Huge APP |
| Serial baud | 115200 |
| Linux port example | `/dev/ttyACM0` |

For serial logging on Linux:

```bash
picocom -b 115200 /dev/ttyACM0 | tee -a gps_log.txt
```

Exit picocom with:

```text
Ctrl+A
Ctrl+X
```

---

## Private Configuration

Create a file named:

```text
secrets.h
```

Place it in the same folder as the `.ino` file.

Example real `secrets.h`:

```cpp
#pragma once

const char APN[]       = "internet";
const char APN_USER[]  = "";
const char APN_PASS[]  = "";

const char TRACCAR_HOST[] = "your.traccar.server.com";
const int  TRACCAR_PORT   = 5055;
const char DEVICE_ID[]    = "hati-tracker-001";
```

Do **not** commit `secrets.h`.

Commit this as `secrets.example.h` instead:

```cpp
#pragma once

const char APN[]       = "internet";
const char APN_USER[]  = "";
const char APN_PASS[]  = "";

const char TRACCAR_HOST[] = "example.traccar.server.com";
const int  TRACCAR_PORT   = 5055;
const char DEVICE_ID[]    = "example-device-id";
```

Recommended `.gitignore`:

```gitignore
secrets.h
*.log
gps_log*.txt
```

Do not commit serial logs containing modem identifiers such as IMEI.

---

## Traccar Setup

This firmware sends positions using Traccar's OsmAnd HTTP protocol.

Minimum server setup:

1. Run your own Traccar server, preferably on a private VPS.
2. Expose Traccar web UI/API port, usually `8082`.
3. Expose OsmAnd receiver port `5055`.
4. Create a device in Traccar.
5. Set the device unique ID to match `DEVICE_ID` in `secrets.h`.
6. Set `TRACCAR_HOST` in `secrets.h` to your VPS IP address or domain.
7. Upload the firmware and verify `Traccar HTTP 200` in serial logs.

The public Traccar demo server should only be used for quick testing, not real tracking.

---

## How It Works

```text
Boot
 ├─ Start watchdog
 ├─ Power on A7670E modem
 ├─ Sync AT communication
 ├─ Connect to cellular network
 ├─ Connect APN / GPRS data
 ├─ Power on GNSS with AT+CGNSSPWR=1
 ├─ Wait for +CGNSSPWR: READY!
 ├─ Download AGPS assistance with AT+CAGPS
 └─ Start tracking loop

Tracking loop
 ├─ Poll AT+CGNSSINFO
 ├─ Parse valid GNSS fix
 ├─ Choose send interval based on speed
 ├─ Send position to Traccar
 └─ Keep LEDs updated
```

---

## LED Status

| LED | State | Meaning |
|---|---|---|
| RED1 | Blinking | Booting or connecting 4G |
| RED1 | Solid ON | 4G data connected |
| RED2 | OFF | GNSS search not started |
| RED2 | Blinking | Searching for GNSS fix |
| RED2 | Solid ON | Valid GNSS fix active |
| RED1 + RED2 | Both solid ON | Tracker is fully working |

---

## Data Sent to Traccar

Each valid fix sends:

- Device ID
- Latitude
- Longitude
- Speed
- Altitude
- Battery percentage
- Satellite count
- Accuracy estimate
- Valid flag
- GNSS UTC timestamp

Speed from `AT+CGNSSINFO` appears to be km/h in the train test. The firmware converts it to knots before sending because Traccar's OsmAnd protocol commonly interprets speed as knots.

---

## Notes and Limitations

- `AT+CGNSSMODE=11` returned `ERROR` on the tested firmware, so the code does not force a constellation mode.
- `AT+AGPS` returned `ERROR`; `AT+CAGPS` worked and returned `+AGPS: success`.
- Battery ADC is disabled by default because USB-only testing showed `0.00V`. Calibrate the ADC with a battery and multimeter before enabling it.
- GNSS altitude can be noisy. Dashboard/backend analytics should smooth elevation before calculating elevation gain.
- LBS fallback is intentionally not included in the clean firmware because inaccurate cell-tower points can damage sports-session statistics.

---

## Project Structure

```text
hati_tracker/
├── hati_tracker.ino
├── secrets.h              # private, not committed
├── secrets.example.h      # committed template
├── README.md
└── .gitignore
```

---

## Related Repositories

- `hati-tracker-backend` — private Traccar server on Docker
- `hati-tracker-dashboard` — future React/TypeScript canicross stats dashboard

---

## License

MIT, unless changed later.
