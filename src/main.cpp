#include <Wire.h>
#include <Adafruit_SHT4x.h>
#include "TCA9548.h"
#include <PCF8575.h>
#include "Rotary/Rotary.h"
#include "main.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <cstdint>
#include <algorithm>
#include "webserver.h"
#include "data_structure.h"
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
#define TEMPERATURE_SCREEN_INDEX 0
#define SET_PWM_SCREEN_INDEX 1
#define SET_TEMPERATURE_SCREEN_INDEX 2
#define SET_FAN_MODE_SCREEN_INDEX 3
#define SCREEN_COUNT 4
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
#define PCF_POLL_INTERVAL_MS 10
#define DEFAULT_PWM_PERCENTAGE 30
#define DEFAULT_TEMPERATURE_POINT 25
#define LONG_PRESS_MS 3000
#define PWM_CHANGE_STEP 10
#define FAN_MODE_MANUAL 0
#define FAN_MODE_STANDARD 1

const uint8_t PCF_BUTTON_PINS[4] = {12, 13, 14, 15};

bool wifiConnected = false;
int lastRotaryDirection = 0;
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

Preferences preferences;
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

int lastPcfButtonState[PCF_BUTTON_COUNT];
int currentScreen = 0;
int lastRotButtonState = HIGH;
unsigned long buttonPressStartTime = 0;
bool buttonLongPressHandled = false;
float targetTemperature = 25.0;
float temperatureAllowance = 2.0;
long rotaryCounter = 0;

Rotary rotary(ROT_S1_PIN, ROT_S2_PIN);
TCA9548 mux(MUX_ADDRESS);
PCF8575 pcf8575(PCF8575_ADDRESS);

static portMUX_TYPE rotaryIsrMux = portMUX_INITIALIZER_UNLOCKED;
static volatile int32_t rotaryPendingSteps = 0;

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

void IRAM_ATTR handleRotaryChange() {
    unsigned char result = rotary.process();
    if (result == DIR_CW) {
        portENTER_CRITICAL_ISR(&rotaryIsrMux);
        rotaryPendingSteps++;
        portEXIT_CRITICAL_ISR(&rotaryIsrMux);
    } else if (result == DIR_CCW) {
        portENTER_CRITICAL_ISR(&rotaryIsrMux);
        rotaryPendingSteps--;
        portEXIT_CRITICAL_ISR(&rotaryIsrMux);
    }
}

void serviceRotary() {
    int32_t delta = 0;
    portENTER_CRITICAL(&rotaryIsrMux);
    delta = rotaryPendingSteps;
    rotaryPendingSteps = 0;
    portEXIT_CRITICAL(&rotaryIsrMux);

    lastRotaryDirection = 0;
    int currentScreenIndex = currentScreen;

    if (delta > 0) {
        if (currentScreenIndex == SET_PWM_SCREEN_INDEX) {
            if (currentPwmPercentage + 1 > PWM_PERCENTAGE_MAX) return;
            currentPwmPercentage++;
        } else if (currentScreenIndex == SET_TEMPERATURE_SCREEN_INDEX) {
            if (temperaturePoint + 1 > TEMPERATURE_POINT_MAX) return;
            temperaturePoint++;
        } else if (currentScreenIndex == SET_FAN_MODE_SCREEN_INDEX) {
            fanMode = FAN_MODE_STANDARD;
        } else if (currentScreenIndex == TEMPERATURE_SCREEN_INDEX) {
            if (fanTempChannelIdx + 1 < SHT41_COUNT) {
                fanTempChannelIdx++;
            }
        }
        rotaryCounter += delta;
        lastRotaryDirection = 1;
        Serial.print("rotary CW -> ");
        Serial.println(rotaryCounter);
    } else if (delta < 0) {
        if (currentScreenIndex == SET_PWM_SCREEN_INDEX) {
            if (currentPwmPercentage - 1 < PWM_PERCENTAGE_MIN) return;
            currentPwmPercentage--;
        } else if (currentScreenIndex == SET_TEMPERATURE_SCREEN_INDEX) {
            if (temperaturePoint - 1 < TEMPERATURE_POINT_MIN) return;
            temperaturePoint--;
        } else if (currentScreenIndex == SET_FAN_MODE_SCREEN_INDEX) {
            fanMode = FAN_MODE_MANUAL;
        } else if (currentScreenIndex == TEMPERATURE_SCREEN_INDEX) {
            if (fanTempChannelIdx > 0) {
                fanTempChannelIdx--;
            }
        }
        rotaryCounter += delta;
        lastRotaryDirection = -1;
        Serial.print("rotary CCW -> ");
        Serial.println(rotaryCounter);
    }
}

