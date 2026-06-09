#include "webserver.h"
#include "data_structure.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <ESP.h>

// Web server instance
AsyncWebServer* server = nullptr;
AsyncEventSource* events = nullptr;  // SSE event source
uint16_t serverPort = 80;
bool routesRegistered = false;
extern Preferences preferences;  // Defined in final-test.cpp

static String getDeviceIpAddress() {
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
        return WiFi.softAPIP().toString();
    }
    return "";
}

// Initialize LittleFS
bool initLittleFS() {
    if (!LittleFS.begin(true)) {  // true = format if mount fails
        Serial.println("LittleFS Mount Failed");
        return false;
    }
    Serial.println("LittleFS Mounted Successfully");
    
    // Debug: List files in LittleFS
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
        Serial.println("Files in LittleFS:");
        File file = root.openNextFile();
        while (file) {
            Serial.print("  ");
            Serial.print(file.name());
            Serial.print(" (");
            Serial.print(file.size());
            Serial.println(" bytes)");
            file = root.openNextFile();
        }
    }
    root.close();
    
    return true;
}

void initWebServer(uint16_t port) {
    serverPort = port;
    
    // Initialize LittleFS
    if (!initLittleFS()) {
        Serial.println("Failed to initialize LittleFS");
        return;
    }
    
    // Stop server if it's running
    if (server != nullptr) {
        server->end();
        delete server;
        server = nullptr;
        routesRegistered = false;
    }
    
    // Create new server instance
    server = new AsyncWebServer(serverPort);

    // Register routes only once
    if (!routesRegistered) {
        // Serve static files from LittleFS
        // Files should be in the root of LittleFS (main.html, style.css)
        server->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
            if (LittleFS.exists("/main.html")) {
                request->send(LittleFS, "/main.html", "text/html");
            } else {
                Serial.println("ERROR: main.html not found in LittleFS");
                request->send(404, "text/plain", "File not found: main.html. Please upload filesystem.");
            }
        });

        server->on("/style.css", HTTP_GET, [](AsyncWebServerRequest* request) {
            if (LittleFS.exists("/style.css")) {
                request->send(LittleFS, "/style.css", "text/css");
            } else {
                Serial.println("ERROR: style.css not found in LittleFS");
                request->send(404, "text/plain", "File not found: style.css");
            }
        });
        
        server->on("/main.html", HTTP_GET, [](AsyncWebServerRequest* request) {
            if (LittleFS.exists("/main.html")) {
                request->send(LittleFS, "/main.html", "text/html");
            } else {
                request->send(404, "text/plain", "File not found: main.html");
            }
        });

        server->on("/fan", HTTP_GET, [](AsyncWebServerRequest* request) {
            if (LittleFS.exists("/fan.html")) {
                request->send(LittleFS, "/fan.html", "text/html");
            } else {
                Serial.println("ERROR: fan.html not found in LittleFS");
                request->send(404, "text/plain", "File not found: fan.html. Please upload filesystem.");
            }
        });

        server->on("/fan_style.css", HTTP_GET, [](AsyncWebServerRequest* request) {
            if (LittleFS.exists("/fan_style.css")) {
                request->send(LittleFS, "/fan_style.css", "text/css");
            } else {
                Serial.println("ERROR: fan_style.css not found in LittleFS");
                request->send(404, "text/plain", "File not found: fan_style.css");
            }
        });

        server->on("/fan/state", HTTP_GET, [](AsyncWebServerRequest *request) {
            extern int currentPwmPercentage;
            extern int fanMode;
            extern unsigned int fanTempChannelIdx;

            String json = "{";
            json += "\"pwm_pct\":" + String(currentPwmPercentage) + ",";
            json += "\"fan_mode\":" + String(fanMode) + ",";
            json += "\"temp_channel_idx\":" + String(fanTempChannelIdx) + ",";
            json += "\"temp_channel\":" + String(SHT41_PINS[fanTempChannelIdx]);
            json += "}";
            request->send(200, "application/json", json);
        });

        server->on("/fan_control", HTTP_GET, [](AsyncWebServerRequest *request) {
            int pwmPct = -1;
            int fanModeParam = -1;
            int channelIdx = -1;

            if (request->hasParam("pwm_pct")) {
                pwmPct = request->getParam("pwm_pct")->value().toInt();
            }
            if (request->hasParam("fan_mode")) {
                fanModeParam = request->getParam("fan_mode")->value().toInt();
            }
            if (request->hasParam("temp_channel")) {
                channelIdx = request->getParam("temp_channel")->value().toInt();
            }
            if (pwmPct == -1 && fanModeParam == -1 && channelIdx == -1) {
                Serial.println("No parameters provided, error hereee");
                request->send(400, "application/json", "{\"error\":\"No parameters provided\"}");
                return;
            }

            applyFanControl(pwmPct, fanModeParam, channelIdx);
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        });

    // Get config endpoint - returns WiFi and MQTT credentials as JSON
    server->on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
        preferences.begin("settings", true);  // Read-only mode
        String ssid = preferences.getString("ssid", "");
        String pass = preferences.getString("pass", "");
        String mqtt_broker_ip = preferences.getString("broker_ip", "");
        String mqtt_port = preferences.getString("port", "");
        String mqtt_client_id = preferences.getString("client_id", "");
        String mqtt_username = preferences.getString("username", "");
        String mqtt_password = preferences.getString("password", "");
        String mqtt_topic_base = preferences.getString("topic_base", "");
        int fan_mode = preferences.getInt("fanMode", 1);  // Default to standard (1)
        int temperature_target = preferences.getInt("tempTarget", 25);  // Default to 25°C
        float temperature_allowance = preferences.getFloat("tempAllowance", 2.0);  // Default to 2.0°C
        preferences.end();
        
        String json = "{\"ssid\":\"" + ssid + "\",\"pass\":\"" + pass + 
                     "\",\"mqtt_broker_ip\":\"" + mqtt_broker_ip + 
                     "\",\"mqtt_port\":\"" + mqtt_port + 
                     "\",\"mqtt_client_id\":\"" + mqtt_client_id + 
                     "\",\"mqtt_username\":\"" + mqtt_username + 
                     "\",\"mqtt_password\":\"" + mqtt_password + 
                     "\",\"mqtt_topic_base\":\"" + mqtt_topic_base + 
                     "\",\"fan_mode\":" + String(fan_mode) + 
                     ",\"temperature_target\":" + String(temperature_target) + 
                     ",\"temperature_allowance\":" + String(temperature_allowance, 1) + "}";
        request->send(200, "application/json", json);
    });

    // Setting endpoint - saves WiFi and MQTT credentials
    server->on("/setting", HTTP_GET, [](AsyncWebServerRequest *request){
        if (request->hasParam("ssid") && request->hasParam("pass")) {
            preferences.begin("settings", false);  // Read-write mode
            
            // Save WiFi settings
            String ssid = request->getParam("ssid")->value();
            String pass = request->getParam("pass")->value();
            preferences.putString("ssid", ssid);
            preferences.putString("pass", pass);
            
            // Save MQTT settings if provided
            if (request->hasParam("mqtt_broker_ip") && request->hasParam("mqtt_port") && 
                request->hasParam("mqtt_client_id") && request->hasParam("mqtt_username") && 
                request->hasParam("mqtt_password") && request->hasParam("mqtt_topic_base")) {
                
                preferences.putString("broker_ip", request->getParam("mqtt_broker_ip")->value());
                preferences.putString("port", request->getParam("mqtt_port")->value());
                preferences.putString("client_id", request->getParam("mqtt_client_id")->value());
                preferences.putString("username", request->getParam("mqtt_username")->value());
                preferences.putString("password", request->getParam("mqtt_password")->value());
                preferences.putString("topic_base", request->getParam("mqtt_topic_base")->value());
            }
            
            // Save fan mode and temperature target if provided
            if (request->hasParam("fan_mode") && request->hasParam("temperature_target")) {
                int fan_mode = request->getParam("fan_mode")->value().toInt();
                int temp_target = request->getParam("temperature_target")->value().toInt();
                preferences.putInt("fanMode", fan_mode);
                preferences.putInt("tempTarget", temp_target);
            }
            
            // Save temperature allowance if provided (can be saved independently)
            if (request->hasParam("temperature_allowance")) {
                float temp_allowance = request->getParam("temperature_allowance")->value().toFloat();
                preferences.putFloat("tempAllowance", temp_allowance);
            }
            
            preferences.end();
            
            request->send(200, "text/html", "<h1>Settings saved!</h1><p>WiFi, MQTT, and fan settings have been saved. Please restart the device.</p><a href='/'>Back</a>");
        } else {
            request->send(400, "text/html", "<h1>Error</h1><p>Missing required parameters (ssid, pass)</p><a href='/'>Back</a>");
        }
    });

    // Reset endpoint - clears all settings
    server->on("/reset", HTTP_GET, [](AsyncWebServerRequest *request){
        preferences.begin("settings", false);
        // Clear only WiFi and MQTT related keys, keep other settings like pwmPct and tempPt
        preferences.remove("ssid");
        preferences.remove("pass");
        preferences.remove("broker_ip");
        preferences.remove("port");
        preferences.remove("client_id");
        preferences.remove("username");
        preferences.remove("password");
        preferences.remove("topic_base");
        preferences.remove("fanMode");
        preferences.remove("tempTarget");
        preferences.remove("tempAllowance");
        preferences.end();
        
        request->send(200, "text/html", "<h1>Settings reset!</h1><p>WiFi, MQTT, and fan settings have been cleared.</p><a href='/'>Back</a>");
    });

    // Restart endpoint - restarts the ESP32
    server->on("/restart", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/html", "<h1>Restarting...</h1><p>The device will restart in a few seconds.</p>");
        delay(1000);
        ESP.restart();
    });


    // SSE endpoint for real-time PWM updates
    events = new AsyncEventSource("/events");
    events->onConnect([](AsyncEventSourceClient *client){
        if(client->lastId()){
            Serial.printf("Client reconnected! Last message ID: %u\n", client->lastId());
        }
        // Send initial empty data when client connects (will be updated when sht41Task runs)
        String json = "{";
        json += "\"channels\":{},";
        json += "\"currentPwm\":0,";
        json += "\"currentPwmPercentage\":0,";
        json += "\"targetPwmPercentage\":0,";
        json += "\"TargetTemperature\":0,";
        json += "\"currentTemperatureChannel\":0,";
        json += "\"fanMode\":1,";
        json += "\"fanTempChannelIdx\":0,";
        json += "\"temperatureUnit\":\"C\",";
        json += "\"ipAddress\":\"" + getDeviceIpAddress() + "\"";
        json += "}";
        client->send(json.c_str(), "pwm_update", millis(), 1000);
    });
    server->addHandler(events);
    
        // Handle 404
        server->onNotFound([](AsyncWebServerRequest *request){
            Serial.print("404 - Not found: ");
            Serial.println(request->url());
            request->send(404, "text/plain", "Not found");
        });
        
        routesRegistered = true;
        Serial.println("Web server routes registered");
    }
}

