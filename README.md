# 3D Printer IoT Safety System
<img width="1852" height="967" alt="Image" src="https://github.com/user-attachments/assets/343c64a8-7450-4011-9045-496f9dc58979" />

An **ESP32 DevKitC V4**-based air-quality monitoring node for a home 3D-printer setup (tested with **Bambu Lab P1SC**).  
It monitors **temperature/humidity, TVOC (scaled VOC Index), formaldehyde (HCHO), CO₂, and particulate matter (PM1.0/PM2.5/PM10)** and uploads the data to **ThingSpeak** for dashboards and mobile notifications.

> ⚠️ **Safety note**  
> This project helps you *observe* air quality and take action (ventilation / filtration).  
> It is **not** a certified safety device and does **not** replace smoke detectors, fire alarms, or proper ventilation.

---

## Features

- **Sensors (1 Hz sampling; SHT41 temperature/humidity compensates the SGP41)**
  - Temp & RH: **SHT41 (SHT4x)**
  - TVOC proxy: **SGP41 VOC Index** *(scaled ÷5 to fit 0–100 range)*
  - Formaldehyde: **ZE08-CH2O** (UART)
  - CO₂: **MH-Z16** (PWM output)
  - Dust / PM: **PMS7003** (UART, **passive mode** request)
- **Data quality**
  - A reading is used only while it is fresh; an unplugged or failing sensor drops out of the upload instead of repeating its last value
  - Wi-Fi reconnects in the background, so sampling continues while the network is down (each HTTPS upload still blocks the loop briefly)
- **Cloud**
  - Upload to **ThingSpeak** over **HTTPS** every **20 s** (free accounts allow one update per channel every 15 s)
- **Mobile / Automation**
  - Works well with **ThingsView / ThingSpeak dashboards**
  - Easy to read latest values from `.../fields/<n>/last.json` in **iOS Shortcuts**, Home automation, etc.
- **Hardware enclosure**
  - Includes case files: **.3mf / .f3d / .step** (printed in **PLA-HS**)

---

## Data mapping (ThingSpeak fields)
<table>
  <tr>
    <td width="33.33%">
      <img src="https://github.com/user-attachments/assets/8bf51ecf-3078-443a-881e-afd320313b84" alt="Image" width="100%">
    </td>
    <td width="33.33%">
      <img src="https://github.com/user-attachments/assets/d6a1eb07-cf87-4e16-a80a-c1d1ec46526b" alt="Image" width="100%">
    </td>
    <td width="33.33%">
      <img src="https://github.com/user-attachments/assets/13ab236a-af40-458e-b194-3f15f0d194a6" alt="Image" width="100%">
    </td>
  </tr>
</table>



| Field | Metric | Unit / notes |
|---:|---|---|
| field1 | Temperature | °C |
| field2 | Humidity | %RH |
| field3 | TVOC | **VOC Index ÷ 5** (scaled, 0–100 target) |
| field4 | HCHO | ppb (from sensor) |
| field5 | CO₂ | ppm (PWM, full-scale configurable) |
| field6 | PM1.0 | µg/m³ |
| field7 | PM2.5 | µg/m³ |
| field8 | PM10 | µg/m³ |

- A field without a fresh, valid reading (sensor warming up, unplugged or failing) is **left out** of the update, so ThingSpeak stores an empty value instead of a placeholder number.
- field3 appears about **1 minute after boot**: 10 s of SGP41 conditioning plus the 45 s start-up blackout of the VOC algorithm.
- Channel history: in v1 (Arduino Mega, 2025-01) field3 was SGP30 TVOC in ppb and field4 was CO in ppm. If the same channel is reused, those two fields are not comparable across the firmware change.

---

## Wiring (ESP32 DevKitC V4)

### I2C bus (shared)
- **SDA = GPIO19**
- **SCL = GPIO22**
- Power: **3.3 V**

### UART / PWM
- PMS7003: UART1 (RX=GPIO4, TX=GPIO5)
- ZE08-CH2O: UART2 (RX=GPIO16, TX=GPIO17)
- MH-Z16: PWM on GPIO33

> **Voltage-level caution**  
> Many PMS7003 / ZE08 modules use **3.3 V UART logic** even when powered by 5 V, but variants exist.  
> If your module outputs **5 V TTL**, use a **level shifter** or a resistor divider into ESP32 RX pins.  
> Check the **PWM output level of your MH-Z16** module the same way before wiring it to GPIO33.

> **Module-variant caution**  
> DevKitC V4 boards ship with either a WROOM or a **WROVER** module. On WROVER (PSRAM) modules, **GPIO16/17 are used by the PSRAM** and cannot serve as UART2 — use a WROOM-based board or move the ZE08 UART to free pins.

| Device | Power | ESP32 Pins |
|---|---|---|
| **SHT41** | 3.3V, GND | SDA=19, SCL=22 |
| **SGP41** | 3.3V, GND | SDA=19, SCL=22 |
| **PMS7003** | 5V, GND | TX→GPIO4, RX→GPIO5 |
| **MH-Z16** | 5V, GND | PWM→GPIO33 |
| **ZE08-CH2O** | 5V, GND | TX→GPIO16, RX→GPIO17 |

---

## Power stability (recommended)

When Wi‑Fi transmits, the ESP32 current spikes can trigger **brownouts** if the 5 V rail droops (especially with multiple sensors).

**Recommended capacitors**
- Near **ESP32 5V/GND**: **470–1000 µF** electrolytic (plus 0.1 µF ceramic if you have space)
- Near **PMS7003 5V/GND**: **100–220 µF** electrolytic

**How to place them (on perfboard)**
- Electrolytic has polarity:
  - **+** leg → **5V**
  - **–** leg → **GND**
