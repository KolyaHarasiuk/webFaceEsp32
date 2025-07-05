#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "vrx_types.h"
#include "vrx_formatters.h"


#define LED_PIN 2 // D2 на твоїй платі

volatile bool systemActive = false;   // глобальний флаг (робить loop простіше)
volatile bool shouldShutdown = false; // флаг для вимкнення системи
unsigned long shutdownTime = 0;
unsigned long lastBlink = 0;
bool ledState = false;

#define BUTTON_PIN 0 // Використовуємо BOOT кнопку на ESP32

const char* ssid = "vrx_control";
const char* password = "freeAzov";

AsyncWebServer server(80);
Preferences preferences;

VRXConfig config;

bool stm32_authenticated = false;

// --- Функція очікування відповіді від UART ---
String waitForLineUART(unsigned long timeout) {
    unsigned long start = millis();
    String result = "";
    while (millis() - start < timeout) {
        if (Serial2.available()) {
            result = Serial2.readStringUntil('\n');
            result.trim();
            break;
        }
    }
    return result;
}

// --- Функція отримання конфігурації з STM32 ---
bool getConfigFromSTM32(VRXConfig &config) {
    // Очистити буфер UART перед початком
    while(Serial2.available()) Serial2.read();
    delay(10);
    
    // 1. crsf
    Serial2.println("crsf");
    String crsf_resp = waitForLineUART(200);
    Serial.println("CRSF відповідь: '" + crsf_resp + "'");
    config.crsf = crsf_resp.indexOf("crsf 1") != -1;
    
    // Очистити буфер і затримка
    while(Serial2.available()) Serial2.read();
    delay(100);
    
    // 2. set_id
    Serial2.println("set_id");
    String id_resp = waitForLineUART(200);
    Serial.println("ID відповідь від STM32: '" + id_resp + "'");
    int idx = id_resp.indexOf("device id ");
    if (idx != -1) {
        String id_str = id_resp.substring(idx + 10);
        Serial.println("ID substring: '" + id_str + "'");
        config.id = id_str.toInt();
        Serial.println("Parsed ID: " + String(config.id));
    }
    
    // 3. set_rotation
    Serial2.println("set_rotation");
    String rot_resp = waitForLineUART(200);
    Serial.println("Rotation відповідь: '" + rot_resp + "'");
    idx = rot_resp.indexOf("lcd_rotation ");
    if (idx != -1) config.rotation = rot_resp.substring(idx + 13).toInt() == 1 ? 180 : 0;
    
    // 4. vrx_type
    Serial2.println("vrx_type");
    String vtype_resp = waitForLineUART(200);
    Serial.println("VRX Type відповідь: '" + vtype_resp + "'");
    idx = vtype_resp.indexOf("vrx type ");
    if (idx != -1) {
        int vcode = vtype_resp.substring(idx + 9, idx + 10).toInt();
        // 0: Skyzone_UART, 1: RX5808, 2: rapidfire, 3: TBS_Fusion, 4: Dec_vrx
        if (vcode == 0) config.vrx_type = "Skyzone_UART";
        else if (vcode == 1) config.vrx_type = "rx5808";
        else if (vcode == 2) config.vrx_type = "rapid-fire";
        else if (vcode == 3) config.vrx_type = "TBS_Fusion";
        else if (vcode == 4) config.vrx_type = "Dec_vrx";
    }
    
    // 5. vrx (10 рядків)
    Serial2.println("vrx");
    String vrx_lines[10];
    int vrx_count = 0;
    unsigned long start = millis();
    while (vrx_count < 10 && millis() - start < 500) {
        if (Serial2.available()) {
            String line = Serial2.readStringUntil('\n');
            line.trim();
            // Пропускаємо рядок "vrx type ..." який не є даними
            if (line.startsWith("vrx ") && line.indexOf("type") == -1) {
                vrx_lines[vrx_count++] = line;
            }
        }
    }
    
    // Очистити старі дані
    config.aux_divider = 0;
    for (int i = 0; i < 10; i++) {
        config.bands[i] = "";
        config.channels[i] = 0;
        config.frequencies[i] = 0;
    }
    
    // Парсинг vrx даних
    for (int i = 0; i < vrx_count; i++) {
        Serial.println("VRX рядок " + String(i) + ": '" + vrx_lines[i] + "'");
        String row = vrx_lines[i].substring(4); // після "vrx "
        Serial.println("Після substring: '" + row + "'");
        
        int values[6] = {0};
        int valueIndex = 0;
        int startPos = 0;
        
        // Парсимо числа розділені пробілами
        for (int j = 0; j <= row.length(); j++) {
            if (j == row.length() || row[j] == ' ') {
                if (j > startPos) {
                    String num = row.substring(startPos, j);
                    num.trim();
                    if (num.length() > 0 && valueIndex < 6) {
                        values[valueIndex] = num.toInt();
                        Serial.print("values[" + String(valueIndex) + "]=" + String(values[valueIndex]) + " ");
                        valueIndex++;
                    }
                }
                startPos = j + 1;
            }
        }
        Serial.println();
        
        // Якщо aux (values[1]) == 0, це кінець даних
        if (values[1] == 0) break;
        
        config.aux_divider++;
        config.aux = values[1] + 5;
        
        // Використовуємо індекс з values[0] або values[3] для правильного розміщення
        int vrx_index = values[0];
        if (vrx_index < 10) {
            config.bands[vrx_index] = getBandName(values[4]);
            config.channels[vrx_index] = values[5];
            // Обчислюємо частоти для внутрішнього використання
            config.frequencies[vrx_index] = channelToFrequency(config.bands[vrx_index], config.channels[vrx_index]);
        }
    }
    
    config.type = "channel_band";
    return true;
}