void startWebServer() {
    if (server != nullptr) {
        // Check if WiFi is in STA mode (connected) or AP mode
        if (WiFi.status() == WL_CONNECTED) {
            // Station mode - connected to WiFi
            server->begin();
            Serial.print("Web server started on port ");
            Serial.println(serverPort);
            Serial.print("Access the portal at: http://");
            Serial.println(WiFi.localIP());
        } else if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
            // Access Point mode - serving configuration portal
            server->begin();
            Serial.print("Web server started on port ");
            Serial.println(serverPort);
            Serial.print("Access the configuration portal at: http://");
            Serial.println(WiFi.softAPIP());
        } else {
            Serial.println("Cannot start web server: WiFi not initialized");
        }
    } else {
        Serial.println("Cannot start web server: server not initialized");
    }
}

void stopWebServer() {
    if (server != nullptr) {
        server->end();
        Serial.println("Web server stopped");
    }
}

void startApServer() {
    // Start Access Point mode for configuration
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32-Config");  // Default AP credentials
    IPAddress AP_IP(192, 168, 4, 1);
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));

    Serial.print("Access Point started. SSID: ESP32-Config, Password: config1234");
    Serial.print(", IP: ");
    Serial.println(AP_IP);

    // --- Initialize and start web server on AP ---
    initWebServer(80);
    startWebServer();
}

