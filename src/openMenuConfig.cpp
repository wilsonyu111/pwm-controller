#include "openMenuConfig.h"

#include <Arduino.h>
#include <PCF8575.h>
#include "OpenMenuOS.h"
#include "TCA9548.h"
#include "data_structure.h"
#include "input_bridge.h"

namespace {
constexpr uint8_t PCF8575_MUX_CHANNEL = 1;
constexpr uint8_t PCF_BTN_UP = 14;
constexpr uint8_t PCF_BTN_DOWN = 13;
constexpr uint8_t PCF_BTN_SELECT = 12;

constexpr uint8_t ROT_ENCODER_CLK = 35;
constexpr uint8_t ROT_ENCODER_DT = 34;

constexpr uint8_t TEMPERATURE_TARGET_MIN = 5;
constexpr uint8_t TEMPERATURE_TARGET_MAX = 60;
constexpr uint8_t PWM_TARGET_MIN = 0;
constexpr uint8_t PWM_TARGET_MAX = 255;

constexpr int FAN_MODE_MANUAL = 0;
constexpr int FAN_MODE_STANDARD = 1;

const char* modeOptions[] = {
    "Manual",
    "Standard"
};

const char* fanTempChannelOptions[] = {
    "ch0",
    "ch2",
    "ch3",
    "ch4"
};
} // namespace

extern TCA9548 mux;
extern PCF8575 pcf8575;

extern int temperaturePoint;
extern int currentPwmPercentage;
extern uint8_t currentPwmValue;
extern int fanMode;
extern unsigned int fanTempChannelIdx;

extern void saveTemperaturePoint();
extern void savePwmPercentage();
extern void saveFanMode();
extern void saveFanTempChannel();
extern void setFanPWM(uint8_t pwmValue);
extern uint8_t getPwmPercentage(uint8_t pwmValue);

OpenMenuOS menu(
    InputBridge::VIRTUAL_BTN_UP,
    InputBridge::VIRTUAL_BTN_DOWN,
    InputBridge::VIRTUAL_BTN_SELECT
);

MenuScreen mainMenu("Main");
SettingsScreen targetTemperatureMenu("target_temperature");
SettingsScreen pwmTargetMenu("pwm_target");
SettingsScreen modeMenu("mode");
SettingsScreen fanTempChannelMenu("fan_temp_channel");

InputBridge inputBridge(
    mux,
    pcf8575,
    PCF8575_MUX_CHANNEL,
    PCF_BTN_UP,
    PCF_BTN_DOWN,
    PCF_BTN_SELECT,
    true
);

static int lastTemperaturePoint = -1;
static int lastPwmValue = -1;
static int lastFanMode = -1;
static int lastFanTempChannel = -1;

void setupOpenMenu() {
    targetTemperatureMenu.addRangeSetting(
        "target_temperature",
        TEMPERATURE_TARGET_MIN,
        TEMPERATURE_TARGET_MAX,
        temperaturePoint,
        " C"
    );

    pwmTargetMenu.addRangeSetting(
        "pwm_target",
        PWM_TARGET_MIN,
        PWM_TARGET_MAX,
        currentPwmValue,
        ""
    );

    modeMenu.addOptionSetting(
        "mode",
        modeOptions,
        2,
        fanMode
    );

    fanTempChannelMenu.addOptionSetting(
        "fan_temp_channel",
        fanTempChannelOptions,
        SHT41_COUNT,
        fanTempChannelIdx
    );

    mainMenu.addItem("target_temperature", &targetTemperatureMenu);
    mainMenu.addItem("pwm_target", &pwmTargetMenu);
    mainMenu.addItem("mode", &modeMenu);
    mainMenu.addItem("fan_temp_channel", &fanTempChannelMenu);

    inputBridge.begin();

    menu.setButtonsMode("low");
    menu.setButtonReadProvider([](int pin) -> int {
        return inputBridge.readVirtualPin(pin);
    });
    menu.setEncoderPin(ROT_ENCODER_CLK, ROT_ENCODER_DT);
    menu.begin(&mainMenu);

    lastTemperaturePoint = temperaturePoint;
    lastPwmValue = currentPwmValue;
    lastFanMode = fanMode;
    lastFanTempChannel = fanTempChannelIdx;
}

void pollOpenMenu() {
    inputBridge.poll();
    PopupManager::update();
    menu.loop();
    syncOpenMenuSettings();
}

void syncOpenMenuSettings() {
    int nextTemperaturePoint =
        targetTemperatureMenu.getSettingValue("target_temperature");
    int nextPwmValue = pwmTargetMenu.getSettingValue("pwm_target");
    int nextFanMode = modeMenu.getSettingValue("mode");
    int nextFanTempChannel =
        fanTempChannelMenu.getSettingValue("fan_temp_channel");

    if (nextTemperaturePoint != lastTemperaturePoint) {
        temperaturePoint = nextTemperaturePoint;
        saveTemperaturePoint();
        PopupManager::showSuccess("Saved", "target_temperature");
        lastTemperaturePoint = nextTemperaturePoint;
    }

    if (nextPwmValue != lastPwmValue) {
        currentPwmValue = static_cast<uint8_t>(nextPwmValue);
        currentPwmPercentage = getPwmPercentage(currentPwmValue);

        // Existing persistence is percent-based; keep runtime PWM exact.
        savePwmPercentage();
        currentPwmValue = static_cast<uint8_t>(nextPwmValue);
        if (fanMode == FAN_MODE_MANUAL) {
            setFanPWM(currentPwmValue);
        }

        PopupManager::showSuccess("Saved", "pwm_target");
        lastPwmValue = nextPwmValue;
    }

    if (nextFanMode != lastFanMode) {
        fanMode = nextFanMode;
        saveFanMode();
        PopupManager::showSuccess("Saved", "mode");
        lastFanMode = nextFanMode;
    }

    if (nextFanTempChannel != lastFanTempChannel) {
        fanTempChannelIdx = static_cast<unsigned int>(nextFanTempChannel);
        saveFanTempChannel();
        PopupManager::showSuccess("Saved", "fan_temp_channel");
        lastFanTempChannel = nextFanTempChannel;
    }
}