// --- Функція для формування команд для STM32 ---
String prepareSTM32Commands(const VRXConfig& config) {
    String result = "";
    result += "crsf " + String(config.crsf ? 1 : 0) + "\n";
    result += "set_id " + String(config.id) + "\n";
    result += "set_rotation " + String(config.rotation == 180 ? 1 : 0) + "\n";
    
    int vrx_code = 0;
    if (config.vrx_type == "Skyzone_UART") vrx_code = 0;
    else if (config.vrx_type == "rx5808") vrx_code = 1;
    else if (config.vrx_type == "rapid-fire") vrx_code = 2;
    else if (config.vrx_type == "TBS_Fusion") vrx_code = 3;
    else if (config.vrx_type == "Dec_vrx") vrx_code = 4;
    
    result += "vrx_type " + String(vrx_code) + "\n";

    int n = config.aux_divider;
    int aux_param = config.aux - 5;
    
    // Записуємо всі 10 VRX команд
    for (int i = 0; i < 10; i++) {
        if (i < n) {
            // Звичайні дані
            result += "vrx " + String(i) + "," + String(aux_param) + "," + String(n) + "," + String(i);
            if (config.type == "frequency") {
                result += "," + String(config.frequencies[i]);
            } else { // channel_band
                int band_idx = getBandIndex(config.bands[i]);
                int channel_num = config.channels[i];
                result += "," + String(band_idx) + "," + String(channel_num);
            }
        } else {
            // Заповнюємо нулями
            result += "vrx " + String(i) + ",0,0,0,0,0";
        }
        result += "\n";
    }
    
    result += "wq\n";
    return result;
}

