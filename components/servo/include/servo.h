#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

class Servo {
public:
    void start(gpio_num_t pin, ledc_channel_t channel = LEDC_CHANNEL_0,
               ledc_timer_t timer = LEDC_TIMER_0);
    void press(uint32_t rest_us = 810, uint32_t press_us = 2050, uint32_t hold_ms = 1000);

private:
    static constexpr uint32_t SERVO_PULSE_MIN_US = 900;
    static constexpr uint32_t SERVO_PULSE_MAX_US = 2100;
    static void task_fn(void *arg);
    void set_pulse_us(uint32_t us);

    ledc_channel_t channel_{};
    ledc_channel_config_t ch_cfg_{};
    QueueHandle_t cmd_queue_{};
    uint8_t angle_rest_{0};
    uint8_t angle_press_{180};
};
