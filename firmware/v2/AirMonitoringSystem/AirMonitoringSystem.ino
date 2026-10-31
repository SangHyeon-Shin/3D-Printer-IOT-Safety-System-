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

// ThingSpeak 무료 계정은 채널당 15초에 1회까지 -> 여유를 두고 20초
static const uint32_t UPLOAD_INTERVAL_MS = 20000;
static uint32_t lastUploadMs = 0;

// WiFi 재접속은 연결을 기다리지 않고 30초 간격으로만 재시도(측정 루프를 막지 않음)
static const uint32_t WIFI_RETRY_MS = 30000;
static uint32_t wifiLastAttemptMs = 0;
static bool wifiAttempted = false;
static bool wifiWasConnected = false;

// =====================
// I2C (SHT41 + SGP41)
// =====================
static const int I2C_SDA = 19;
static const int I2C_SCL = 22;
static const uint8_t SHT4X_ADDR = 0x44;

SensirionI2CSgp41 sgp41;
SensirionI2cSht4x sht4x;
VOCGasIndexAlgorithm vocAlgo;

// Sensirion VOC 알고리즘은 시작 후 45초(blackout) 동안 0을 출력 -> 그 뒤 값만 유효
static const uint32_t VOC_BLACKOUT_SAMPLES = 46;
static uint32_t vocAlgoSamples = 0;

// =====================
// HCHO (UART2)
// =====================
static const int HCHO_RX = 16;   // 센서 TX -> ESP32 RX2
static const int HCHO_TX = 17;   // (옵션) ESP32 TX2 -> 센서 RX
HardwareSerial HCHO(2);
DFRobotHCHOSensor hchoSensor(&HCHO);

// =====================
// PMS7003 (UART1, Passive)
// =====================
static const int PMS_RX = 4;  // PMS TXD -> ESP32 RX1
static const int PMS_TX = 5;  // PMS RXD -> ESP32 TX1
HardwareSerial pmsSerial(1);
PMS pms(pmsSerial);
PMS::DATA pmsData;

static const uint32_t PMS_INTERVAL_MS = 5000;  // 5초마다 1회 요청
static const uint32_t PMS_WAIT_MS     = 900;   // 요청 후 최대 0.9초 동안 수신 시도
static const uint8_t  PMS_MAX_MISSES  = 3;     // 연속 무응답 시 passive 명령 재전송
static uint32_t pmsLastReqMs = 0;
static uint32_t pmsReqStartMs = 0;
static bool pmsWaiting = false;
static uint8_t pmsMisses = 0;

// =====================
// MH-Z16 (PWM)
// =====================
static const int CO2_PWM_PIN = 33;          // MH-Z16 PWM -> GPIO33
static const int CO2_FULL_SCALE_PPM = 5000; // 센서 범위(2000/5000/10000 등)로 수정
static const uint32_t CO2_STALE_US = 3000000; // PWM 주기 약 1초 -> 3초간 상승 엣지가 없으면 결측

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
// Latest cache (업로드용) + 마지막 수신 시각
// =====================
// 최근에 받은 값만 유효: 센서가 빠지거나 고장 나면 마지막 값을 계속 올리지 않고 결측 처리
static const uint32_t SHT_STALE_MS  = 5000;   // 1초마다 측정
static const uint32_t VOC_STALE_MS  = 5000;   // 1초마다 측정
static const uint32_t HCHO_STALE_MS = 10000;  // 센서가 약 1초마다 송출
static const uint32_t PMS_STALE_MS  = 20000;  // 5초마다 요청

static float lastTempC = NAN;
static float lastRhPct = NAN;
static int   lastVocIndex = -1;   // field3에 업로드(=TVOC 역할, ÷5)
static float lastHchoPpb = NAN;
static int   lastPm1 = -1, lastPm25 = -1, lastPm10 = -1;

static uint32_t shtMs = 0, vocMs = 0, hchoMs = 0, pmsMs = 0;  // 마지막 수신 시각(0 = 아직 없음)

// =====================
// Helpers
// =====================
static bool isFresh(uint32_t receivedMs, uint32_t maxAgeMs) {
  return receivedMs != 0 && millis() - receivedMs <= maxAgeMs;
}

