#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <esp_task_wdt.h>
#include <cstring>

// =============================================================================
// 硬體與《test/接线方式》對齊說明（重構思路）
// - DHT11：GPIO14，DATA 需 10K 上拉到 5V；採樣間隔 ≥2s（本程式強制節流）。
// - GZ-A1 光照：PCB 固定 AO→GPIO32（與馬達腳位無衝突，馬達程式保留）。
// - HC-SR501：與光照同用 GPIO32 時無法並存（類比光照優先），USE_PIR_MOTION=0 時上報 motion=0。
// - FC-22 / 煙霧類比：GPIO33（ADC1），AO 經 10K→腳、20K→GND 分壓後讀 ESP 端電壓。
//   上報僅 smoke：0=未判定有煙霧、1=有煙霧（依 FC22_SMOKE_ON_V / OFF_V 閾值，請依現場預熱後校準）。
// - L298N×2：IN 腳位與文檔一致；ENA/ENB 可日後接 GPIO13/15 做 PWM（本版仍為方向控制）。
// - LED：GPIO2。
// 雲端：上報屬性鍵名若與華為雲物模型不一致，請在控制台同步修改服務 ID 與屬性 ID。
// =============================================================================

// ========================
// 硬體腳位定義（與接線表一致）
// ========================

const int LED_PIN = 2;

const int MOTOR1_IN1 = 4;
const int MOTOR1_IN2 = 5;
const int MOTOR2_IN1 = 18;
const int MOTOR2_IN2 = 19;
const int MOTOR3_IN1 = 21;
const int MOTOR3_IN2 = 22;
const int MOTOR4_IN1 = 23;
const int MOTOR4_IN2 = 12;

// 可選：兩塊 L298N 的 ENA/ENB 若改接 PWM，請啟用下列腳位並擴充 ledc（接線方式已預留 13/15）
// const int L298N1_ENA_PWM = 13;
// const int L298N1_ENB_PWM = 15;

const int DHT11_PIN = 14;

// PCB 已打板：光照 AO 走 GPIO32。與馬達腳位不重疊，無需關閉馬達程式。
// 若日後 GPIO32 改接 PIR 且不用光照，可設 USE_PIR_MOTION=1 並改 LIGHT_SENSOR_AO_PIN。
#define USE_PIR_MOTION 0
const int LIGHT_SENSOR_AO_PIN = 32;
#if USE_PIR_MOTION
const int PIR_HC_SR501_PIN = 32;
#endif
const float LIGHT_LUX_MAX     = 20000.0f;
const int FC22_AO_PIN         = 33;  // 有害氣體模組 AO 經分壓後接入（ADC1）

// FC-22 煙霧判定：分壓後在 GPIO33 上的電壓（V）。濃度越高 AO 越高 → Vesp 越高（MQ 類典型特性）。
// 預熱約 2 分鐘後觀察 Serial 靜態讀數再調整閾值；OFF 須小於 ON 以形成遲滯。
const float FC22_SMOKE_ON_V  = 1.25f;   // 達到或超過 → 判定有煙霧
const float FC22_SMOKE_OFF_V = 1.05f;   // 已告警後低於此 → 恢復無煙霧

// ========================
// WiFi 與華為雲 MQTTS（8883 + WiFiClientSecure）
// 與平台下發之裝置接入資訊對齊：username / password / clientId / hostname / port
// ========================

const char* WIFI_SSID     = "CAINEONE";
const char* WIFI_PASSWORD = "12345678";

const char* MQTT_SERVER    = "4ce16929b3.st1.iotda-device.cn-north-4.myhuaweicloud.com";
const int   MQTT_PORT      = 8883;
const char* MQTT_DEVICE_ID = "69a8f163cbb0cf6bb943fca5_20260305";
const char* MQTT_CLIENT_ID = "69a8f163cbb0cf6bb943fca5_20260305_0_0_2026041110";
const char* MQTT_USERNAME  = "69a8f163cbb0cf6bb943fca5_20260305";
const char* MQTT_PASSWORD  = "dda332c6c6777aff3660fa32add9d306d728baf1c9b8404809cfc31fb85386cd";

const char* TOPIC_REPORT  = "$oc/devices/69a8f163cbb0cf6bb943fca5_20260305/sys/properties/report";
const char* TOPIC_COMMAND = "$oc/devices/69a8f163cbb0cf6bb943fca5_20260305/sys/commands/#";

// ========================
// 全域物件
// ========================

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
DHT dht(DHT11_PIN, DHT11);

