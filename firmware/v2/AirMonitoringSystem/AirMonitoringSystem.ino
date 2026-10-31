#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include <DFRobotHCHOSensor.h>

#include <SensirionI2CSgp41.h>
#include <SensirionI2cSht4x.h>
#include <VOCGasIndexAlgorithm.h>

#include "PMS.h"

// =====================
// ThingSpeak / WiFi
// =====================
// 자격 증명은 저장소에 두지 않는다: secrets.example.h를 secrets.h로 복사해 값을 채운다(.gitignore 대상)
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "secrets.h not found: copy secrets.example.h to secrets.h and fill in your credentials"
#endif
#include "thingspeak_ca.h"

static const char* THINGSPEAK_UPDATE_URL = "https://api.thingspeak.com/update";

// ThingSpeak 제한 고려: 15초 이상 권장 -> 20초
static const uint32_t UPLOAD_INTERVAL_MS = 20000;
static uint32_t lastUploadMs = 0;

// =====================
// I2C (SHT41 + SGP41)
// =====================
static const int I2C_SDA = 19;
static const int I2C_SCL = 22;
static const uint8_t SHT4X_ADDR = 0x44;

SensirionI2CSgp41 sgp41;
SensirionI2cSht4x sht4x;
VOCGasIndexAlgorithm vocAlgo;

// =====================
// HCHO (UART2)
// =====================
static const int HCHO_RX = 16;   // 센서 TX -> ESP32 RX2
static const int HCHO_TX = 17;   // (옵션) ESP32 TX2 -> 센서 RX
HardwareSerial HCHO(2);
DFRobotHCHOSensor hchoSensor(&HCHO);

static bool  hasHcho = false;
static float lastHchoPpb = NAN;

// =====================
// PMS7003 (UART1, Passive)
// =====================
static const int PMS_RX = 4;  // PMS TXD -> ESP32 RX1
static const int PMS_TX = 5;  // PMS RXD -> ESP32 TX1
HardwareSerial pmsSerial(1);
PMS pms(pmsSerial);
PMS::DATA pmsData;

static bool hasPms = false;
static PMS::DATA lastPmsData;

static const uint32_t PMS_INTERVAL_MS = 5000;  // 5초마다 1회 요청
static const uint32_t PMS_WAIT_MS     = 900;   // 요청 후 최대 0.9초 동안 수신 시도
static uint32_t pmsLastReqMs = 0;
static uint32_t pmsReqStartMs = 0;
static bool pmsWaiting = false;

// =====================
// MH-Z16 (PWM)
// =====================
static const int CO2_PWM_PIN = 33;          // MH-Z16 PWM -> GPIO33
static const int CO2_FULL_SCALE_PPM = 5000; // 센서 범위(2000/5000/10000 등)로 수정

volatile uint32_t co2LastRiseUs = 0;
volatile uint32_t co2PeriodUs = 0;
volatile uint32_t co2HighUs = 0;

static bool hasCo2 = false;
static int  lastCo2Ppm = -1;

void IRAM_ATTR onCo2PwmChange() {
  uint32_t now = micros();
  int level = digitalRead(CO2_PWM_PIN);

  if (level == HIGH) {
    if (co2LastRiseUs != 0) co2PeriodUs = now - co2LastRiseUs;
    co2LastRiseUs = now;
  } else {
    if (co2LastRiseUs != 0) co2HighUs = now - co2LastRiseUs;
  }
}

// =====================
// Timing (SGP41 1Hz)
// =====================
static const uint32_t SAMPLE_MS = 1000;
static uint32_t lastTick = 0;
static uint32_t t0 = 0;

// =====================
// Latest cache (업로드용)
// =====================
static float lastTempC = NAN;
static float lastRhPct = NAN;
static int   lastVocIndex = -1;   // field3에 업로드(=TVOC 역할)
static int   lastPm1 = -1, lastPm25 = -1, lastPm10 = -1;

// =====================
// Helpers
// =====================
static uint16_t rhToTicks(float rh_percent) {
  if (rh_percent < 0) rh_percent = 0;
  if (rh_percent > 100) rh_percent = 100;
  return (uint16_t)(rh_percent * 65535.0f / 100.0f + 0.5f);
}

static uint16_t tToTicks(float t_c) {
  if (t_c < -45) t_c = -45;
  if (t_c > 130) t_c = 130;
  return (uint16_t)((((t_c + 45.0f) * 65535.0f) / 175.0f) + 0.5f);
}

