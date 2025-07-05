#pragma once

#include <Arduino.h>

struct VRXConfig {
    bool crsf = false;
    int id = 0;
    int rotation = 0;
    String vrx_type = "";
    String type = "channel_band";
    int aux = 5;
    int aux_divider = 1;
    String bands[10];
    int channels[10];
    int frequencies[10]; // ОБОВ'ЯЗКОВО! Бо використовується у форматерах
};
