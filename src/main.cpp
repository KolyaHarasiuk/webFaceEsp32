#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>


#define LED_PIN 2 // D2 на твоїй платі

volatile bool systemActive = false;   // глобальний флаг (робить loop простіше)
unsigned long lastBlink = 0;
bool ledState = false;

#define BUTTON_PIN 0 // Використовуємо BOOT кнопку на ESP32

const char* ssid = "vrx_control";
const char* password = "freeAzov";

AsyncWebServer server(80);
Preferences preferences;

struct VRXConfig {
    uint8_t id;
    bool crsf;
    uint16_t rotation;
    String vrx_type;
    String type;
    uint8_t aux;
    uint8_t aux_divider;
    uint16_t frequencies[8];
    String bands[8];
    uint8_t channels[8];
};

VRXConfig config;

#include "vrx_formatters.h"

bool stm32_authenticated = false;

// --- Функція для формування команд для STM32 ---
String prepareSTM32Commands(const VRXConfig& config) {
    String result = "";
    result += "crsf " + String(config.crsf ? 1 : 0) + "\n";
    result += "set_id " + String(config.id) + "\n";
    result += "set_rotation " + String(config.rotation == 180 ? 1 : 0) + "\n";
    int vrx_code = 0;
    if (config.vrx_type == "rx5808") vrx_code = 1;
    else if (config.vrx_type == "rapid-fire") vrx_code = 2;
    else if (config.vrx_type == "TBS_Fusion") vrx_code = 3;
    else if (config.vrx_type == "Dec_vrx") vrx_code = 4;
    if (vrx_code > 0) {
        result += "vrx_type " + String(vrx_code) + "\n";
    }

    int n = config.aux_divider;
    int aux_param = config.aux - 5;
    for (int i = 0; i < n; i++) {
        result += "vrx " + String(i) + "," + String(aux_param) + "," + String(n) + "," + String(i);
        if (config.type == "frequency") {
            result += "," + String(config.frequencies[i]);
        } else { // channel_band
            int band_idx = getBandIndex(config.bands[i]); // band рахується з 1
            int channel_num = config.channels[i]; // вже з 1
            result += "," + String(band_idx) + "," + String(channel_num);
        }
        result += "\n";
    }
    result += "wq\n";
    return result;
}

void initFS() {
    if (!LittleFS.begin()) {
        Serial.println("Помилка монтування LittleFS");
        return;
    }
    Serial.println("LittleFS змонтовано успішно");
}

void loadConfig() {
    preferences.begin("vrx-config", false);
    config.id = preferences.getUInt("id", 0);
    config.crsf = preferences.getBool("crsf", false);
    config.rotation = preferences.getUInt("rotation", 0);
    config.vrx_type = preferences.getString("vrx_type", "rx5808");
    config.type = preferences.getString("type", "frequency");
    config.aux = preferences.getUInt("aux", 4);
    config.aux_divider = preferences.getUInt("aux_divider", 1);

    for (int i = 0; i < 8; i++) {
        String key = "freq" + String(i);
        config.frequencies[i] = preferences.getUInt(key.c_str(), 5800);
    }
    for (int i = 0; i < 8; i++) {
        String bandKey = "band" + String(i);
        String channelKey = "ch" + String(i);
        config.bands[i] = preferences.getString(bandKey.c_str(), "A");
        config.channels[i] = preferences.getUInt(channelKey.c_str(), 1);
    }
    preferences.end();
    Serial.println("Конфігурація завантажена");
}

void saveConfig() {
    preferences.begin("vrx-config", false);
    preferences.putUInt("id", config.id);
    preferences.putBool("crsf", config.crsf);
    preferences.putUInt("rotation", config.rotation);
    preferences.putString("vrx_type", config.vrx_type);
    preferences.putString("type", config.type);
    preferences.putUInt("aux", config.aux);
    preferences.putUInt("aux_divider", config.aux_divider);

    for (int i = 0; i < config.aux_divider; i++) {
        String key = "freq" + String(i);
        preferences.putUInt(key.c_str(), config.frequencies[i]);
    }
    for (int i = 0; i < config.aux_divider; i++) {
        String bandKey = "band" + String(i);
        String channelKey = "ch" + String(i);
        preferences.putString(bandKey.c_str(), config.bands[i]);
        preferences.putUInt(channelKey.c_str(), config.channels[i]);
    }
    preferences.end();
    Serial.println("Конфігурація збережена");
}