static bool shtValid()  { return isFresh(shtMs, SHT_STALE_MS); }
static bool vocValid()  { return isFresh(vocMs, VOC_STALE_MS); }
static bool hchoValid() { return isFresh(hchoMs, HCHO_STALE_MS); }
static bool pmsValid()  { return isFresh(pmsMs, PMS_STALE_MS); }

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

static void printOrDash(bool valid, float value, int digits) {
  if (valid) Serial.print(value, digits);
  else Serial.print('-');
}

static void printOrDash(bool valid, long value) {
  if (valid) Serial.print(value);
  else Serial.print('-');
}

// PMS 패시브 요청 + 논블로킹 수신
static void pmsTask() {
  uint32_t now = millis();

  if (!pmsWaiting && (now - pmsLastReqMs >= PMS_INTERVAL_MS)) {
    while (pmsSerial.available()) pmsSerial.read();
    pms.requestRead();
    pmsWaiting = true;
    pmsReqStartMs = now;
    pmsLastReqMs = now;
  }

  if (!pmsWaiting) return;

  // PMS 라이브러리 read()는 호출 1회에 1바이트만 처리 -> 쌓인 바이트를 한 번에 소진
  // (업로드 등으로 loop가 늦어져도 버퍼에 도착한 프레임을 놓치지 않음)
  while (pmsSerial.available()) {
    if (pms.read(pmsData)) {
      lastPm1  = (int)pmsData.PM_AE_UG_1_0;
      lastPm25 = (int)pmsData.PM_AE_UG_2_5;
      lastPm10 = (int)pmsData.PM_AE_UG_10_0;
      pmsMs = now;
      pmsWaiting = false;
      pmsMisses = 0;
      return;
    }
  }

  if (now - pmsReqStartMs > PMS_WAIT_MS) {
    pmsWaiting = false;
    // 부팅 직후 passive 명령을 센서가 놓쳤을 수 있으므로 연속 무응답이면 다시 보낸다
    if (++pmsMisses >= PMS_MAX_MISSES) {
      Serial.println("PMS7003: no response, re-sending passive mode");
      pms.passiveMode();
      pmsMisses = 0;
    }
  }
}

// CO2(PWM) 캡처값을 ppm으로 변환해서 캐시(1초마다 호출 권장)
static void co2UpdateOncePerSecond() {
  uint32_t pUs, hUs, riseUs;
  noInterrupts();
  pUs = co2PeriodUs;
  hUs = co2HighUs;
  riseUs = co2LastRiseUs;
  interrupts();

  // 마지막 상승 엣지가 오래됐으면(센서 분리·고장) 이전 측정값을 쓰지 않는다
  bool fresh = (riseUs != 0) && (micros() - riseUs <= CO2_STALE_US);
  if (!fresh || pUs < 900000 || pUs > 1100000 || hUs == 0) {
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
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.print("WiFi OK, IP=");
      Serial.println(WiFi.localIP());
    }
    return;
  }
  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("WiFi disconnected");
  }

  // 연결 완료를 기다리지 않는다: 시도만 하고 반환, 30초 뒤에도 끊겨 있으면 다시 시도
  uint32_t now = millis();
  if (wifiAttempted && now - wifiLastAttemptMs < WIFI_RETRY_MS) return;

  Serial.print("WiFi connecting to ");
  Serial.println(WIFI_SSID);
  if (!wifiAttempted) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  } else {
    WiFi.reconnect();
  }
  wifiAttempted = true;
  wifiLastAttemptMs = now;
}

static void appendField(String& body, int field, const String& value) {
  body += "&field";
  body += field;
  body += "=";
  body += value;
}

