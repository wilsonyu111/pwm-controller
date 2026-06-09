#ifndef FINAL_TEST_H
#define FINAL_TEST_H

#include <Arduino.h>
#include <stdint.h>

#define SHT41_COUNT 4
extern const uint8_t SHT41_PINS[4];

void IRAM_ATTR handleRotaryChange();
void serviceRotary();
void pollRotaryButton();
void pollPcfButtons();
void setup();
void loop();

#endif // FINAL_TEST_H
