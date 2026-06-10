#include "input_bridge.h"

InputBridge::InputBridge(
    TCA9548& mux,
    PCF8575& pcf8575,
    uint8_t muxChannel,
    uint8_t upPin,
    uint8_t downPin,
    uint8_t selectPin,
    bool activeHigh,
    unsigned long pollIntervalMs
) :
    mux(mux),
    pcf8575(pcf8575),
    muxChannel(muxChannel),
    upPin(upPin),
    downPin(downPin),
    selectPin(selectPin),
    activeHigh(activeHigh),
    pollIntervalMs(pollIntervalMs)
{
}

void InputBridge::begin() {
    if (!mux.selectChannel(muxChannel)) {
        return;
    }

    // PCF8575 pins must be written HIGH before they can behave like inputs.
    pcf8575.write(upPin, HIGH);
    pcf8575.write(downPin, HIGH);
    pcf8575.write(selectPin, HIGH);

    poll(true);
}

void InputBridge::poll(bool force) {
    unsigned long now = millis();
    if (!force && now - lastPollMs < pollIntervalMs) {
        return;
    }

    lastPollMs = now;

    if (!mux.selectChannel(muxChannel)) {
        return;
    }

    upPressed = readPressed(upPin);
    downPressed = readPressed(downPin);
    selectPressed = readPressed(selectPin);
}

bool InputBridge::isUpPressed() const {
    return upPressed;
}

bool InputBridge::isDownPressed() const {
    return downPressed;
}

bool InputBridge::isSelectPressed() const {
    return selectPressed;
}

int InputBridge::readVirtualPin(int pin) const {
    switch (pin) {
        case VIRTUAL_BTN_UP:
            return upPressed ? LOW : HIGH;
        case VIRTUAL_BTN_DOWN:
            return downPressed ? LOW : HIGH;
        case VIRTUAL_BTN_SELECT:
            return selectPressed ? LOW : HIGH;
        default:
            return HIGH;
    }
}

bool InputBridge::readPressed(uint8_t pcfPin) {
    int value = pcf8575.read(pcfPin);
    return activeHigh ? value == HIGH : value == LOW;
}