static unsigned long lastWiFiAttemptMs  = 0;
static unsigned long lastMqttAttemptMs  = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS  = 5000;
const unsigned long MQTT_RETRY_INTERVAL_MS  = 5000;

static unsigned long lastDhtReadMs = 0;
const unsigned long DHT_MIN_INTERVAL_MS = 2000;

bool isPlaceholder(const char* s) {
  if (!s) return true;
  return (strlen(s) == 0) || (strncmp(s, "your_", 5) == 0) || (strncmp(s, "$oc/devices/your_", 16) == 0);
}

bool cloudConfigured() {
  return !isPlaceholder(MQTT_SERVER) &&
         !isPlaceholder(MQTT_DEVICE_ID) &&
         !isPlaceholder(MQTT_CLIENT_ID) &&
         !isPlaceholder(MQTT_USERNAME) &&
         !isPlaceholder(MQTT_PASSWORD) &&
         !isPlaceholder(TOPIC_REPORT);
}

// ========================
// 馬達
// ========================

enum MotorDirection {
  MOTOR_STOP = 0,
  MOTOR_FORWARD,
  MOTOR_BACKWARD
};

struct MotorPins {
  int in1;
  int in2;
};

MotorPins motors[4] = {
  {MOTOR1_IN1, MOTOR1_IN2},
  {MOTOR2_IN1, MOTOR2_IN2},
  {MOTOR3_IN1, MOTOR3_IN2},
  {MOTOR4_IN1, MOTOR4_IN2}
};

void setMotorDirection(uint8_t index, MotorDirection dir) {
  if (index >= 4) return;
  int in1 = motors[index].in1;
  int in2 = motors[index].in2;
  switch (dir) {
    case MOTOR_FORWARD:
      digitalWrite(in1, HIGH);
      digitalWrite(in2, LOW);
      break;
    case MOTOR_BACKWARD:
      digitalWrite(in1, LOW);
      digitalWrite(in2, HIGH);
      break;
    case MOTOR_STOP:
    default:
      digitalWrite(in1, LOW);
      digitalWrite(in2, LOW);
      break;
  }
}

void setAllMotors(MotorDirection dir) {
  for (uint8_t i = 0; i < 4; ++i) {
    setMotorDirection(i, dir);
  }
}

static bool parseMotorDirectionFromJson(JsonVariantConst v, MotorDirection* out) {
  if (v.is<int>()) {
    int x = v.as<int>();
    if (x == 0) { *out = MOTOR_STOP; return true; }
    if (x == 1) { *out = MOTOR_FORWARD; return true; }
    if (x == 2) { *out = MOTOR_BACKWARD; return true; }
    return false;
  }
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s) return false;
    if (!strcasecmp(s, "stop") || !strcasecmp(s, "STOP")) { *out = MOTOR_STOP; return true; }
    if (!strcasecmp(s, "forward") || !strcasecmp(s, "f") || !strcasecmp(s, "on")) { *out = MOTOR_FORWARD; return true; }
    if (!strcasecmp(s, "backward") || !strcasecmp(s, "back") || !strcasecmp(s, "reverse")) { *out = MOTOR_BACKWARD; return true; }
  }
  return false;
}

// ========================
// 感測器讀取（對齊模組電氣特性）
// ========================

static void readDhtThrottled(float* tempC, float* rhPct, bool* ok) {
  unsigned long now = millis();
  if (now - lastDhtReadMs < DHT_MIN_INTERVAL_MS) {
    *ok = false;
    return;
  }
  lastDhtReadMs = now;
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    *ok = false;
    return;
  }
  *tempC = t;
  *rhPct = h;
  *ok = true;
}

// GZ-A1：強光 → AO 電壓低；上報「亮度百分比」為越大越亮（0–100）
static float readLightBrightnessPercent() {
  int   raw = analogRead(LIGHT_SENSOR_AO_PIN);
  float v   = raw * (3.3f / 4095.0f);
  float bright01 = (3.3f - v) / 3.3f;
  if (bright01 < 0.0f) bright01 = 0.0f;
  if (bright01 > 1.0f) bright01 = 1.0f;
  return bright01 * 100.0f;
}

static float readLightLuxApprox() {
  int   raw = analogRead(LIGHT_SENSOR_AO_PIN);
  float v   = raw * (3.3f / 4095.0f);
  float bright01 = (3.3f - v) / 3.3f;
  if (bright01 < 0.0f) bright01 = 0.0f;
  if (bright01 > 1.0f) bright01 = 1.0f;
  return bright01 * LIGHT_LUX_MAX;
}

