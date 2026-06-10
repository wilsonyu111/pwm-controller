#ifndef INPUT_BRIDGE_H
#define INPUT_BRIDGE_H

#include <Arduino.h>
#include <PCF8575.h>
#include "TCA9548.h"

class InputBridge {
public:
    enum VirtualPin : int {
        VIRTUAL_BTN_UP = 1001,
        VIRTUAL_BTN_DOWN = 1002,
        VIRTUAL_BTN_SELECT = 1003
    };

    InputBridge(
        TCA9548& mux,
        PCF8575& pcf8575,
        uint8_t muxChannel,
        uint8_t upPin,
        uint8_t downPin,
        uint8_t selectPin,
        bool activeHigh = true,
        unsigned long pollIntervalMs = 20
    );

    void begin();
    void poll(bool force = false);

    bool isUpPressed() const;
    bool isDownPressed() const;
    bool isSelectPressed() const;

    int readVirtualPin(int pin) const;

private:
    TCA9548& mux;
    PCF8575& pcf8575;
    uint8_t muxChannel;
    uint8_t upPin;
    uint8_t downPin;
    uint8_t selectPin;
    bool activeHigh;
    unsigned long pollIntervalMs;

    bool upPressed = false;
    bool downPressed = false;
    bool selectPressed = false;
    unsigned long lastPollMs = 0;

    bool readPressed(uint8_t pcfPin);
};

#endif // INPUT_BRIDGE_H