// PMS 패시브 요청 + 논블로킹 수신 시도
static void pmsTask() {
  uint32_t now = millis();

  if (!pmsWaiting && (now - pmsLastReqMs >= PMS_INTERVAL_MS)) {
    while (pmsSerial.available()) pmsSerial.read();
    pms.requestRead();
    pmsWaiting = true;
    pmsReqStartMs = now;
    pmsLastReqMs = now;
  }

  if (pmsWaiting) {
    if (pms.read(pmsData)) {
      lastPmsData = pmsData;
      hasPms = true;
      pmsWaiting = false;

      // 업로드 캐시 갱신
      lastPm1  = (int)lastPmsData.PM_AE_UG_1_0;
      lastPm25 = (int)lastPmsData.PM_AE_UG_2_5;
      lastPm10 = (int)lastPmsData.PM_AE_UG_10_0;
    } else if (now - pmsReqStartMs > PMS_WAIT_MS) {
      pmsWaiting = false;
    }
  }
}

// CO2(PWM) 캡처값을 ppm으로 변환해서 캐시(1초마다 호출 권장)
static void co2UpdateOncePerSecond() {
  uint32_t pUs, hUs;
  noInterrupts();
  pUs = co2PeriodUs;
  hUs = co2HighUs;
  interrupts();

  if (pUs < 900000 || pUs > 1100000 || hUs == 0) {
    hasCo2 = false;
    lastCo2Ppm = -1;
    return;
  }

  float pMs = pUs / 1000.0f;
  float hMs = hUs / 1000.0f;
  float lMs = pMs - hMs;

  float denom = (hMs + lMs - 4.0f);
  float ppm = (denom > 0.0f) ? (CO2_FULL_SCALE_PPM * (hMs - 2.0f) / denom) : -1.0f;

  if (ppm < 0) ppm = 0;
  if (ppm > CO2_FULL_SCALE_PPM) ppm = CO2_FULL_SCALE_PPM;

  lastCo2Ppm = (int)lroundf(ppm);
  hasCo2 = true;
}

// =====================
// WiFi + ThingSpeak
// =====================
static void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("WiFi connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t tStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - tStart < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed (will retry)");
  }
}

static void uploadThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;

  // 유효하지 않은 값은 -1로 업로드(그래프에서 결측치처럼 처리하기 쉬움)
  float fTemp = isnan(lastTempC) ? -1.0f : lastTempC;
  float fHum  = isnan(lastRhPct) ? -1.0f : lastRhPct;

  int tvoc = (lastVocIndex < 0) ? -1 : lastVocIndex; // field3
  int hcho = (!hasHcho || isnan(lastHchoPpb)) ? -1 : (int)lroundf(lastHchoPpb);
  int co2  = (!hasCo2 || lastCo2Ppm < 0) ? -1 : lastCo2Ppm;

  int pm1  = (lastPm1  < 0) ? -1 : lastPm1;
  int pm25 = (lastPm25 < 0) ? -1 : lastPm25;
  int pm10 = (lastPm10 < 0) ? -1 : lastPm10;

  // API Key는 URL이 아니라 POST 본문에 넣어 TLS 구간으로만 전송
  String body = String("api_key=") + THINGSPEAK_WRITE_API_KEY
              + "&field1=" + String(fTemp, 2)
              + "&field2=" + String(fHum, 2)
              + "&field3=" + String(tvoc/5)
              + "&field4=" + String(hcho)
              + "&field5=" + String(co2)
              + "&field6=" + String(pm1)
              + "&field7=" + String(pm25)
              + "&field8=" + String(pm10);

  WiFiClientSecure client;
  client.setCACert(THINGSPEAK_ROOT_CA);  // 서버 인증서 검증(thingspeak_ca.h)

  HTTPClient http;
  if (!http.begin(client, THINGSPEAK_UPDATE_URL)) {
    Serial.println("ThingSpeak: HTTPS begin failed");
    return;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int httpCode = http.POST(body);
  if (httpCode > 0) {
    String payload = http.getString(); // 성공 시 entry id(숫자) 반환이 보통
    Serial.print("ThingSpeak HTTP ");
    Serial.print(httpCode);
    Serial.print(", resp=");
    Serial.println(payload);
  } else {
    // 음수 코드: 연결 실패(TLS 인증서 검증 실패 포함 -> thingspeak_ca.h 확인)
    Serial.print("ThingSpeak HTTPS failed: ");
    Serial.println(HTTPClient::errorToString(httpCode));
  }
  http.end();
}

