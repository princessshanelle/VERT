# 🌱 V.E.R.T. — Versatile Eco-farming Robotic Technology

An automated smart-farming system for monitoring and managing crop-growing
conditions. V.E.R.T. combines soil/climate sensing, automated irrigation, a
motorized access door, and camera-based AI plant-health detection, all
exposed through a single web dashboard.

## 🚧 Project Status

Under active development / hardware bring-up. Core sensor + irrigation +
door control is working on real hardware. The camera/AI module streams and
classifies but is still being debugged for a heap-corruption crash on some
board package versions.
---

## 🧠 How It's Built

V.E.R.T. is **two independent ESP32 boards on the same WiFi network**, not
one monolithic device:

```
                     Farmer's browser
                            |
              (loads dashboard from vert.local)
                            |
        +-------------------+-------------------+
        |                                        |
   vert.local                             vert-cam.local
  (Doit ESP32 DevKit V1)                (Seeed XIAO ESP32S3 Sense)
   "VERT_ESP32"                          "XIAO_ESP32S3_CAM"
        |                                        |
  sensors, pump,                          camera + on-device
  door motor, grow light                  Edge Impulse AI model
```

The two boards **never talk to each other directly**. The dashboard's
browser fetches from both independently:
- `vert.local` — sensors, irrigation, door, grow light, dashboard UI (serves
  the whole website over LittleFS)
- `vert-cam.local` — live MJPEG video stream + AI plant-health predictions

This keeps real-time sensor/motor timing on the main board completely
isolated from anything camera- or AI-related, so a slow/crashing camera
board can never affect irrigation, the door, or the grow light.

---

## 📁 Repository Structure

```
VERT/
├── VERT_ESP32/              # Main controller firmware (Doit ESP32 DevKit V1)
│   ├── VERT_ESP32.ino        # Sensors, irrigation, door, grow light, web server
│   └── data/                 # Dashboard website (flashed to LittleFS)
│       ├── index.html        #   Dashboard tab: sensors, controls, preferences
│       ├── camera.html       #   Camera & AI tab: live stream + health chart
│       ├── script.js
│       ├── camera.js
│       └── style.css
└── XIAO_ESP32S3_CAM/        # Camera + AI firmware (Seeed XIAO ESP32S3 Sense)
    └── XIAO_ESP32S3_CAM.ino  # MJPEG stream, snapshot, Edge Impulse classifier
```

---

## ⚙️ Features

- 🌡️ **Climate monitoring** — DHT22 temperature + humidity
- 💧 **Soil moisture monitoring** — 5x capacitive sensors, averaged
- 🧪 **pH monitoring** — 2x pH sensor modules
- 🚿 **Automated irrigation** — pump switches on/off around farmer-set soil
  thresholds, or run manually
- 🚪 **Motorized access door** — open/close on command, with limit-switch
  homing and a stall/jam timeout
- 💡 **Grow light control** — relay-driven bulb, on/off from the dashboard
- 📷 **Live camera feed** — MJPEG stream embedded in the dashboard, with a
  start/stop toggle so it isn't pulling bandwidth when nobody's watching
- 🤖 **On-device plant health AI** — Edge Impulse image classifier
  (MobileNetV2) runs directly on the camera board, no cloud involved
- 📊 **Plant health history chart** — confidence-over-time graph, logged
  client-side in the browser from the AI's predictions
- 🖥️ **Web dashboard** — two-tab responsive UI served entirely from the main
  board's flash (LittleFS), no app or cloud account required

---

## 🔩 Hardware

### Main controller (`VERT_ESP32`)
| Component | Notes |
|---|---|
| ESP32 DevKit V1 (Doit, 30-pin) | main board |
| 5x soil moisture sensors | analog, capacitive |
| 2x pH sensor modules | analog |
| 1x DHT22 | temperature + humidity |
| 2x limit switches | door OPEN / door CLOSED |
| 1x L298N driver | Channel A drives the 12V door motor |
| 1x relay module | switches the 12V DC irrigation pump |
| 1x relay module | switches the grow light bulb |
| 12V/5A adapter | powers L298N motor + pump |
| HW-131 breadboard PSU | powers the ESP32 5V/3V3 rail |

