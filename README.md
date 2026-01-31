# 3D Printer IoT Safety System
<img width="1852" height="967" alt="Image" src="https://github.com/user-attachments/assets/343c64a8-7450-4011-9045-496f9dc58979" />
An **ESP32 DevKitC V4**-based air-quality monitoring node for a home 3D-printer setup (tested with **Bambu Lab P1SC**).  
It monitors **temperature/humidity, TVOC (scaled VOC Index), formaldehyde (HCHO), CO₂, and particulate matter (PM1.0/PM2.5/PM10)** and uploads the data to **ThingSpeak** for dashboards and mobile notifications.

> ⚠️ **Safety note**  
> This project helps you *observe* air quality and take action (ventilation / filtration).  
> It is **not** a certified safety device and does **not** replace smoke detectors, fire alarms, or proper ventilation.

---

## Features

- **Sensors (multi-sensor fusion, non-blocking loop)**
  - Temp & RH: **SHT41 (SHT4x)**
  - TVOC proxy: **SGP41 VOC Index** *(scaled ÷5 to fit 0–100 range)*
  - Formaldehyde: **ZE08-CH2O** (UART)
  - CO₂: **MH-Z16** (PWM output)
  - Dust / PM: **PMS7003** (UART, **passive mode** request)
- **Cloud**
  - Upload to **ThingSpeak** every **20 s** (ThingSpeak free-tier minimum interval)
- **Mobile / Automation**
  - Works well with **ThingsView / ThingSpeak dashboards**
  - Easy to read latest values from `.../fields/<n>/last.json` in **iOS Shortcuts**, Home automation, etc.
- **Hardware enclosure**
  - Includes case files: **.3mf / .f3d / .step** (printed in **PLA-HS**)

---

## Data mapping (ThingSpeak fields)
<table width="100%">
  <tr>
    <td width="33%">
      <img
        src="https://github.com/user-attachments/assets/8bf51ecf-3078-443a-881e-afd320313b84"
        alt="Image"
        style="width:100%; height:260px; object-fit:cover; display:block;"
      />
    </td>
    <td width="33%">
      <img
        src="https://github.com/user-attachments/assets/d6a1eb07-cf87-4e16-a80a-c1d1ec46526b"
        alt="Image"
        style="width:100%; height:260px; object-fit:cover; display:block;"
      />
    </td>
    <td width="33%">
      <img
        src="https://github.com/user-attachments/assets/13ab236a-af40-458e-b194-3f15f0d194a6"
        alt="Image"
        style="width:100%; height:260px; object-fit:cover; display:block;"
      />
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

### File
- `Sensor_Thingspeak.ino`

### Configs you may change
- **CO₂ full-scale** (your setting):
  ```cpp
  static const int CO2_FULL_SCALE_PPM = 5000;
  ```
- **Upload interval** (default 20 seconds):
  ```cpp
  static const uint32_t UPLOAD_INTERVAL_MS = 20000;
  ```

### TVOC scaling (VOC Index ÷ 5)
This project converts Sensirion’s **VOC Index** to a 0–100-like scale:
- Upload:
  ```cpp
  + "&field3=" + String(tvoc/5)
  ```
- Serial log:
  ```cpp
  Serial.print(lastVocIndex/5);
  ```

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
- `HTTPClient.h`
- `Wire.h`

---

## Build & upload (Arduino IDE)

1. Install **ESP32** board package.
2. Board: **ESP32 Dev Module** (or your DevKitC V4 equivalent).
3. Select the correct **COM port**.
4. Open `Sensor_Thingspeak.ino` and upload.
5. Open Serial Monitor at **115200 baud**.

---

## Case / enclosure
<table>
  <tr>
    <td><img src="https://github.com/user-attachments/assets/3fcf2e9f-58f9-403e-a3fe-984188b8244a" width="100%"></td>
    <td><img src="https://github.com/user-attachments/assets/4715c162-8c1e-450c-97b7-60d8e0a64c18" width="100%"></td>
    <td><img src="https://github.com/user-attachments/assets/64ca6098-19f8-4d24-96ca-e4efdb6c4d70" width="100%"></td>
  </tr>
</table>



- Size: **105 mm × 105 mm × 32 mm**
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

This project is licensed under the MIT License.

Copyright (c) 2026 SangHyeon Shin


---

## Credits

- Sensirion libraries and algorithms (SGP41, SHT4x, VOC Index)
- PMS sensor community libraries
- ThingSpeak platform
