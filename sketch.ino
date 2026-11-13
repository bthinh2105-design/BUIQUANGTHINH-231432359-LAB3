
/************************************************************
 * DE TAI 3: MACH BAO CHAY GIA DINH
 * ESP32 + MQ2 + DHT22 + MQTT + FreeRTOS
 *
 * CAC TASK:
 * 1. TaskReadData  : Core 1 - Doc cam bien
 * 2. TaskSendData  : Core 0 - Gui du lieu len MQTT
 * 3. TaskAlert     : Core 1 - Xu ly canh bao va dieu khien
 *
 * QUEUE:
 * 1. dataQueue     : Du lieu gui MQTT
 * 2. dataAlertQueue: Du lieu xu ly canh bao
 * 3. cmdQueue      : Lenh dieu khien tu Web
 ************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <math.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

//============================================================
// 1. KHAI BAO CHAN GPIO - THEO SO DO WOKWI
//============================================================

#define MQ2_PIN       34
#define DHT_PIN       4

#define FAN_LED       14
#define PUMP_LED      27

#define DHT_TYPE      DHT22

DHT dht(DHT_PIN, DHT_TYPE);

//============================================================
// 2. CAU HINH WIFI
//============================================================

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

//============================================================
// 3. CAU HINH MQTT BROKER
//============================================================

const char* MQTT_SERVER = "broker.hivemq.com";
const int MQTT_PORT = 1883;

// Topic goc
#define MQTT_BASE "lab3/firealarm/demo"

// Topic ESP32 gui len Broker
const char* TOPIC_GAS =
  MQTT_BASE "/gas";

const char* TOPIC_TEMP =
  MQTT_BASE "/temp";

const char* TOPIC_FAN =
  MQTT_BASE "/fan";

const char* TOPIC_PUMP =
  MQTT_BASE "/pump";

const char* TOPIC_ALARM =
  MQTT_BASE "/alarm";

const char* TOPIC_MODE =
  MQTT_BASE "/mode";

// Topic ESP32 nhan lenh tu Web
const char* TOPIC_CMD_FAN =
  MQTT_BASE "/cmd/fan";

const char* TOPIC_CMD_PUMP =
  MQTT_BASE "/cmd/pump";

const char* TOPIC_CMD_MODE =
  MQTT_BASE "/cmd/mode";

// MQTT Client
WiFiClient espClient;
PubSubClient mqtt(espClient);

//============================================================
// 4. NGUONG DIEU KHIEN
//============================================================

// Gia tri ADC MQ2: 0 - 4095
// Day la nguong thu nghiem ban dau, can dieu chinh
// dua tren gia tri hien thi tren Serial Monitor.

#define GAS_THRESHOLD_ADC 1800

// Nhiet do vuot 50 do C thi bat may bom
#define TEMP_THRESHOLD_C 50.0

// Chu ky doc cam bien
#define READ_INTERVAL_MS 1000

//============================================================
// 5. CAU TRUC DU LIEU CAM BIEN
//============================================================

struct SensorData {
  uint16_t gasADC;
  float temperature;
  uint32_t sampleNumber;
};

//============================================================
// 6. CAU TRUC LENH DIEU KHIEN
//============================================================

enum CommandType {
  CMD_SET_MODE,
  CMD_SET_FAN,
  CMD_SET_PUMP
};

struct ControlCommand {
  CommandType type;
  bool value;
};

//============================================================
// 7. TRANG THAI HE THONG
//============================================================

struct OutputState {
  bool autoMode;
  bool fanOn;
  bool pumpOn;
};

// Dung Critical Section de chia se trang thai an toan
// giua TaskSendData va TaskAlert.

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

OutputState outputState = {
  true, false, false
};

//============================================================
// 8. KHAI BAO QUEUE
//============================================================

QueueHandle_t dataQueue;
QueueHandle_t dataAlertQueue;
QueueHandle_t cmdQueue;

//============================================================
// 9. HAM DOC TRANG THAI DUNG CHO MQTT
//============================================================

OutputState getOutputState() {

  OutputState tempState;

  portENTER_CRITICAL(&stateMux);

  tempState = outputState;

  portEXIT_CRITICAL(&stateMux);

  return tempState;
}

//============================================================
// 10. HAM DUA LENH VAO QUEUE
//============================================================

void enqueueCommand(ControlCommand cmd) {

  if (xQueueSend(cmdQueue, &cmd, 0) != pdPASS) {

    // Neu Queue day, loai bo lenh cu nhat
    ControlCommand oldCommand;

    xQueueReceive(cmdQueue, &oldCommand, 0);

    xQueueSend(cmdQueue, &cmd, 0);
  }
}

//============================================================
// 11. NHAN LENH TU MQTT
//============================================================

void mqttCallback(
  char* topic,
  byte* payload,
  unsigned int length
) {

  char message[20];

  if (length >= sizeof(message)) {
    length = sizeof(message) - 1;
  }

  memcpy(message, payload, length);
  message[length] = '\0';

  ControlCommand cmd;

  // Lenh chuyen che do AUTO / MANUAL
  if (strcmp(topic, TOPIC_CMD_MODE) == 0) {

    if (strcmp(message, "AUTO") == 0) {

      cmd.type = CMD_SET_MODE;
      cmd.value = true;

      enqueueCommand(cmd);

    }
    else if (strcmp(message, "MANUAL") == 0) {

      cmd.type = CMD_SET_MODE;
      cmd.value = false;

      enqueueCommand(cmd);
    }
  }

  // Lenh dieu khien quat
  else if (strcmp(topic, TOPIC_CMD_FAN) == 0) {

    if (strcmp(message, "ON") == 0 ||
        strcmp(message, "OFF") == 0) {

      cmd.type = CMD_SET_FAN;
      cmd.value = (strcmp(message, "ON") == 0);

      enqueueCommand(cmd);
    }
  }

  // Lenh dieu khien may bom
  else if (strcmp(topic, TOPIC_CMD_PUMP) == 0) {

    if (strcmp(message, "ON") == 0 ||
        strcmp(message, "OFF") == 0) {

      cmd.type = CMD_SET_PUMP;
      cmd.value = (strcmp(message, "ON") == 0);

      enqueueCommand(cmd);
    }
  }
}

//============================================================
// 12. TASK DOC DU LIEU CAM BIEN - CORE 1
//============================================================

void TaskReadData(void* parameter) {

  SensorData data;

  uint32_t sampleCount = 0;

  Serial.println("[READ] TaskReadData started - Core 1");

  while (true) {

    // Doc gia tri ADC tu MQ2
    int gasValue = analogRead(MQ2_PIN);

    // Doc nhiet do tu DHT22
    float temperature = dht.readTemperature();

    // Kiem tra loi doc DHT22
    if (isnan(temperature)) {

      Serial.println("[READ] Loi doc cam bien DHT22!");

      vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));

      continue;
    }

    // Dong goi du lieu
    data.gasADC = gasValue;
    data.temperature = temperature;
    data.sampleNumber = ++sampleCount;

    // Hien thi Serial Monitor
    Serial.println();
    Serial.println("========== DU LIEU CAM BIEN ==========");

    Serial.print("Lan doc: ");
    Serial.println(data.sampleNumber);

    Serial.print("Muc khi gas (ADC): ");
    Serial.println(data.gasADC);

    Serial.print("Nhiet do: ");
    Serial.print(data.temperature);
    Serial.println(" *C");

    Serial.println("=====================================");

    // Gui du lieu vao dataQueue
    // Queue co 1 phan tu, luon giu mau moi nhat
    xQueueOverwrite(dataQueue, &data);

    // Gui du lieu vao dataAlertQueue
    xQueueOverwrite(dataAlertQueue, &data);

    // Chu ky doc 1 giay
    vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
  }
}

//============================================================
// 13. TASK CANH BAO - CORE 1
//============================================================

void TaskAlert(void* parameter) {

  SensorData latestData;

  ControlCommand cmd;

  bool hasData = false;

  // Mac dinh khoi dong o che do tu dong
  bool autoMode = true;

  // Trang thai khi su dung Manual
  bool manualFan = false;
  bool manualPump = false;

  bool fanOutput = false;
  bool pumpOutput = false;

  Serial.println("[ALERT] TaskAlert started - Core 1");

  while (true) {

    // Doc du lieu moi nhat tu dataAlertQueue
    if (xQueueReceive(
          dataAlertQueue,
          &latestData,
          pdMS_TO_TICKS(50)
        ) == pdTRUE) {

      hasData = true;
    }

    // Kiem tra cac lenh dieu khien tu Web
    while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {

      switch (cmd.type) {

        case CMD_SET_MODE:

          autoMode = cmd.value;

          if (autoMode) {
            Serial.println("[ALERT] CHUYEN SANG AUTO");
          }
          else {
            Serial.println("[ALERT] CHUYEN SANG MANUAL");
          }

          break;

        case CMD_SET_FAN:

          if (!autoMode) {

            manualFan = cmd.value;

            Serial.println("[ALERT] Nhan lenh dieu khien QUAT");
          }
          else {

            Serial.println(
              "[ALERT] Hay chuyen MANUAL truoc khi dieu khien quat"
            );
          }

          break;

        case CMD_SET_PUMP:

          if (!autoMode) {

            manualPump = cmd.value;

            Serial.println("[ALERT] Nhan lenh dieu khien MAY BOM");
          }
          else {

            Serial.println(
              "[ALERT] Hay chuyen MANUAL truoc khi dieu khien may bom"
            );
          }

          break;
      }
    }

    // Kiem tra dieu kien canh bao
    bool gasAlarm = false;
    bool tempAlarm = false;

    if (hasData) {

      gasAlarm =
        latestData.gasADC > GAS_THRESHOLD_ADC;

      tempAlarm =
        latestData.temperature >= TEMP_THRESHOLD_C;
    }

    // Dieu khien theo che do AUTO / MANUAL
    if (autoMode) {

      fanOutput = gasAlarm;

      pumpOutput = tempAlarm;
    }
    else {

      fanOutput = manualFan;

      pumpOutput = manualPump;
    }

    // Dieu khien LED dai dien cho quat
    digitalWrite(FAN_LED, fanOutput ? HIGH : LOW);

    // Dieu khien LED dai dien cho may bom
    digitalWrite(PUMP_LED, pumpOutput ? HIGH : LOW);

    // Cap nhat trang thai dung chung
    portENTER_CRITICAL(&stateMux);

    outputState.autoMode = autoMode;
    outputState.fanOn = fanOutput;
    outputState.pumpOn = pumpOutput;

    portEXIT_CRITICAL(&stateMux);

    // Hien thi khi thay doi trang thai quat
    static bool oldFan = false;
    static bool oldPump = false;

    if (fanOutput != oldFan) {

      Serial.print("[ALERT] QUAT: ");

      Serial.println(fanOutput ? "BAT" : "TAT");

      oldFan = fanOutput;
    }

    if (pumpOutput != oldPump) {

      Serial.print("[ALERT] MAY BOM: ");

      Serial.println(pumpOutput ? "BAT" : "TAT");

      oldPump = pumpOutput;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

//============================================================
// 14. TASK TRUYEN DU LIEU MQTT - CORE 0
//============================================================

void TaskSendData(void* parameter) {

  bool wifiStarted = false;
  bool mqttAttempted = false;

  unsigned long lastWiFiAttempt = 0;
  unsigned long lastMQTTAttempt = 0;

  unsigned long lastStatePublish = 0;

  // Tao Client ID rieng cho ESP32
  char clientID[40];

  uint32_t chipID =
    (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);

  snprintf(
    clientID,
    sizeof(clientID),
    "FireAlarm_%08X",
    chipID
  );

  Serial.println("[MQTT] TaskSendData started - Core 0");

  // Bat WiFi o Task nay
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  while (true) {

    unsigned long now = millis();

    //========================================================
    // KET NOI WIFI
    //========================================================

    if (WiFi.status() != WL_CONNECTED) {

      if (mqtt.connected()) {
        mqtt.disconnect();
      }

      if (!wifiStarted ||
          now - lastWiFiAttempt >= 5000) {

        Serial.println("[WIFI] Dang ket noi WiFi...");

        WiFi.begin(WIFI_SSID, WIFI_PASS);

        wifiStarted = true;

        lastWiFiAttempt = now;
      }

      vTaskDelay(pdMS_TO_TICKS(100));

      continue;
    }

    //========================================================
    // WIFI DA KET NOI
    //========================================================

    if (wifiStarted && lastWiFiAttempt != 0) {

      static bool wifiReported = false;

      if (!wifiReported) {

        Serial.println("[WIFI] Ket noi thanh cong!");

        Serial.print("[WIFI] IP: ");

        Serial.println(WiFi.localIP());

        wifiReported = true;
      }
    }

    //========================================================
    // KET NOI MQTT BROKER
    //========================================================

    if (!mqtt.connected()) {

      if (!mqttAttempted ||
          now - lastMQTTAttempt >= 5000) {

        Serial.println("[MQTT] Dang ket noi Broker...");

        if (mqtt.connect(clientID)) {

          Serial.println("[MQTT] Ket noi thanh cong!");

          // Dang ky nhan lenh tu Web
          mqtt.subscribe(TOPIC_CMD_FAN);

          mqtt.subscribe(TOPIC_CMD_PUMP);

          mqtt.subscribe(TOPIC_CMD_MODE);

          lastStatePublish = 0;
        }
        else {

          Serial.print("[MQTT] Loi ket noi, ma loi: ");

          Serial.println(mqtt.state());
        }

        mqttAttempted = true;

        lastMQTTAttempt = millis();
      }
    }

    //========================================================
    // GUI DU LIEU LEN MQTT BROKER
    //========================================================

    if (mqtt.connected()) {

      // Xu ly cac ban tin MQTT nhan duoc
      mqtt.loop();

      SensorData receivedData;

      // Doc du lieu tu dataQueue
      if (xQueueReceive(
            dataQueue,
            &receivedData,
            0
          ) == pdTRUE) {

        char gasString[16];

        char tempString[20];

        snprintf(
          gasString,
          sizeof(gasString),
          "%u",
          receivedData.gasADC
        );

        snprintf(
          tempString,
          sizeof(tempString),
          "%.2f",
          receivedData.temperature
        );

        // Gui gia tri khi gas
        mqtt.publish(
          TOPIC_GAS,
          gasString,
          true
        );

        // Gui nhiet do
        mqtt.publish(
          TOPIC_TEMP,
          tempString,
          true
        );

        // Xac dinh trang thai canh bao
        bool gasAlarm =
          receivedData.gasADC > GAS_THRESHOLD_ADC;

        bool tempAlarm =
          receivedData.temperature >= TEMP_THRESHOLD_C;

        const char* alarmStatus = "NORMAL";

        if (gasAlarm && tempAlarm) {
          alarmStatus = "BOTH";
        }
        else if (gasAlarm) {
          alarmStatus = "GAS";
        }
        else if (tempAlarm) {
          alarmStatus = "TEMP";
        }

        mqtt.publish(
          TOPIC_ALARM,
          alarmStatus,
          true
        );

        // Hien thi ket qua gui MQTT
        Serial.println("[MQTT] Da gui du lieu:");

        Serial.print("Gas ADC: ");
        Serial.println(gasString);

        Serial.print("Temperature: ");
        Serial.println(tempString);

        Serial.print("Alarm: ");
        Serial.println(alarmStatus);
      }

      //======================================================
      // GUI TRANG THAI QUAT, BOM, MODE
      //======================================================

      if (millis() - lastStatePublish >= 1000) {

        OutputState state = getOutputState();

        mqtt.publish(
          TOPIC_FAN,
          state.fanOn ? "ON" : "OFF",
          true
        );

        mqtt.publish(
          TOPIC_PUMP,
          state.pumpOn ? "ON" : "OFF",
          true
        );

        mqtt.publish(
          TOPIC_MODE,
          state.autoMode ? "AUTO" : "MANUAL",
          true
        );

        lastStatePublish = millis();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

//============================================================
// 15. HAM SETUP
//============================================================

void setup() {

  Serial.begin(115200);

  delay(500);

  Serial.println();

  Serial.println("======================================");

  Serial.println("       FIRE ALARM ESP32 SYSTEM");

  Serial.println("======================================");

  // Cau hinh chan GPIO
  pinMode(MQ2_PIN, INPUT);

  pinMode(FAN_LED, OUTPUT);

  pinMode(PUMP_LED, OUTPUT);

  // Tat LED luc khoi dong
  digitalWrite(FAN_LED, LOW);

  digitalWrite(PUMP_LED, LOW);

  // Cau hinh ADC 12 bit
  analogReadResolution(12);

  analogSetPinAttenuation(MQ2_PIN, ADC_11db);

  // Khoi dong DHT22
  dht.begin();

  // Cau hinh MQTT
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);

  mqtt.setCallback(mqttCallback);

  mqtt.setBufferSize(256);

  mqtt.setKeepAlive(30);

  //==========================================================
  // TAO QUEUE
  //==========================================================

  // Queue 1 phan tu, luu mau du lieu moi nhat
  dataQueue = xQueueCreate(
    1,
    sizeof(SensorData)
  );

  dataAlertQueue = xQueueCreate(
    1,
    sizeof(SensorData)
  );

  // Queue nhan lenh dieu khien tu Web
  cmdQueue = xQueueCreate(
    5,
    sizeof(ControlCommand)
  );

  if (dataQueue == NULL ||
      dataAlertQueue == NULL ||
      cmdQueue == NULL) {

    Serial.println("[ERROR] Khong tao duoc Queue!");

    while (true) {
      delay(1000);
    }
  }

  Serial.println("[SYSTEM] Tao Queue thanh cong!");

  //==========================================================
  // TAO 3 TASK FREERTOS
  //==========================================================

  // Task doc cam bien - Core 1
  BaseType_t task1 = xTaskCreatePinnedToCore(
    TaskReadData,
    "TaskReadData",
    4096,
    NULL,
    2,
    NULL,
    1
  );

  // Task gui MQTT - Core 0
  BaseType_t task2 = xTaskCreatePinnedToCore(
    TaskSendData,
    "TaskSendData",
    8192,
    NULL,
    1,
    NULL,
    0
  );

  // Task canh bao - Core 1
  BaseType_t task3 = xTaskCreatePinnedToCore(
    TaskAlert,
    "TaskAlert",
    4096,
    NULL,
    2,
    NULL,
    1
  );

  if (task1 != pdPASS ||
      task2 != pdPASS ||
      task3 != pdPASS) {

    Serial.println("[ERROR] Tao Task that bai!");

  }
  else {

    Serial.println("[SYSTEM] Khoi tao 3 Task thanh cong!");

    Serial.println("[SYSTEM] TaskReadData : Core 1");

    Serial.println("[SYSTEM] TaskSendData : Core 0");

    Serial.println("[SYSTEM] TaskAlert    : Core 1");
  }
}

//============================================================
// 16. HAM LOOP
//============================================================

void loop() {

  // Tat ca cong viec da duoc xu ly boi FreeRTOS
  vTaskDelay(pdMS_TO_TICKS(1000));
}
