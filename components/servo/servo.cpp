#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "servo.h"

static const char *TAG = "SERVO";

struct ServoCmd {
    uint32_t rest_pct;
    uint32_t press_pct;
    uint32_t hold_ms;
};

void Servo::start(gpio_num_t pin, ledc_channel_t channel, ledc_timer_t timer) {
    channel_ = channel;
    cmd_queue_ = xQueueCreate(4, sizeof(ServoCmd));

    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_cfg.timer_num = timer;
    timer_cfg.duty_resolution = LEDC_TIMER_14_BIT;
    timer_cfg.freq_hz = 50;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer_cfg);

    ch_cfg_ = {};
    ch_cfg_.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_cfg_.channel = channel;
    ch_cfg_.timer_sel = timer;
    ch_cfg_.gpio_num = pin;
    ch_cfg_.duty = 0;
    ch_cfg_.hpoint = 0;
    ledc_channel_config(&ch_cfg_);

    xTaskCreate(task_fn, "servo", 2048, this, tskIDLE_PRIORITY + 1, nullptr);
    ESP_LOGI(TAG, "Servo on GPIO %d ready", pin);
}

void Servo::press(uint32_t rest_pct, uint32_t press_pct, uint32_t hold_ms) {
    ServoCmd cmd{rest_pct, press_pct, hold_ms};
    xQueueSend(cmd_queue_, &cmd, 0);
}

void Servo::task_fn(void *arg) {
    auto *self = static_cast<Servo *>(arg);
    ServoCmd cmd;
    while (true) {
        if (xQueueReceive(self->cmd_queue_, &cmd, portMAX_DELAY) == pdTRUE) {
            uint32_t press_us{}, rest_us{};
            press_us = SERVO_PULSE_MIN_US + ((SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) * cmd.press_pct) / 100;
            rest_us = SERVO_PULSE_MIN_US + ((SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) * cmd.rest_pct) / 100;
            ESP_LOGI(TAG, "Press: rest_pct=%lu rest_us=%lu press_pct=%lu press_us=%lu hold=%lums",
                     cmd.rest_pct, rest_us, cmd.press_pct, press_us, cmd.hold_ms);
            self->set_pulse_us(press_us);
            vTaskDelay(pdMS_TO_TICKS(cmd.hold_ms));
            self->set_pulse_us(rest_us);
            vTaskDelay(pdMS_TO_TICKS(500));
            self->set_pulse_us(0);
        }
    }
}

void Servo::set_pulse_us(uint32_t us) {
    // 14-bit resolution at 50Hz → 16384 ticks per 20ms period
    if (us == 0) {
        // no-op: duty=0 produces no pulses, servo goes limp
    } else if (us < SERVO_PULSE_MIN_US) {
        us = SERVO_PULSE_MIN_US;
    } else if (us > SERVO_PULSE_MAX_US) {
        us = SERVO_PULSE_MAX_US;
    }
    uint32_t duty = us * 16384 / 20000;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel_, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel_);
}
