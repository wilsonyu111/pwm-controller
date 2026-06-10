#include <Wire.h>
#include <Adafruit_SHT4x.h>
#include "TCA9548.h"
#include <PCF8575.h>
#include "main.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <cstdint>
#include <algorithm>
#include "webserver.h"
#include "data_structure.h"
#include "openMenuConfig.h"
#include <arduino.h>
#include <Preferences.h>

// ---- PIN DEFINITIONS ----
#define SDA_PIN 21
#define SCL_PIN 22
#define ROT_S1_PIN 35
#define ROT_S2_PIN 34
#define ROT_BUTTON_PIN 32
#define FAN1_PIN 33
#define FAN2_PIN 25
#define FAN1_CHANNEL 1
#define FAN2_CHANNEL 2
#define PCF8575_MUX_CHANNEL 1
#define PWM_PERCENTAGE_MIN 0
#define PWM_PERCENTAGE_MAX 100
#define PWM_MIN 0
#define PWM_MAX 255
#define TEMPERATURE_POINT_MIN 5
#define TEMPERATURE_POINT_MAX 60
#define PCF_PIN_COUNT 4
#define MUX_ADDRESS 0x70
#define PCF8575_ADDRESS 0x20
#define SCAN_INTERVAL_MS 5000
#define PCF_BUTTON_COUNT 4
#define DEFAULT_PWM_PERCENTAGE 30
#define DEFAULT_TEMPERATURE_POINT 25
#define PWM_CHANGE_STEP 10
#define FAN_MODE_MANUAL 0
#define FAN_MODE_STANDARD 1

const uint8_t PCF_BUTTON_PINS[4] = {12, 13, 14, 15};

bool wifiConnected = false;
int currentPwmPercentage = DEFAULT_PWM_PERCENTAGE;
uint8_t currentPwmValue = PWM_MIN;
int temperaturePoint = DEFAULT_TEMPERATURE_POINT;
int fanMode = FAN_MODE_STANDARD;
unsigned int fanTempChannelIdx = 0;
unsigned long lastMqttPublishMillis = 0;
bool pwmReadingError = false;
String wifiSSID = "";
String wifiPassword = "";
String mqttBrokerIP = "";
uint16_t mqttBrokerPort = 1883;
String mqttClientID = "";
String mqttUsername = "";
String mqttPassword = "";
String mqttTopicBase = "";

Preferences appPreferences;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

void loadPreferences();
void savePwmPercentage();
void saveTemperaturePoint();
void saveFanTempChannel();
void saveFanMode();
void applyFanControl(int pwmPct, int fanModeParam, int channelIdx, int tempTarget);
void publishSensorReadingsToMQTT(const SensorData& sensorData);
void calculateFanPWM(float tempTarget, float currentTemp, float tempAllowance);
void setFanPWM(uint8_t pwmValue);
uint8_t getPwmPercentage(uint8_t pwmValue);
SensorReading getSHT41Reading(uint8_t sensorIndex);

float temperatureAllowance = 2.0;

TCA9548 mux(MUX_ADDRESS);
PCF8575 pcf8575(PCF8575_ADDRESS);

void calculateFanPWM(float tempTarget, float currentTemp, float tempAllowance) {
    static float prevTemp = -1;
    if (prevTemp == -1) {
        prevTemp = currentTemp;
        return;
    }

    if (abs(currentTemp - tempTarget) <= tempAllowance) return;

    if (currentTemp > tempTarget) {
        int nextPwm = currentPwmValue + PWM_CHANGE_STEP;
        currentPwmValue = (uint8_t)std::min(nextPwm, PWM_MAX);
    } else if (currentTemp < tempTarget) {
        if (currentPwmValue >= PWM_CHANGE_STEP) {
            currentPwmValue = currentPwmValue - PWM_CHANGE_STEP;
        } else {
            currentPwmValue = (uint8_t)PWM_MIN;
        }
    }

    setFanPWM(currentPwmValue);
    prevTemp = currentTemp;
}

void setFanPWM(uint8_t pwmValue) {
    pwmValue = std::max((uint8_t)PWM_MIN, std::min(pwmValue, (uint8_t)PWM_MAX));
    ledcWrite(FAN1_CHANNEL, pwmValue);
    ledcWrite(FAN2_CHANNEL, pwmValue);
}