// =====================
// Setup / Loop
// =====================
void setup() {
  Serial.begin(115200);
  delay(300);

  // WiFi
  ensureWiFi();

  // ---- UART2 (HCHO) ----
  HCHO.begin(9600, SERIAL_8N1, HCHO_RX, HCHO_TX);

  // ---- UART1 (PMS7003) ----
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);
  pms.passiveMode();

  // ---- I2C ----
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  sht4x.begin(Wire, SHT4X_ADDR);
  sgp41.begin(Wire);

  // ---- CO2 PWM ----
  pinMode(CO2_PWM_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(CO2_PWM_PIN), onCo2PwmChange, CHANGE);

  Serial.println();
  Serial.println("Output: T(C), RH(%), VOC_Index->field3, HCHO(ppb), CO2(ppm), PM(ug/m3)");
  Serial.println("SGP41 warmup: first 10 seconds conditioning.");
  Serial.println("PMS7003: passive request every 5 seconds.");
  Serial.println("ThingSpeak: upload every 20 seconds.");
}

void loop() {
  // (0) WiFi 유지
  ensureWiFi();

  // (A) HCHO UART: 들어오면 즉시 마지막값 갱신
  if (hchoSensor.available()) {
    float ppm = hchoSensor.uartReadPPM();
    lastHchoPpb = ppm * 1000.0f; // ppm -> ppb
    hasHcho = true;
  }

  // (B) PMS7003 패시브 태스크
  pmsTask();

  // (C) 1초 틱
  uint32_t now = millis();
  if (now - lastTick < SAMPLE_MS) {
    // 업로드 타이밍은 loop에서 따로 체크
  } else {
    lastTick = now;

    if (t0 == 0) t0 = now;
    uint32_t elapsed_s = (now - t0) / 1000;

    // (D) CO2 업데이트(1초마다)
    co2UpdateOncePerSecond();

    // ---- SHT41 ----
    float t = NAN, rh = NAN;
    uint16_t err = sht4x.measureHighPrecision(t, rh);
    if (!err) {
      lastTempC = t;
      lastRhPct = rh;
    } else {
      Serial.print("ERR SHT4x=");
      Serial.println(err);
    }

    // SHT 값이 유효할 때만 SGP41에 보정값 넣기
    if (!isnan(lastTempC) && !isnan(lastRhPct)) {
      uint16_t rhTicks = rhToTicks(lastRhPct);
      uint16_t tTicks  = tToTicks(lastTempC);

      if (elapsed_s < 10) {
        uint16_t srawVoc = 0;
        err = sgp41.executeConditioning(rhTicks, tTicks, srawVoc);
        if (err) {
          Serial.print("ERR SGP41(cond)=");
          Serial.println(err);
        }
        lastVocIndex = -1; // 워밍업 동안은 업로드-출력에서 - 처리
      } else {
        uint16_t srawVoc = 0, srawNox = 0;
        err = sgp41.measureRawSignals(rhTicks, tTicks, srawVoc, srawNox);
        if (!err) {
          lastVocIndex = (int)vocAlgo.process(srawVoc);
        } else {
          Serial.print("ERR SGP41(meas)=");
          Serial.println(err);
        }
      }
    }

    // ---- Serial print(디버그) ----
    Serial.print("T=");
    Serial.print(isnan(lastTempC) ? -1 : lastTempC, 2);
    Serial.print(" C, RH=");
    Serial.print(isnan(lastRhPct) ? -1 : lastRhPct, 2);
    Serial.print(" %, TVOC(VOC_Index)=");
    Serial.print(lastVocIndex/5);
    Serial.print(", HCHO=");
    Serial.print((!hasHcho || isnan(lastHchoPpb)) ? -1 : (int)lroundf(lastHchoPpb));
    Serial.print(" ppb, CO2=");
    Serial.print((!hasCo2 || lastCo2Ppm < 0) ? -1 : lastCo2Ppm);
    Serial.print(" ppm, PM1.0=");
    Serial.print(lastPm1);
    Serial.print(" ug/m3, PM2.5=");
    Serial.print(lastPm25);
    Serial.print(" ug/m3, PM10=");
    Serial.print(lastPm10);
    Serial.println(" ug/m3");
  }

  // (D) ThingSpeak 업로드(20초마다)
  if (millis() - lastUploadMs >= UPLOAD_INTERVAL_MS) {
    lastUploadMs = millis();
    uploadThingSpeak();
  }
}