String getConfigJSON() {
    StaticJsonDocument<2048> doc;
    doc["id"] = config.id;
    doc["crsf"] = config.crsf;
    doc["rotation"] = config.rotation;
    doc["vrx_type"] = config.vrx_type;
    doc["type"] = config.type;
    doc["aux"] = config.aux;
    doc["aux_divider"] = config.aux_divider;

    if (config.type == "frequency") {
        JsonArray frequencies = doc.createNestedArray("frequencies");
        for (int i = 0; i < config.aux_divider; i++) {
            frequencies.add(config.frequencies[i]);
        }
    } else {
        JsonArray bands = doc.createNestedArray("bands");
        JsonArray channels = doc.createNestedArray("channels");
        for (int i = 0; i < config.aux_divider; i++) {
            bands.add(config.bands[i]);
            channels.add(config.channels[i]);
        }
    }
    String output;
    serializeJson(doc, output);
    return output;
}

bool parseConfigJSON(const String& json) {
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
        Serial.print("Помилка парсингу JSON: ");
        Serial.println(error.c_str());
        return false;
    }
    config.id = doc["id"] | config.id;
    config.crsf = doc["crsf"] | config.crsf;
    config.rotation = doc["rotation"] | config.rotation;
    config.vrx_type = doc["vrx_type"] | config.vrx_type;
    config.type = doc["type"] | config.type;
    config.aux = doc["aux"] | config.aux;
    config.aux_divider = doc["aux_divider"] | config.aux_divider;
    for (int i = 0; i < 8; i++) {
        config.frequencies[i] = 0;
        config.bands[i] = "";
        config.channels[i] = 0;
    }
    if (config.type == "frequency") {
        JsonArray frequencies = doc["frequencies"];
        for (int i = 0; i < frequencies.size() && i < 8; i++) {
            config.frequencies[i] = frequencies[i];
        }
    } else {
        JsonArray bands = doc["bands"];
        JsonArray channels = doc["channels"];
        for (int i = 0; i < bands.size() && i < 8; i++) {
            config.bands[i] = bands[i].as<String>();
            config.channels[i] = channels[i];
        }
    }
    return true;
}

void printConfig() {
    Serial.println("\n========== Поточна конфігурація ==========");
    Serial.print("ID: "); Serial.println(config.id);
    Serial.print("CRSF: "); Serial.println(config.crsf ? "ON" : "OFF");
    Serial.print("Rotation: "); Serial.print(config.rotation); Serial.println("°");
    Serial.print("VRX Type: "); Serial.println(config.vrx_type);
    Serial.print("Type: "); Serial.println(config.type);
    Serial.print("AUX: "); Serial.println(config.aux);
    Serial.print("AUX Divider: "); Serial.println(config.aux_divider);
    if (config.type == "frequency") {
        Serial.println("Частоти:");
        for (int i = 0; i < config.aux_divider; i++) {
            Serial.print("  "); Serial.print(i + 1); Serial.print(": ");
            Serial.print(config.frequencies[i]); Serial.println(" MHz");
        }
    } else {
        Serial.println("Канали та бенди:");
        for (int i = 0; i < config.aux_divider; i++) {
            Serial.print("  "); Serial.print(i + 1); Serial.print(": ");
            Serial.print("Band "); Serial.print(config.bands[i]);
            Serial.print(", Channel "); Serial.println(config.channels[i]);
        }
    }
    Serial.println("========================================\n");
}