- Put the capacitor physically **close to the module’s 5V/GND pins** (short leads/wires).

---

## Firmware

### Files
- `firmware/v2/AirMonitoringSystem/AirMonitoringSystem.ino` — sketch
- `firmware/v2/AirMonitoringSystem/secrets.example.h` — credential template (copy to `secrets.h`)
- `firmware/v2/AirMonitoringSystem/thingspeak_ca.h` — root CA used to verify `api.thingspeak.com`

### Credentials (required)
Wi-Fi and ThingSpeak credentials are **not** stored in this repository.

1. Copy `secrets.example.h` to `secrets.h` in the same folder.
2. Fill in `WIFI_SSID`, `WIFI_PASSWORD` and `THINGSPEAK_WRITE_API_KEY`.
3. `secrets.h` is ignored by Git (`.gitignore`) — never commit it. The build stops with an error if it is missing.

Uploads go to `https://api.thingspeak.com/update` as an HTTPS `POST`. The Write API Key travels in the request body (not in the URL), and the server certificate is verified against the root CA in `thingspeak_ca.h` (DigiCert Global Root G2).
If the serial log shows `ThingSpeak HTTPS failed`, ThingSpeak may have changed its certificate chain: export the root certificate of `api.thingspeak.com` from a browser as PEM and replace it in `thingspeak_ca.h`.

> If credentials were ever committed or shared, treat them as leaked: regenerate the ThingSpeak Write API Key (*Channel → API Keys → Generate New Write API Key*) and change the Wi-Fi password.

### Configs you may change
- **CO₂ full-scale** (your setting):
  ```cpp
  static const int CO2_FULL_SCALE_PPM = 5000;
  ```
- **Upload interval** (default 20 seconds, keep ≥ 15 s on a free ThingSpeak account):
  ```cpp
  static const uint32_t UPLOAD_INTERVAL_MS = 20000;
  ```
- **Freshness limits** (older readings are treated as missing):
  ```cpp
  static const uint32_t SHT_STALE_MS  = 5000;
  static const uint32_t VOC_STALE_MS  = 5000;
  static const uint32_t HCHO_STALE_MS = 10000;
  static const uint32_t PMS_STALE_MS  = 20000;
  static const uint32_t CO2_STALE_US  = 3000000;
  ```

### TVOC scaling (VOC Index ÷ 5)
This project converts Sensirion’s **VOC Index** to a 0–100-like scale, and only once the index is valid (no `0` is sent during warm-up):
- Upload:
  ```cpp
  if (vocValid()) appendField(body, 3, String(lastVocIndex / 5));
  ```
- Serial log: `-` is printed for any value that is not currently valid.

**Interpretation suggestion (scaled TVOC = VOC Index ÷ 5)**
- 0–20: clean / baseline
- 20–40: noticeable VOCs (watch trend)
- 40–60: high → **consider ventilation**
- 60–100: very high → **ventilate now** (and consider filtration)

> Tip: VOC Index is best used as a **trend indicator** (relative changes), not an absolute ppb reading.

---

## Libraries

Install via Arduino Library Manager (or include in `libraries/`):

- Sensirion:
  - `Sensirion I2C SGP41`
  - `Sensirion I2C SHT4x`
  - `VOCGasIndexAlgorithm`
- PMS7003:
  - A library that provides `PMS.h` (passive mode supported)
- ZE08-CH2O:
  - Uses `DFRobotHCHOSensor`-style API in the sketch.  
    If your library only supports `SoftwareSerial`, you may need an ESP32-compatible fork that accepts `HardwareSerial`.

ESP32 core includes:
- `WiFi.h`
- `WiFiClientSecure.h`
- `HTTPClient.h`
- `Wire.h`

---

## Build & upload (Arduino IDE)

1. Install **ESP32** board package.
2. Board: **ESP32 Dev Module** (or your DevKitC V4 equivalent).
3. Select the correct **COM port**.
4. Create `secrets.h` (see *Credentials*).
5. Open `firmware/v2/AirMonitoringSystem/AirMonitoringSystem.ino` and upload.
6. Open Serial Monitor at **115200 baud**.

---

## Case / enclosure
<table>
  <tr>
    <td><img src="https://github.com/user-attachments/assets/3fcf2e9f-58f9-403e-a3fe-984188b8244a" width="100%"></td>
    <td><img src="https://github.com/user-attachments/assets/4715c162-8c1e-450c-97b7-60d8e0a64c18" width="100%"></td>
    <td><img src="https://github.com/user-attachments/assets/64ca6098-19f8-4d24-96ca-e4efdb6c4d70" width="100%"></td>
  </tr>
</table>



- Size: **125 mm × 125 mm × 32 mm** body + **2 mm** lid (measured from the CAD files; 5.5 mm walls)
- Printed in **PLA-HS**
- Ventilation: multiple **3 mm holes** on 5 sides (bottom solid)

**Practical tips**
- Keep the **gas sensors away from hot spots** (ESP32 regulator, PMS7003 fan exhaust).
- Avoid placing the air inlets directly against a wall; keep a **few cm clearance**.

---

## Example: read latest values (ThingSpeak JSON)

`last.json` endpoint:
- `https://api.thingspeak.com/channels/<CHANNEL_ID>/fields/<FIELD>/last.json?api_key=<READ_API_KEY>`

This is convenient for **iOS Shortcuts** or simple scripts.

---

## Roadmap ideas

- Automatic actions:
  - Trigger a smart plug / purifier when thresholds are exceeded

---

## License

This project is licensed under the MIT License — see [LICENSE](LICENSE).

Copyright (c) 2026 SangHyeon Shin


---

## Credits

- Sensirion libraries and algorithms (SGP41, SHT4x, VOC Index)
- PMS sensor community libraries
- ThingSpeak platform