void scanSHT41Sensors(SensorReading readings[]) {
    for (uint8_t idx = 0; idx < SHT41_COUNT; idx++) {
        readings[idx] = getSHT41Reading(idx);
    }
}

SensorReading getSHT41Reading(uint8_t index) {
    SensorReading reading;
    uint8_t channel = SHT41_PINS[index];
    mux.selectChannel(channel);
    delay(50);

    Adafruit_SHT4x sht4 = Adafruit_SHT4x();
    float tval = -1.0, hval = -1.0;

    if (sht4.begin()) {
        sensors_event_t humidity, temp;
        if (sht4.getEvent(&humidity, &temp)) {
            tval = temp.temperature;
            hval = humidity.relative_humidity;
        }
    }

    reading.temperature = tval;
    reading.humidity = hval;
    return reading;
}

void sht41Task(SensorReading readings[]) {
    scanSHT41Sensors(readings);

    if (fanMode == FAN_MODE_STANDARD) {
        if (readings[fanTempChannelIdx].temperature < 0) {
            pwmReadingError = true;
        } else {
            calculateFanPWM(temperaturePoint, readings[fanTempChannelIdx].temperature, temperatureAllowance);
            pwmReadingError = false;
        }
    } else {
        pwmReadingError = false;
    }

    SensorData sensorData;
    for (uint8_t idx = 0; idx < SHT41_COUNT; idx++) {
        sensorData.readings[idx] = readings[idx];
    }
    sensorData.currentPwmValue = currentPwmValue;
    sensorData.targetPwmPercentage = currentPwmPercentage;
    sensorData.currentPwmPercentage = getPwmPercentage(currentPwmValue);
    sensorData.temperatureTarget = (uint8_t)temperaturePoint;
    sensorData.fanTempChannelIdx = fanTempChannelIdx;
    sensorData.fanMode = fanMode;

    publishSensorReadingsToMQTT(sensorData);
    broadcastPwmUpdate(sensorData);
}

uint8_t getPwmPercentage(uint8_t pwmValue) {
    return (pwmValue * 100) / 255;
}

void publishSensorReadingsToMQTT(const SensorData& sensorData) {
    if (WiFi.status() != WL_CONNECTED) {
        wifiConnected = false;
        return;
    }
    wifiConnected = true;

    if (!mqttClient.connected()) {
        if (!mqttClient.connect(mqttClientID.c_str(), mqttUsername.c_str(), mqttPassword.c_str())) {
            Serial.println("MQTT connection failed");
            return;
        }
    }

    if (mqttTopicBase.length() == 0) {
        return;
    }

    String payload = buildSensorDataJson(sensorData);
    String topic = mqttTopicBase + "/state";
    mqttClient.publish(topic.c_str(), payload.c_str());

    lastMqttPublishMillis = millis();
    mqttClient.loop();
}

void loadPreferences() {
    appPreferences.begin("settings", true);
    wifiSSID = appPreferences.getString("ssid", "");
    wifiPassword = appPreferences.getString("pass", "");
    temperatureAllowance = appPreferences.getFloat("tempAllowance", 2.0);
    currentPwmPercentage = appPreferences.getInt("pwmPct", PWM_PERCENTAGE_MIN);
    currentPwmValue = (currentPwmPercentage * 255) / 100;
    temperaturePoint = appPreferences.getInt("tempPt", TEMPERATURE_POINT_MIN);
    fanMode = appPreferences.getInt("fanMode", FAN_MODE_STANDARD);
    fanTempChannelIdx = appPreferences.getUChar("fanTempChIdx", 0);
    mqttBrokerIP = appPreferences.getString("broker_ip", "");
    mqttBrokerPort = appPreferences.getString("port", "1883").toInt();
    mqttClientID = appPreferences.getString("client_id", "");
    mqttUsername = appPreferences.getString("username", "");
    mqttPassword = appPreferences.getString("password", "");
    mqttTopicBase = appPreferences.getString("topic_base", "");
    appPreferences.end();
}

void savePwmPercentage() {
    appPreferences.begin("settings", false);
    appPreferences.putInt("pwmPct", currentPwmPercentage);
    appPreferences.end();
    if (fanMode == FAN_MODE_MANUAL) {
        currentPwmValue = (currentPwmPercentage * 255) / 100;
        setFanPWM(currentPwmValue);
    }
    Serial.println("saved pwm percentage");
}

