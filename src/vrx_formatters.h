#ifndef VRX_FORMATTERS_H
#define VRX_FORMATTERS_H

#include <Arduino.h>

// Таблиця частот для перетворення каналів у частоти
const uint16_t frequencyTable[9][8] = {
    // Band A
    {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725},
    // Band B
    {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866},
    // Band E
    {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945},
    // Band F (FatShark)
    {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880},
    // Band R (Raceband)
    {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917},
    // Band L (LowRace)
    {5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621},
    // Band X
    {4990, 5020, 5050, 5080, 5110, 5140, 5170, 5200},
    // Band LL
    {4900, 4910, 4920, 4930, 4940, 4950, 4960, 4970},
    // Band S
    {6002, 6028, 6054, 6080, 6106, 6132, 6158, 6184}
};

// Функція для отримання індексу бенду
int getBandIndex(const String& band) {
    if (band == "A") return 1;
    if (band == "B") return 2;
    if (band == "E") return 3;
    if (band == "F") return 4;
    if (band == "R") return 5;
    if (band == "L") return 6;
    if (band == "X") return 7;
    if (band == "LL") return 8;
    if (band == "S") return 9;
    return 1; // За замовчуванням Band A
}
 
// Функція для перетворення каналу та бенду в частоту
uint16_t channelToFrequency(const String& band, uint8_t channel) {
    if (channel < 1 || channel > 8) return 5800; // Значення за замовчуванням
    int bandIdx = getBandIndex(band);
    return frequencyTable[bandIdx][channel - 1];
}

// Формат для RX5808
String formatRX5808(const VRXConfig& config) {
    String result = "AT+FRQ=";
    
    if (config.type == "frequency") {
        // Просто використовуємо частоти
        for (int i = 0; i < config.aux_divider; i++) {
            result += String(config.frequencies[i]);
            if (i < config.aux_divider - 1) result += ",";
        }
    } else {
        // Конвертуємо канали в частоти
        for (int i = 0; i < config.aux_divider; i++) {
            uint16_t freq = channelToFrequency(config.bands[i], config.channels[i]);
            result += String(freq);
            if (i < config.aux_divider - 1) result += ",";
        }
    }
    
    return result;
}

// Формат для RapidFire
String formatRapidFire(const VRXConfig& config) {
    // RapidFire працює тільки з каналами
    String result = "RF:";
    
    for (int i = 0; i < config.aux_divider; i++) {
        result += config.bands[i] + String(config.channels[i]);
        if (i < config.aux_divider - 1) result += ";";
    }
    
    return result;
}

// Формат для TBS Fusion
String formatTBSFusion(const VRXConfig& config) {
    String result = "{\"cmd\":\"set_channels\",\"data\":[";
    
    if (config.type == "frequency") {
        for (int i = 0; i < config.aux_divider; i++) {
            result += "{\"freq\":" + String(config.frequencies[i]) + "}";
            if (i < config.aux_divider - 1) result += ",";
        }
    } else {
        for (int i = 0; i < config.aux_divider; i++) {
            uint16_t freq = channelToFrequency(config.bands[i], config.channels[i]);
            result += "{\"band\":\"" + config.bands[i] + "\",";
            result += "\"ch\":" + String(config.channels[i]) + ",";
            result += "\"freq\":" + String(freq) + "}";
            if (i < config.aux_divider - 1) result += ",";
        }
    }
    
    result += "]}";
    return result;
}

// Формат для DEC VRX
String formatDecVRX(const VRXConfig& config) {
    String result = "DEC>";
    
    if (config.type == "frequency") {
        for (int i = 0; i < config.aux_divider; i++) {
            result += "F" + String(i + 1) + ":" + String(config.frequencies[i]) + ";";
        }
    } else {
        for (int i = 0; i < config.aux_divider; i++) {
            result += "CH" + String(i + 1) + ":" + config.bands[i] + String(config.channels[i]) + ";";
        }
    }
    
    return result;
}

// Формат для Skyzone UART
String formatSkyzoneUART(const VRXConfig& config) {
    // Skyzone працює тільки з каналами
    String result = "$SKY,";
    
    for (int i = 0; i < config.aux_divider; i++) {
        // Skyzone використовує числові коди для бендів
        int bandCode = getBandIndex(config.bands[i]) + 1;
        result += String(bandCode) + "," + String(config.channels[i]);
        if (i < config.aux_divider - 1) result += ",";
    }
    
    result += "*";
    
    // Додаємо контрольну суму
    uint8_t checksum = 0;
    for (int i = 1; i < result.length() - 1; i++) {
        checksum ^= result[i];
    }
    result += String(checksum, HEX);
    
    return result;
}

// Головна функція форматування
String formatVRXData(const VRXConfig& config) {
    if (config.vrx_type == "rx5808") {
        return formatRX5808(config);
    } else if (config.vrx_type == "rapid-fire") {
        return formatRapidFire(config);
    } else if (config.vrx_type == "TBS_Fusion") {
        return formatTBSFusion(config);
    } else if (config.vrx_type == "Dec_vrx") {
        return formatDecVRX(config);
    } else if (config.vrx_type == "Skyzone_UART") {
        return formatSkyzoneUART(config);
    }
    
    return "Unknown VRX type";
}

// Функція для відправки даних через UART
void sendVRXCommand(const String& command, HardwareSerial& serial = Serial2) {
    Serial.print("Sending VRX command: ");
    Serial.println(command);
    
    serial.println(command);
    
    // Додаткова логіка для різних VRX
    if (command.startsWith("AT+")) {
        // RX5808 потребує затримку після команди
        delay(100);
    } else if (command.startsWith("$SKY")) {
        // Skyzone потребує CR+LF
        serial.write('\r');
        serial.write('\n');
    }
}

#endif // VRX_FORMATTERS_H