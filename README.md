# 🐾 Hati Tracker Firmware

Firmware for a DIY 4G GNSS tracker built on the **LilyGO T-A7670E / SIMCom A7670E** board.

This tracker was built for live outdoor GNSS tracking over 4G. It sends position data to a private [Traccar](https://traccar.org) server using the OsmAnd HTTP protocol, so it can run without relying on the public Traccar demo server.

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

## Traccar Server Setup on a VPS

This firmware sends positions using Traccar's OsmAnd HTTP protocol. The clean setup is to run your own Traccar instance on a small VPS instead of using the public Traccar demo server.

The examples below assume an Ubuntu VPS and Docker Compose.

### 1. Point a stable address to the VPS

Use either:

- a domain name, for example `tracker.example.com`
- a static VPS IP address
- a cloud provider Elastic/Reserved IP

Avoid hard-coding a temporary public IP in the firmware for long-term use, because it can change after a VPS reboot, stop/start, or rebuild.

In `secrets.h`, the tracker will later use this address:

```cpp
const char TRACCAR_HOST[] = "tracker.example.com";
const int  TRACCAR_PORT   = 5055;
const char DEVICE_ID[]    = "example-device-id";
```

### 2. Open the required firewall ports

You need these ports:

| Port | Purpose | Recommended exposure |
|---:|---|---|
| `22/tcp` | SSH administration | Only your own IP |
| `8082/tcp` | Traccar web UI/API | Only your own IP, or behind HTTPS reverse proxy |
| `5055/tcp` | OsmAnd tracker input | Public, unless your tracker has a fixed source IP |

For early testing, `8082` can be opened temporarily so you can access the web UI. After testing, restrict it or put it behind a reverse proxy with HTTPS.

If using `ufw` on the VPS:

```bash
sudo ufw allow from YOUR_PUBLIC_IP to any port 22 proto tcp
sudo ufw allow from YOUR_PUBLIC_IP to any port 8082 proto tcp
sudo ufw allow 5055/tcp
sudo ufw enable
sudo ufw status verbose
```

Replace `YOUR_PUBLIC_IP` with your own home/office IP. Do not leave SSH open to the whole internet.

### 3. Install Docker

SSH into the VPS:

```bash
ssh ubuntu@tracker.example.com
```

Install Docker and the Compose plugin:

```bash
sudo apt update
sudo apt install -y ca-certificates curl
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker "$USER"
newgrp docker
docker --version
docker compose version
```

### 4. Create the Traccar folders

```bash
sudo mkdir -p /opt/traccar/{conf,data,logs}
sudo chown -R "$USER":"$USER" /opt/traccar
cd /opt/traccar
```

### 5. Create `traccar.xml`

For a simple personal setup or first test, the built-in H2 database is enough:

```bash
cat > /opt/traccar/conf/traccar.xml << 'EOF'
<?xml version='1.0' encoding='UTF-8'?>
<!DOCTYPE properties SYSTEM 'http://java.sun.com/dtd/properties.dtd'>
<properties>
    <entry key='config.default'>./conf/default.xml</entry>

    <entry key='database.driver'>org.h2.Driver</entry>
    <entry key='database.url'>jdbc:h2:./data/database</entry>
    <entry key='database.user'>sa</entry>
    <entry key='database.password'></entry>
</properties>
EOF
```

For heavy long-term use, consider PostgreSQL or MySQL instead of H2.

### 6. Create `docker-compose.yml`

```bash
cat > /opt/traccar/docker-compose.yml << 'EOF'
services:
  traccar:
    image: traccar/traccar:latest
    container_name: traccar
    restart: unless-stopped
    ports:
      - "8082:8082"
      - "5055:5055"
    volumes:
      - ./conf/traccar.xml:/opt/traccar/conf/traccar.xml:ro
      - ./data:/opt/traccar/data
      - ./logs:/opt/traccar/logs
EOF
```

Make sure the container can write to the data and log folders:

```bash
sudo chown -R 1000:1000 /opt/traccar/data /opt/traccar/logs
```

### 7. Start Traccar

```bash
cd /opt/traccar
docker compose pull
docker compose up -d
docker compose ps
docker compose logs --tail=50
```

Check that Traccar responds locally:

```bash
curl -I http://localhost:8082
```

A working server should return an HTTP response such as `200 OK`.

### 8. Open the web UI

Open this in a browser:

```text
http://tracker.example.com:8082
```

or:

```text
http://YOUR_VPS_IP:8082
```

Create the first user account. Use a strong password because this is your own tracking server.

### 9. Add the tracker device

In the Traccar web UI:

1. Open **Devices**.
2. Add a new device.
3. Set the name to anything you like.
4. Set the **Identifier** to exactly the same value as `DEVICE_ID` in `secrets.h`.

Example:

```cpp
const char DEVICE_ID[] = "example-device-id";
```

The identifier must match exactly. If it does not, Traccar can receive the HTTP request but will not attach the position to your device.

### 10. Test the VPS receiver before flashing the board

From inside the VPS:

```bash
curl "http://localhost:5055/?id=example-device-id&lat=60.1699&lon=24.9384&timestamp=2026-01-01T12:00:00Z&speed=0&altitude=0&accuracy=10&batt=90&valid=true"
```

From your own computer:

```bash
curl "http://tracker.example.com:5055/?id=example-device-id&lat=60.1699&lon=24.9384&timestamp=2026-01-01T12:00:00Z&speed=0&altitude=0&accuracy=10&batt=90&valid=true"
```

Then check the Traccar UI. The device should show one test position.

This test position is not a live fake GPS stream. It is only one stored test point. When the real tracker sends a valid GNSS fix, the latest position will update.

### 11. Configure the firmware

Create `secrets.h` next to the `.ino` file:

```cpp
#pragma once

const char APN[]       = "internet";
const char APN_USER[]  = "";
const char APN_PASS[]  = "";

const char TRACCAR_HOST[] = "tracker.example.com";
const int  TRACCAR_PORT   = 5055;
const char DEVICE_ID[]    = "example-device-id";
```

Then build and upload the firmware to the LilyGO board.

### 12. Watch logs during the first real test

On the VPS:

```bash
cd /opt/traccar
docker compose logs -f
```

In another SSH session, you can also watch for incoming packets:

```bash
sudo tcpdump -i any 'tcp port 5055'
```

Turn the tracker on outside or near a window. GNSS may not get a fix indoors.

### Troubleshooting

| Symptom | Likely cause | What to check |
|---|---|---|
| Traccar web UI does not open | Port `8082` blocked or container not running | `docker compose ps`, VPS firewall, cloud firewall/security group |
| Test `curl` to `localhost:5055` works but public `curl` does not | VPS/cloud firewall blocks `5055` | Open `5055/tcp` to the internet |
| VPS sees no packets from the board | Firmware still points to the wrong host, mobile data failed, or APN is wrong | Serial logs, `TRACCAR_HOST`, APN, SIM data plan |
| Packets arrive but no device updates | Wrong Traccar device identifier | Match Traccar Identifier with `DEVICE_ID` |
| Device stays at old test point | Real tracker has not sent a newer valid fix yet | Wait for GNSS fix and check serial logs |
| Web UI is exposed to everyone | Firewall too open | Restrict `8082` to your IP or use a reverse proxy with HTTPS |

### Basic maintenance

Update Traccar:

```bash
cd /opt/traccar
docker compose pull
docker compose up -d
docker image prune -f
```

Back up the simple H2 database:

```bash
mkdir -p ~/traccar-backups
docker compose stop
cp -a /opt/traccar/data ~/traccar-backups/data-$(date +%F-%H%M)
docker compose start
```

For a public-facing setup, also consider:

- HTTPS with Caddy, Nginx, or another reverse proxy
- automatic VPS security updates
- regular backups
- fail2ban or equivalent SSH protection
- a production database if tracking many devices or storing lots of history

The public Traccar demo server should only be used for quick firmware testing, not real tracking.

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