// Broadcast PWM update via SSE (call from sht41Task)
static String formatReadingValue(float value) {
    if (value < 0) {
        return "-1";
    }
    return String(value, 1);
}

String buildSensorDataJson(const SensorData& sensorData) {
    String json = "{";

    json += "\"channels\":{";
    for (uint8_t idx = 0; idx < SHT41_COUNT; idx++) {
        if (idx > 0) json += ",";
        json += "\"ch" + String(SHT41_PINS[idx]) + "\":{";
        json += "\"temperature\":" + formatReadingValue(sensorData.readings[idx].temperature) + ",";
        json += "\"humidity\":" + formatReadingValue(sensorData.readings[idx].humidity);
        json += "}";
    }
    json += "},";

    json += "\"currentPwm\":" + String(sensorData.currentPwmValue) + ",";
    json += "\"currentPwmPercentage\":" + String(sensorData.currentPwmPercentage) + ",";
    json += "\"targetPwmPercentage\":" + String(sensorData.targetPwmPercentage) + ",";
    json += "\"TargetTemperature\":" + String(sensorData.temperatureTarget) + ",";
    json += "\"currentTemperatureChannel\":" + String(SHT41_PINS[sensorData.fanTempChannelIdx]) + ",";
    json += "\"fanMode\":" + String(sensorData.fanMode) + ",";
    json += "\"fanTempChannelIdx\":" + String(sensorData.fanTempChannelIdx) + ",";
    json += "\"temperatureUnit\":\"C\",";
    json += "\"ipAddress\":\"" + getDeviceIpAddress() + "\"";
    json += "}";

    return json;
}

void broadcastPwmUpdate(SensorData sensorData) {
    if (events != nullptr) {
        String json = buildSensorDataJson(sensorData);
        events->send(json.c_str(), "pwm_update", millis());
    }
}