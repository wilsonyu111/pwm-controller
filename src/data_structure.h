#ifndef DATA_STRUCTURE_H
#define DATA_STRUCTURE_H
#define SHT41_COUNT 4
#include <Arduino.h>
#include <stdint.h>

const uint8_t SHT41_PINS[4] = {0, 2, 3, 4}; // SHT41 pins used for sensors
// SensorReading struct - used for SHT41 sensor readings
struct SensorReading {
    float temperature;
    float humidity;
};

struct SensorData {
    SensorReading readings[SHT41_COUNT];
    uint8_t currentPwmValue;
    uint8_t currentPwmPercentage;
    uint8_t targetPwmPercentage;
    uint8_t temperatureTarget;
    unsigned int fanTempChannelIdx;
    int fanMode;

};

#endif // DATA_STRUCTURE_H