#if USE_PIR_MOTION
// HC-SR501：數位輸出，一般為偵測到移動時 OUT=HIGH（須與光照分腳）
static bool readPirMotion() {
  return digitalRead(PIR_HC_SR501_PIN) == HIGH;
}
#endif

// FC-22：讀分壓後電壓並做遲滯比較，得到是否判定有煙霧（布林）
static bool readSmokePresent() {
  int   raw  = analogRead(FC22_AO_PIN);
  float vEsp = raw * (3.3f / 4095.0f);

  static bool latched = false;
  if (!latched) {
    if (vEsp >= FC22_SMOKE_ON_V) latched = true;
  } else {
    if (vEsp <= FC22_SMOKE_OFF_V) latched = false;
  }
  return latched;
}

struct EnvData {
  bool     dht_ok;
  float    temperature;
  float    humidity;
  bool     motion;  // HC-SR501：true=偵測到移動
  float    light_percent;    // 0–100，越大越亮
  float    illuminance_lux;  // 依 LIGHT_LUX_MAX 線性換算之近似照度
  bool     smoke;   // FC-22：true=判定有煙霧（類比閾值 + 遲滯）
};

EnvData readEnvData() {
  EnvData d;
  d.dht_ok = false;
  readDhtThrottled(&d.temperature, &d.humidity, &d.dht_ok);
#if USE_PIR_MOTION
  d.motion = readPirMotion();
#else
  d.motion = false;  // GPIO32 供光照類比，PIR 未接或未啟用
#endif
  d.light_percent    = readLightBrightnessPercent();
  d.illuminance_lux  = readLightLuxApprox();
  d.smoke = readSmokePresent();
  return d;
}

// ========================
// WiFi / MQTT
// ========================

void connectWiFi() {
  Serial.println();
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();
}

// 將下行 topic 轉成華為雲要求的 response topic
static bool buildHuaweiCommandResponseTopic(const char* inTopic, char* out, size_t outLen) {
  const char* p = strstr(inTopic, "/sys/commands/request_id=");
  if (!p) return false;
  size_t prefixLen = (size_t)(p - inTopic) + strlen("/sys/commands");
  const char* tail = p + strlen("/sys/commands");
  if (prefixLen + strlen("/response") + strlen(tail) + 1 > outLen) return false;
  memcpy(out, inTopic, prefixLen);
  out[prefixLen] = '\0';
  strcat(out, "/response");
  strcat(out, tail);
  return true;
}

static void publishCommandResponse(const char* responseTopic, int resultCode,
                                   const char* resultMsg) {
  if (!mqttClient.connected() || !responseTopic[0]) return;
  JsonDocument rsp;
  rsp["result_code"]   = resultCode;
  rsp["response_name"] = "COMMAND_RESPONSE";
  JsonObject paras = rsp["paras"].to<JsonObject>();
  paras["result"] = resultMsg;

  char buf[192];
  size_t n = serializeJson(rsp, buf, sizeof(buf));
  mqttClient.publish(responseTopic, buf, n);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[384];
  if (length >= sizeof(msg)) {
    Serial.println("[MQTT] command payload too large");
    return;
  }
  memcpy(msg, payload, length);
  msg[length] = '\0';

  Serial.print("[MQTT] cmd [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(msg);

  char respTopic[192];
  if (!buildHuaweiCommandResponseTopic(topic, respTopic, sizeof(respTopic))) {
    Serial.println("[MQTT] cannot build response topic");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    publishCommandResponse(respTopic, 1, "json_parse_failed");
    return;
  }

  const char* cmdName = doc["command_name"] | "";
  JsonObjectConst paras = doc["paras"].as<JsonObjectConst>();

  // 與物模型一致時可校驗 service_id（可選）
  // const char* svc = doc["service_id"] | "";

  int rc = 0;
  const char* rmsg = "success";

  if (!strcasecmp(cmdName, "motor_set") || !strcasecmp(cmdName, "MotorSet")) {
    int idx = paras["motor_index"] | paras["index"] | -1;
    MotorDirection dir;
    if (idx < 0 || idx > 3 || !parseMotorDirectionFromJson(paras["direction"] | paras["dir"], &dir)) {
      rc = 2;
      rmsg = "bad_motor_paras";
    } else {
      setMotorDirection((uint8_t)idx, dir);
    }
  } else if (!strcasecmp(cmdName, "motor_all") || !strcasecmp(cmdName, "MotorAll")) {
    MotorDirection dir;
    if (!parseMotorDirectionFromJson(paras["direction"] | paras["dir"], &dir)) {
      rc = 3;
      rmsg = "bad_motor_all_paras";
    } else {
      setAllMotors(dir);
    }
  } else if (cmdName[0] == '\0') {
    rc = 4;
    rmsg = "missing_command_name";
  } else {
    rc = 5;
    rmsg = "unknown_command";
  }

  publishCommandResponse(respTopic, rc, rmsg);
}

bool connectMQTTOnce() {
  if (!cloudConfigured()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  espClient.setInsecure();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);
  mqttClient.setKeepAlive(120);

  Serial.print("MQTT connect ");
  Serial.print(MQTT_SERVER);
  Serial.print(" clientId=");
  Serial.print(MQTT_CLIENT_ID);
  Serial.print(" ... ");

  if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("OK");
    if (!isPlaceholder(TOPIC_COMMAND)) {
      mqttClient.subscribe(TOPIC_COMMAND);
    }
    return true;
  }

  Serial.print("fail rc=");
  Serial.println(mqttClient.state());
  return false;
}