void saveTemperaturePoint() {
    appPreferences.begin("settings", false);
    appPreferences.putInt("tempPt", temperaturePoint);
    appPreferences.end();
    Serial.println("saved temperature point");
}

void saveFanTempChannel() {
    appPreferences.begin("settings", false);
    appPreferences.putUChar("fanTempChIdx", fanTempChannelIdx);
    appPreferences.end();
    Serial.println("saved fan temp channel");
}

void saveFanMode() {
    appPreferences.begin("settings", false);
    appPreferences.putInt("fanMode", fanMode);
    appPreferences.end();
    if (fanMode == FAN_MODE_MANUAL) {
        currentPwmValue = (currentPwmPercentage * 255) / 100;
        setFanPWM(currentPwmValue);
    }
    Serial.println("saved fan mode");
}

void applyFanControl(int pwmPct, int fanModeParam, int channelIdx, int tempTarget) {
    if (pwmPct >= PWM_PERCENTAGE_MIN && pwmPct <= PWM_PERCENTAGE_MAX) {
        currentPwmPercentage = pwmPct;
        savePwmPercentage();
    }
    if (fanModeParam == FAN_MODE_MANUAL || fanModeParam == FAN_MODE_STANDARD) {
        fanMode = fanModeParam;
        saveFanMode();
    }
    if (channelIdx >= 0 && (unsigned int)channelIdx < SHT41_COUNT) {
        fanTempChannelIdx = channelIdx;
        saveFanTempChannel();
    }
    if (tempTarget >= TEMPERATURE_POINT_MIN && tempTarget <= TEMPERATURE_POINT_MAX) {
        temperaturePoint = tempTarget;
        saveTemperaturePoint();
    }
}

void setup() {
    Serial.begin(9600);

    loadPreferences();

    if (wifiSSID.length() > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
        Serial.print("Connecting to WiFi: ");
        Serial.print(wifiSSID);

        int wifiTimeout = 0;
        while (WiFi.status() != WL_CONNECTED && wifiTimeout < 30) {
            delay(500);
            Serial.print(".");
            wifiTimeout++;
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            wifiConnected = true;
            Serial.print("WiFi connected. IP: ");
            Serial.println(WiFi.localIP());

            initWebServer(80);
            startWebServer();

            mqttClient.setBufferSize(512);
            mqttClient.setServer(mqttBrokerIP.c_str(), mqttBrokerPort);
            if (mqttClient.connect(mqttClientID.c_str(), mqttUsername.c_str(), mqttPassword.c_str())) {
                Serial.println("MQTT connected");
            } else {
                Serial.println("MQTT connection failed");
            }
        } else {
            wifiConnected = false;
            Serial.println("WiFi connection failed! Starting Access Point mode...");
            startApServer();
        }
    } else {
        wifiConnected = false;
        Serial.println("No WiFi credentials found in Preferences. Starting Access Point mode...");
        startApServer();
    }

    ledcSetup(FAN1_CHANNEL, 5000, 8);
    ledcAttachPin(FAN1_PIN, FAN1_CHANNEL);
    ledcSetup(FAN2_CHANNEL, 5000, 8);
    ledcAttachPin(FAN2_PIN, FAN2_CHANNEL);
    setFanPWM(currentPwmValue);

    Wire.begin(SDA_PIN, SCL_PIN);

    if (!mux.begin(0x00)) {
        Serial.println("FATAL: PCA9548A not found");
        while (1);
    }

    mux.selectChannel(PCF8575_MUX_CHANNEL);
    delay(5);
    if (!pcf8575.begin()) {
        Serial.println("FATAL: PCF8575 not found on mux channel 1 (addr 0x20). Check wiring/address.");
        while (1);
    }

    for (uint8_t idx = 0; idx < PCF_BUTTON_COUNT; idx++) {
        uint8_t pin = PCF_BUTTON_PINS[idx];
        pcf8575.write(pin, HIGH);
    }

    setupOpenMenu();

    Serial.println("controller v1.0 ready");
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        mqttClient.loop();
    } else {
        wifiConnected = false;
    }

    pollOpenMenu();

    static unsigned long lastScanMillis = 0;
    unsigned long now = millis();
    if (now - lastScanMillis >= SCAN_INTERVAL_MS) {
        SensorReading readings[SHT41_COUNT];
        sht41Task(readings);
        lastScanMillis = now;
    }
}