### Camera + AI module (`XIAO_ESP32S3_CAM`)
| Component | Notes |
|---|---|
| Seeed XIAO ESP32S3 **Sense** | must be the Sense variant — it's the one with PSRAM + camera connector |
| OV2640 camera (Sense expansion board) | ships attached to the Sense variant |

---

## 🌐 API Reference

### Main board — `http://vert.local` (port 80)
| Endpoint | Method | Description |
|---|---|---|
| `/data` | GET | Full sensor + state snapshot (soil, pH, temp, humidity, door, pump, bulb, thresholds) |
| `/setThresholds` | POST | Save farmer-configured min/max thresholds |
| `/pump?state=on\|off` | GET | Manual pump control (only when auto-irrigation is off) |
| `/bulb?state=on\|off` | GET | Grow light on/off |
| `/mode?auto=1\|0` | GET | Toggle automatic irrigation |
| `/door?action=open\|close` | GET | Move the door |

### Camera board — `http://vert-cam.local` (port 80 + 81)
| Endpoint | Method | Description |
|---|---|---|
| `:81/stream` | GET | MJPEG live video (`multipart/x-mixed-replace`) |
| `/capture` | GET | Single JPEG snapshot |
| `/status` | GET | Latest AI reading: `{"ok":true,"detected":true,"label":"healthy","confidence":0.93,"ageMs":1200}` |

`detected` is `false` whenever the top label's confidence is below the
classifier's confidence threshold — i.e. nothing was confidently recognized
in frame (empty view, blurry, plant out of shot). The dashboard skips
charting a reading when `detected` is false rather than plotting a
low-confidence guess.

---

## 🚀 Getting Started

### 1. Main controller (`VERT_ESP32`)
1. Install libraries: **DHT sensor library** by Adafruit (Library Manager).
   Everything else (WiFi, WebServer, LittleFS, Preferences, ESPmDNS) ships
   with the ESP32 board package.
2. Set `WIFI_SSID` / `WIFI_PASSWORD` at the top of `VERT_ESP32.ino`.
3. Flash the sketch (Sketch > Upload).
4. Upload the dashboard website: **Tools > ESP32 Sketch Data Upload** (pushes
   `data/` into LittleFS). Order relative to flashing the sketch doesn't
   matter, but this must be done at least once.
5. Calibrate the soil and pH sensors — see the `CALIBRATION` constants near
   the top of `VERT_ESP32.ino` (dry-air/water-cup readings for soil, pH 4.0/7.0
   buffer readings for pH).
6. Visit `http://vert.local` (or the IP address printed on Serial boot).

### 2. Camera + AI module (`XIAO_ESP32S3_CAM`)
1. Board package: add
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   under File > Preferences > "Additional Board Manager URLs", then install
   **"esp32" by Espressif Systems** (Boards Manager). Select
   **Tools > Board > XIAO_ESP32S3**, and set **Tools > PSRAM > OPI PSRAM**
   (required — the camera won't init without it).
2. Train a model on [Edge Impulse](https://studio.edgeimpulse.com): image
   classification (96x96 or 160x160) → Transfer Learning (MobileNetV2 96x96
   0.1). Export as an **Arduino library** and add it via Sketch > Include
   Library > Add .ZIP Library.
3. Set `WIFI_SSID` / `WIFI_PASSWORD` (same network as the main board) at the
   top of `XIAO_ESP32S3_CAM.ino`.
4. Flash the sketch and check Serial Monitor for `Reachable at
   http://vert-cam.local`.

### 3. Using the dashboard
Open `http://vert.local` in a browser on the same network. The **Dashboard**
tab shows live sensor readings and controls; the **Camera & AI** tab shows
the live stream (toggle on/off) and a plant-health-over-time chart. If your
browser can't resolve `.local` hostnames (common on Windows without
Bonjour installed), use the **Camera address** field on the Camera & AI page
to enter the camera board's IP address directly instead.

## 🗺️ Roadmap

- [ ] Resolve the camera board heap-corruption crash
- [ ] Persist plant-health history server-side instead of per-browser
      `localStorage`
- [ ] Automatic door scheduling
- [ ] Multi-camera support