// ========================
// 上報華為雲（屬性鍵請與產品模型一致，必要時只保留控制台已定義的欄位）
// ========================

void reportEnvDataToCloud() {
  if (!cloudConfigured()) return;
  if (!mqttClient.connected()) {
    static unsigned long lastWarn = 0;
    if (millis() - lastWarn > 15000) {
      lastWarn = millis();
      Serial.println("[Cloud] MQTT offline, skip report.");
    }
    return;
  }

  EnvData d = readEnvData();

  JsonDocument root;
  JsonArray services = root["services"].to<JsonArray>();
  JsonObject svc     = services.add<JsonObject>();
  svc["service_id"]  = "ESP32-IOT";

  JsonObject props = svc["properties"].to<JsonObject>();
  if (d.dht_ok) {
    props["temperature"] = d.temperature;
    props["humidity"]      = d.humidity;
  }
  // 人體感測：0=無移動，1=偵測到移動（物模型：motion，int）
  props["motion"] = d.motion ? 1 : 0;
  // 煙霧：0=無，1=有（物模型：smoke，int；勿再用 gas_adc_v / gas_ao_est_v）
  props["smoke"]  = d.smoke ? 1 : 0;
  // 物模型屬性名須與華為雲產品定義一致（常見為 light / illuminance_lux；若控制台鍵名不同請改下列字串）
  props["light"]           = d.light_percent;
  props["illuminance_lux"] = d.illuminance_lux;

  char jsonBuffer[512];
  size_t len = serializeJson(root, jsonBuffer, sizeof(jsonBuffer));

  bool ok = mqttClient.publish(TOPIC_REPORT, jsonBuffer, len);
  Serial.print("[Cloud] report ");
  Serial.println(jsonBuffer);
  Serial.println(ok ? "[Cloud] publish OK" : "[Cloud] publish FAILED");
}

// ========================

unsigned long lastReportMs = 0;
const unsigned long REPORT_INTERVAL_MS = 5000;

void setup() {
  Serial.begin(115200);
  delay(200);

  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  Serial.println("ESP32-WROOM-32E: wiring-aligned firmware");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  for (uint8_t i = 0; i < 4; ++i) {
    pinMode(motors[i].in1, OUTPUT);
    pinMode(motors[i].in2, OUTPUT);
    setMotorDirection(i, MOTOR_STOP);
  }

  dht.begin();
#if USE_PIR_MOTION
  pinMode(PIR_HC_SR501_PIN, INPUT);
#endif
  pinMode(LIGHT_SENSOR_AO_PIN, INPUT);
  pinMode(FC22_AO_PIN, INPUT);
  analogSetAttenuation(ADC_11db);

  connectWiFi();
  setAllMotors(MOTOR_STOP);
}

void loop() {
  esp_task_wdt_reset();

  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWiFiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
      lastWiFiAttemptMs = now;
      Serial.println("WiFi retry...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  } else {
    static bool printedWiFi = false;
    if (!printedWiFi) {
      printedWiFi = true;
      Serial.print("WiFi OK IP=");
      Serial.println(WiFi.localIP());
    }
  }

  if (cloudConfigured()) {
    if (!mqttClient.connected()) {
      if (now - lastMqttAttemptMs >= MQTT_RETRY_INTERVAL_MS) {
        lastMqttAttemptMs = now;
        connectMQTTOnce();
      }
    } else {
      mqttClient.loop();
    }
  }

  static unsigned long lastLedToggle = 0;
  const unsigned long LED_BLINK_MS = 500;
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastLedToggle >= LED_BLINK_MS) {
      lastLedToggle = now;
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  if (now - lastReportMs >= REPORT_INTERVAL_MS) {
    lastReportMs = now;
    reportEnvDataToCloud();
  }
}
