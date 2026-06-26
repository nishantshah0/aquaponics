# AquaControl 🐟🌱

A live monitoring dashboard for a 40‑fish / 40‑plant aquaponics system
(ENG SCI 1050, Western University). It runs entirely in the browser as a single
`index.html` — **no backend, no build step** — and connects to **real hardware**
two different ways, with a physics‑flavoured simulation as the fallback.

**Live demo:** open `index.html`, or host it on GitHub Pages.

---

## What it does

- **Live sensor cards** — pH, water temperature, water level, and TAN (ammonia),
  each with an in‑range/out‑of‑range state and a rolling **sparkline** of recent history.
- **System health score** + **engineering‑constraints pass/fail table** that update
  in real time from the current readings.
- **Design calculators** — fish‑growth model `W = 5·e^(0.015·t)`, daily water‑makeup
  mass balance, and nitrogen / bio‑filter sizing.
- **CSV export** of everything the dashboard has logged this session.
- **Three data sources**, chosen at runtime:

| Source | How | When to use |
|--------|-----|-------------|
| **USB (Web Serial)** | ESP32/Arduino plugged in over USB | Bench demo, judging table |
| **Wi‑Fi (MQTT)** | ESP32 publishes to an MQTT broker | Wireless / remote tank |
| **Simulation** | Built‑in, runs when nothing is connected | No hardware on hand |

---

## Connect real hardware

### Option A — USB (Web Serial)
1. Flash the firmware (below) to an ESP32 and plug it into the laptop.
2. Open the dashboard in **Chrome or Edge** (Web Serial requires it), served over
   `https://` or `localhost`.
3. Click **Connect via USB**, pick the serial port, done — cards go live and the
   badge switches to `USB · ESP32`.

### Option B — Wi‑Fi (MQTT)
1. In the firmware set `USE_WIFI 1`, fill in your Wi‑Fi + broker, flash.
2. In the dashboard click **Connect via Wi‑Fi (MQTT)**, confirm the broker URL and
   topic (defaults to the public HiveMQ broker), click **Subscribe**.

---

## Telemetry protocol

The device streams **newline‑delimited JSON**, one reading per line, at **115200 baud**:

```json
{"ph":7.21,"temp":24.1,"level":1048,"tan":8.4}
```

- Every field is optional — send any subset; missing fields keep their last value,
  so sensors can report at different rates.
- A bare CSV line `ph,temp,level,tan` (e.g. `7.21,24.1,1048,8.4`) is also accepted.

| Field   | Unit     | Meaning                                  |
|---------|----------|------------------------------------------|
| `ph`    | —        | pH of the tank water                     |
| `temp`  | °C       | Water temperature                        |
| `level` | L        | Water volume in the tank                 |
| `tan`   | g/day    | Total ammonia nitrogen load              |

---

## Firmware

Sketch: [`firmware/aquacontrol_esp32/aquacontrol_esp32.ino`](firmware/aquacontrol_esp32/aquacontrol_esp32.ino)

**Board:** ESP32 Dev Module (Espressif esp32 core).
**Libraries:** `OneWire`, `DallasTemperature`, and `PubSubClient` (only if `USE_WIFI 1`).

### Default wiring

| Sensor | Part (example) | ESP32 pin |
|--------|----------------|-----------|
| pH | analog pH board, e.g. DFRobot SEN0161 | `GPIO34` (ADC1) |
| Temperature | DS18B20 waterproof probe (4.7 kΩ pull‑up) | `GPIO4` |
| Water level | HC‑SR04 ultrasonic | `TRIG GPIO5`, `ECHO GPIO18` |
| TAN (optional probe) | ion‑selective ammonia electrode | `GPIO35` (ADC1) |

> **Calibrate before trusting readings.** Set `PH_V_AT_7` / `PH_V_AT_4` from pH 7.0
> and 4.0 buffer solutions, and `SENSOR_GAP_CM` from your sensor‑to‑water distance.
> TAN has no cheap inline probe, so by default it's **estimated** from feed mass
> (`TAN = feed_g/day · protein% · 0.092`, per Timmons & Ebeling). Set
> `TAN_FROM_PROBE 1` if you have a real ammonia electrode.

### Upload
1. Arduino IDE → install the **esp32** boards package and the libraries above.
2. Select **ESP32 Dev Module** and the right COM port.
3. Upload. Open Serial Monitor at **115200** — you should see JSON lines every 2 s.

---

## Run without hardware
Just open `index.html`. It starts in **Simulation** mode (random‑walk physics within
realistic bounds) so every feature — alerts, health score, constraints, sparklines,
CSV export — is demoable on a laptop with nothing plugged in. The on‑screen
simulation controls let you force alert conditions for a walkthrough.

---

## Tech notes
- Pure HTML/CSS/JS, single file, zero dependencies (MQTT.js is lazy‑loaded from a CDN
  only if you use the Wi‑Fi path).
- `applyReading({ph,temp,level,tan})` is the single entry point all three sources feed
  into, so adding a fourth transport is a few lines.
- Web Serial is a Chromium‑only API and needs a secure context (`https`/`localhost`).