void pollRotaryButton() {
    int reading = digitalRead(ROT_BUTTON_PIN);
    unsigned long now = millis();

    if (reading != lastRotButtonState) {
        if (reading == LOW) {
            buttonPressStartTime = now;
            buttonLongPressHandled = false;
        } else {
            unsigned long pressDuration = now - buttonPressStartTime;

            if (!buttonLongPressHandled && pressDuration < LONG_PRESS_MS) {
                int currentScreenIndex = currentScreen;

                if (currentScreenIndex == SET_PWM_SCREEN_INDEX) {
                    savePwmPercentage();
                } else if (currentScreenIndex == SET_TEMPERATURE_SCREEN_INDEX) {
                    saveTemperaturePoint();
                } else if (currentScreenIndex == SET_FAN_MODE_SCREEN_INDEX) {
                    saveFanMode();
                } else if (currentScreenIndex == TEMPERATURE_SCREEN_INDEX) {
                    saveFanTempChannel();
                }
            }

            buttonPressStartTime = 0;
        }
        lastRotButtonState = reading;
    }

    if (reading == LOW && buttonPressStartTime > 0 && !buttonLongPressHandled) {
        unsigned long pressDuration = now - buttonPressStartTime;

        if (pressDuration >= LONG_PRESS_MS) {
            buttonLongPressHandled = true;
            int currentScreenIndex = currentScreen;

            if (currentScreenIndex == SET_PWM_SCREEN_INDEX) {
                currentPwmPercentage = DEFAULT_PWM_PERCENTAGE;
                savePwmPercentage();
            } else if (currentScreenIndex == SET_TEMPERATURE_SCREEN_INDEX) {
                temperaturePoint = DEFAULT_TEMPERATURE_POINT;
                saveTemperaturePoint();
            } else if (currentScreenIndex == SET_FAN_MODE_SCREEN_INDEX) {
                fanMode = FAN_MODE_STANDARD;
                saveFanMode();
            } else if (currentScreenIndex == TEMPERATURE_SCREEN_INDEX) {
                fanTempChannelIdx = 0;
                saveFanTempChannel();
            }
        }
    }
}

void pollPcfButtons() {
    if (!mux.selectChannel(PCF8575_MUX_CHANNEL)) return;

    for (uint8_t idx = 0; idx < PCF_BUTTON_COUNT; idx++) {
        uint8_t pin = PCF_BUTTON_PINS[idx];
        int reading = pcf8575.read(pin);

        if (reading != lastPcfButtonState[idx]) {
            if (reading == HIGH) {
                if (pin == 13) {
                    currentScreen = (currentScreen + 1) % SCREEN_COUNT;
                } else if (pin == 14) {
                    currentScreen = (currentScreen - 1 + SCREEN_COUNT) % SCREEN_COUNT;
                }
                Serial.print("PCF8575 button pressed: P");
                Serial.println(pin);
                Serial.print("currentScreen: ");
                Serial.println(currentScreen);
            }
            lastPcfButtonState[idx] = reading;
        }
    }
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
    preferences.begin("settings", true);
    wifiSSID = preferences.getString("ssid", "");
    wifiPassword = preferences.getString("pass", "");
    temperatureAllowance = preferences.getFloat("tempAllowance", 2.0);
    currentPwmPercentage = preferences.getInt("pwmPct", PWM_PERCENTAGE_MIN);
    currentPwmValue = (currentPwmPercentage * 255) / 100;
    temperaturePoint = preferences.getInt("tempPt", TEMPERATURE_POINT_MIN);
    fanMode = preferences.getInt("fanMode", FAN_MODE_STANDARD);
    fanTempChannelIdx = preferences.getUChar("fanTempChIdx", 0);
    mqttBrokerIP = preferences.getString("broker_ip", "");
    mqttBrokerPort = preferences.getString("port", "1883").toInt();
    mqttClientID = preferences.getString("client_id", "");
    mqttUsername = preferences.getString("username", "");
    mqttPassword = preferences.getString("password", "");
    mqttTopicBase = preferences.getString("topic_base", "");
    preferences.end();
}

void savePwmPercentage() {
    preferences.begin("settings", false);
    preferences.putInt("pwmPct", currentPwmPercentage);
    preferences.end();
    if (fanMode == FAN_MODE_MANUAL) {
        currentPwmValue = (currentPwmPercentage * 255) / 100;
        setFanPWM(currentPwmValue);
    }
    Serial.println("saved pwm percentage");
}

void saveTemperaturePoint() {
    preferences.begin("settings", false);
    preferences.putInt("tempPt", temperaturePoint);
    preferences.end();
    Serial.println("saved temperature point");
}

void saveFanTempChannel() {
    preferences.begin("settings", false);
    preferences.putUChar("fanTempChIdx", fanTempChannelIdx);
    preferences.end();
    Serial.println("saved fan temp channel");
}

void saveFanMode() {
    preferences.begin("settings", false);
    preferences.putInt("fanMode", fanMode);
    preferences.end();
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

    pinMode(ROT_S1_PIN, INPUT);
    pinMode(ROT_S2_PIN, INPUT);
    pinMode(ROT_BUTTON_PIN, INPUT_PULLUP);
    lastRotButtonState = digitalRead(ROT_BUTTON_PIN);

    attachInterrupt(digitalPinToInterrupt(ROT_S1_PIN), handleRotaryChange, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ROT_S2_PIN), handleRotaryChange, CHANGE);

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
        lastPcfButtonState[idx] = pcf8575.read(pin);
    }

    Serial.println("controller v1.0 ready");
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        mqttClient.loop();
    } else {
        wifiConnected = false;
    }

    serviceRotary();
    pollRotaryButton();
    pollPcfButtons();

    static unsigned long lastScanMillis = 0;
    unsigned long now = millis();
    if (now - lastScanMillis >= SCAN_INTERVAL_MS) {
        SensorReading readings[SHT41_COUNT];
        sht41Task(readings);
        lastScanMillis = now;
    }
}