static void uploadThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;

  // 유효한(최근 수신) 값만 보낸다. 결측 필드는 생략 -> ThingSpeak에는 빈값으로 남는다
  // API Key는 URL이 아니라 POST 본문에 넣어 TLS 구간으로만 전송
  String body = String("api_key=") + THINGSPEAK_WRITE_API_KEY;
  int fields = 0;
  if (shtValid()) {
    appendField(body, 1, String(lastTempC, 2));
    appendField(body, 2, String(lastRhPct, 2));
    fields += 2;
  }
  if (vocValid()) {
    appendField(body, 3, String(lastVocIndex / 5));
    fields++;
  }
  if (hchoValid()) {
    appendField(body, 4, String(lroundf(lastHchoPpb)));
    fields++;
  }
  if (hasCo2) {
    appendField(body, 5, String(lastCo2Ppm));
    fields++;
  }
  if (pmsValid()) {
    appendField(body, 6, String(lastPm1));
    appendField(body, 7, String(lastPm25));
    appendField(body, 8, String(lastPm10));
    fields += 3;
  }
  if (fields == 0) {
    Serial.println("ThingSpeak: no valid data, upload skipped");
    return;
  }

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
    String payload = http.getString(); // 성공 시 entry id, 거부 시 "0"
    Serial.print("ThingSpeak HTTP ");
    Serial.print(httpCode);
    Serial.print(", resp=");
    Serial.print(payload);
    Serial.println(payload == "0" ? " (rejected: rate limit or invalid key)" : "");
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

  // WiFi: 연결을 기다리지 않는다(loop의 ensureWiFi가 상태 확인·재시도)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
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
  Serial.println("Output: T(C), RH(%), VOC_Index/5->field3, HCHO(ppb), CO2(ppm), PM(ug/m3); '-' = no valid data");
  Serial.println("SGP41 warmup: 10 s conditioning + 45 s VOC algorithm blackout.");
  Serial.println("PMS7003: passive request every 5 seconds.");
  Serial.println("ThingSpeak: HTTPS upload every 20 seconds (invalid fields omitted).");
}

void loop() {
  // (0) WiFi 유지(논블로킹)
  ensureWiFi();

  // (A) HCHO UART: 들어오면 즉시 마지막값 갱신
  if (hchoSensor.available()) {
    float ppm = hchoSensor.uartReadPPM();
    lastHchoPpb = ppm * 1000.0f; // ppm -> ppb
    hchoMs = millis();
  }

  // (B) PMS7003 패시브 태스크
  pmsTask();

  // (C) 1초 틱
  uint32_t now = millis();
  if (now - lastTick >= SAMPLE_MS) {
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
      shtMs = now;
    } else {
      Serial.print("ERR SHT4x=");
      Serial.println(err);
    }

    // SGP41 보정값: SHT41이 유효하면 실측값, 아니면 Sensirion 기본값(50 %RH, 25 °C)
    uint16_t rhTicks = 0x8000;
    uint16_t tTicks  = 0x6666;
    if (shtValid()) {
      rhTicks = rhToTicks(lastRhPct);
      tTicks  = tToTicks(lastTempC);
    }

    if (elapsed_s < 10) {
      uint16_t srawVoc = 0;
      err = sgp41.executeConditioning(rhTicks, tTicks, srawVoc);
      if (err) {
        Serial.print("ERR SGP41(cond)=");
        Serial.println(err);
      }
    } else {
      uint16_t srawVoc = 0, srawNox = 0;
      err = sgp41.measureRawSignals(rhTicks, tTicks, srawVoc, srawNox);
      if (!err) {
        int32_t vocIndex = vocAlgo.process(srawVoc);
        // 알고리즘 blackout 동안의 0은 측정값이 아니므로 버린다
        if (++vocAlgoSamples > VOC_BLACKOUT_SAMPLES) {
          lastVocIndex = (int)vocIndex;
          vocMs = now;
        }
      } else {
        Serial.print("ERR SGP41(meas)=");
        Serial.println(err);
      }
    }

    // ---- Serial print(디버그) ---- '-' = 유효한 값 없음
    Serial.print("T=");
    printOrDash(shtValid(), lastTempC, 2);
    Serial.print(" C, RH=");
    printOrDash(shtValid(), lastRhPct, 2);
    Serial.print(" %, TVOC(VOC_Index/5)=");
    printOrDash(vocValid(), (long)(lastVocIndex / 5));
    Serial.print(", HCHO=");
    printOrDash(hchoValid(), hchoValid() ? lroundf(lastHchoPpb) : 0L);
    Serial.print(" ppb, CO2=");
    printOrDash(hasCo2, (long)lastCo2Ppm);
    Serial.print(" ppm, PM1.0=");
    printOrDash(pmsValid(), (long)lastPm1);
    Serial.print(" ug/m3, PM2.5=");
    printOrDash(pmsValid(), (long)lastPm25);
    Serial.print(" ug/m3, PM10=");
    printOrDash(pmsValid(), (long)lastPm10);
    Serial.println(" ug/m3");
  }

  // (E) ThingSpeak 업로드(20초마다)
  if (millis() - lastUploadMs >= UPLOAD_INTERVAL_MS) {
    lastUploadMs = millis();
    uploadThingSpeak();
  }
}
