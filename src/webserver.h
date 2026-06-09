#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESP.h>
#include <Preferences.h>
#include "data_structure.h"

// Initialize the async web server
// port: TCP port to listen on (default: 80)
void initWebServer(uint16_t port = 80);

// Start the web server (call after WiFi is connected)
void startWebServer();

// Stop the web server
void stopWebServer();
void startApServer();

// Broadcast PWM update via SSE (call from sht41Task)
// sensorData: SensorData struct containing all sensor and PWM information
void broadcastPwmUpdate(SensorData sensorData);

// Build JSON payload for sensor/fan state (SSE and MQTT)
String buildSensorDataJson(const SensorData& sensorData);

// Apply fan control from web UI (-1 = leave unchanged)
void applyFanControl(int pwmPct, int fanModeParam, int channelIdx, int tempTarget);