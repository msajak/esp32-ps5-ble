#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

struct Params {
    int temp{};
    int time{};
};

struct Status {
    int64_t timenow;
    float temp_celsius;
    float setpoint;
    float temp_error;
    bool heating_active;
    uint32_t duty;
    uint32_t duty_max;
    uint32_t heater_timeout_sec;
    uint32_t heater_time_left_sec;
    bool fan_status;
    int64_t last_rise_fan1;
    int64_t edge_count_fan1;
    int64_t last_rise_fan2;
    int64_t edge_count_fan2;
    bool fault_lockout;
};