// --- Автентифікація з STM32 ---
bool authenticateSTM32() {
    bool authenticated = false;
    for (int attempt = 0; attempt < 3 && !authenticated; attempt++) {
        Serial.println("Відправляю пароль до STM32...");
        Serial2.println("freeAzov");
        delay(300);
        unsigned long startTime = millis();
        String response = "";
        while (millis() - startTime < 1000) {
            while (Serial2.available()) {
                char c = Serial2.read();
                response += c;
            }
            if (response.indexOf("hello mellon!") != -1 || response.indexOf("try help") != -1) {
                authenticated = true;
                Serial.println("Аутентифікація STM32: OK");
                break;
            }
        }
        if (!authenticated) {
            Serial.println("STM32 не відповів, пробую ще раз...");
            delay(300);
        }
    }
    if (!authenticated) {
        Serial.println("Помилка: STM32 не підтвердив аутентифікацію!");
    }
    return authenticated;
}

// --- Запуск системи після кнопки ---
void activateSystem() {
    Serial.println("Активація системи...");

    IPAddress apIP(10, 0, 0, 1);
    IPAddress gateway(10, 0, 0, 1);
    IPAddress subnet(255, 255, 255, 0);

    WiFi.softAPConfig(apIP, gateway, subnet);
    WiFi.softAP("VRX-Config", "12345678");

    Serial.println("WiFi AP піднято!");
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());

    stm32_authenticated = authenticateSTM32();
    server.begin();
    Serial.println("Веб-сервер запущено!");
    Serial.print("http://");
    Serial.println(WiFi.softAPIP());
    
    // Встановлюємо прапорець активності системи
    systemActive = true;
}



void setup() {
    Serial.begin(115200);
    Serial2.begin(115200, SERIAL_8N1, 16, 17);
    delay(1000);
    Serial.println("\nVRX Control System");
    Serial.println("==================");
    initFS();
    loadConfig();
    printConfig();
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); // LED вимкнено при старті
}

// --- Логіка затиснення кнопки у loop() ---
void loop() {
    static unsigned long buttonDownTime = 0;
    static bool buttonWasDown = false;
    static unsigned long lastBlink = 0;
    static bool ledState = false;

    // КНОПКА: активуємо систему по довгому натисненню (3 сек)
    if (!systemActive) {
        digitalWrite(LED_PIN, LOW); // LED завжди вимкнений коли система не активна
        if (digitalRead(BUTTON_PIN) == LOW) {
            if (!buttonWasDown) {
                buttonDownTime = millis();
                buttonWasDown = true;
            } else {
                if (millis() - buttonDownTime > 3000) {
                    Serial.println("Кнопка затиснута 3 секунди — запускаємо систему!");
                    activateSystem();
                    systemActive = true;
                    buttonWasDown = false;
                }
            }
        } else {
            buttonWasDown = false;
        }
    } else {
        // МИГОТІННЯ LED поки вебморда активна
        unsigned long now = millis();
        if (now - lastBlink > 250) { // раз на 250 мс (4 рази на сек)
            ledState = !ledState;
            digitalWrite(LED_PIN, ledState ? HIGH : LOW);
            lastBlink = now;
        }
    }
}

// --- POST-обробник та API ---
AsyncWebServerResponse* handleConfigPost(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    String json = "";
    for (size_t i = 0; i < len; i++) {
        json += (char)data[i];
    }
    String resp;
    if (parseConfigJSON(json)) {
        saveConfig();
        printConfig();
        String stm32_cmds = prepareSTM32Commands(config);
        bool sent = false;
        if (stm32_authenticated) {
            int start = 0;
            int end = stm32_cmds.indexOf('\n', start);
            while (end != -1) {
                String cmd = stm32_cmds.substring(start, end);
                if (cmd.length() > 0) {
                    Serial2.println(cmd);
                    Serial.print("STM32 << "); Serial.println(cmd);
                    sent = true;
                }
                start = end + 1;
                end = stm32_cmds.indexOf('\n', start);
            }
            resp = "{\"status\":\"ok\",\"stm32\":\"connected\"}";
        } else {
            Serial.println("УВАГА: Аутентифікація з STM32 не пройдена. Команди не відправлені.");
            resp = "{\"status\":\"error\",\"stm32\":\"not_connected\"}";
        }
    } else {
        resp = "{\"status\":\"error\",\"stm32\":\"not_connected\"}";
    }
    return request->beginResponse(200, "application/json", resp);
}

__attribute__((constructor))
void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LittleFS, "/index.html", "text/html");
    });
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", getConfigJSON());
    });
    server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, handleConfigPost);
}