// --- Функція вимкнення системи ---
void shutdownSystem() {
    Serial.println("Вимикаю систему...");
    
    // Зупиняємо веб-сервер
    server.end();
    
    // Вимикаємо WiFi
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    
    // Вимикаємо LED
    digitalWrite(LED_PIN, LOW);
    
    // Скидаємо флаги
    systemActive = false;
    shouldShutdown = false;
    
    Serial.println("Система вимкнена");
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
    config.type = preferences.getString("type", "channel_band");
    config.aux = preferences.getUInt("aux", 5);
    config.aux_divider = preferences.getUInt("aux_divider", 1);

    for (int i = 0; i < 10; i++) {
        String key = "freq" + String(i);
        config.frequencies[i] = preferences.getUInt(key.c_str(), 5800);
    }
    for (int i = 0; i < 10; i++) {
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

    // Зберігаємо всі 10 елементів
    for (int i = 0; i < 10; i++) {
        String key = "freq" + String(i);
        preferences.putUInt(key.c_str(), config.frequencies[i]);
    }
    for (int i = 0; i < 10; i++) {
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
    
    // Очистити масиви
    for (int i = 0; i < 10; i++) {
        config.frequencies[i] = 0;
        config.bands[i] = "";
        config.channels[i] = 0;
    }
    
    if (config.type == "frequency") {
        JsonArray frequencies = doc["frequencies"];
        for (int i = 0; i < frequencies.size() && i < 10; i++) {
            config.frequencies[i] = frequencies[i];
        }
    } else {
        JsonArray bands = doc["bands"];
        JsonArray channels = doc["channels"];
        for (int i = 0; i < bands.size() && i < 10; i++) {
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

// Forward declaration
void setupWebServer();

// --- Автентифікація з STM32 ---
bool authenticateSTM32() {
    Serial.println("Відправляю пароль до STM32...");
    // Очистити буфер перед відправкою
    while(Serial2.available()) Serial2.read();
    
    Serial2.println("freeAzov");
    
    String response = waitForLineUART(500); // Чекаємо максимум 500мс згідно нового флоу
    
    if (response.indexOf("hello mellon!") != -1 || response.indexOf("try help") != -1) {
        Serial.println("Аутентифікація STM32: OK");
        return true;
    }
    
    Serial.println("STM32 не відповів або відповідь не розпізнана");
    return false;
}

// --- Запуск системи після кнопки ---
void activateSystem() {
    Serial.println("Активація системи...");
    
    // Включаємо миготіння LED
    systemActive = true;
    
    // Спроба аутентифікації з STM32
    stm32_authenticated = authenticateSTM32();
    
    if (stm32_authenticated) {
        Serial.println("STM32 підключений, отримую конфігурацію...");
        if (getConfigFromSTM32(config)) {
            Serial.println("Конфігурація отримана з STM32");
            saveConfig(); // Зберігаємо нову конфігурацію
            printConfig();
        }
    } else {
        Serial.println("STM32 не підключений, використовую збережену конфігурацію");
    }
    
    // Піднімаємо WiFi AP незалежно від статусу STM32
    IPAddress apIP(10, 0, 0, 1);
    IPAddress gateway(10, 0, 0, 1);
    IPAddress subnet(255, 255, 255, 0);

    WiFi.softAPConfig(apIP, gateway, subnet);
    WiFi.softAP("VRX_Config", "freeAzov");

    Serial.println("WiFi AP піднято!");
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());

    // Запускаємо веб-сервер
    setupWebServer();
    server.begin();
    Serial.println("Веб-сервер запущено!");
    Serial.print("http://");
    Serial.println(WiFi.softAPIP());
}

// --- Налаштування веб-сервера ---
void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LittleFS, "/index.html", "text/html");
    });
    
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = getConfigJSON();
        // Додаємо статус STM32 до відповіді
        StaticJsonDocument<2048> doc;
        deserializeJson(doc, json);
        doc["stm32"] = stm32_authenticated ? "connected" : "not_connected";
        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });
    
    server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, 
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
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
                    // Очистити буфер перед відправкою команд
                    while(Serial2.available()) Serial2.read();
                    delay(10);
                    
                    int start = 0;
                    int end = stm32_cmds.indexOf('\n', start);
                    while (end != -1) {
                        String cmd = stm32_cmds.substring(start, end);
                        if (cmd.length() > 0) {
                            Serial2.println(cmd);
                            Serial.print("STM32 << "); Serial.println(cmd);
                            delay(10); // Маленька затримка між командами
                            sent = true;
                        }
                        start = end + 1;
                        end = stm32_cmds.indexOf('\n', start);
                    }
                    
                    if (sent) {
                        resp = "{\"status\":\"ok\",\"stm32\":\"connected\",\"shutdown\":true}";
                        // Встановлюємо флаг для вимкнення через 2 секунди
                        shouldShutdown = true;
                        shutdownTime = millis() + 2000;
                    } else {
                        resp = "{\"status\":\"ok\",\"stm32\":\"connected\"}";
                    }
                } else {
                    Serial.println("УВАГА: Аутентифікація з STM32 не пройдена. Команди не відправлені.");
                    resp = "{\"status\":\"ok\",\"stm32\":\"not_connected\"}";
                }
            } else {
                resp = "{\"status\":\"error\",\"stm32\":\"" + String(stm32_authenticated ? "connected" : "not_connected") + "\"}";
            }
            request->send(200, "application/json", resp);
        });
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

    // Перевірка чи потрібно вимкнути систему
    if (shouldShutdown && millis() >= shutdownTime) {
        shutdownSystem();
        return;
    }

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