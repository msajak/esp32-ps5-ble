#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "servo.h"

static const char *TAG = "SERVO";

struct ServoCmd {
    uint32_t rest_us;
    uint32_t press_us;
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

    ledc_channel_config_t ch_cfg = {};
    ch_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_cfg.channel = channel;
    ch_cfg.timer_sel = timer;
    ch_cfg.gpio_num = pin;
    ch_cfg.duty = 0;
    ch_cfg.hpoint = 0;
    ledc_channel_config(&ch_cfg);

    set_pulse_us(832);

    xTaskCreate(task_fn, "servo", 2048, this, tskIDLE_PRIORITY + 1, nullptr);
    ESP_LOGI(TAG, "Servo on GPIO %d ready", pin);
}

void Servo::press(uint32_t rest_us, uint32_t press_us, uint32_t hold_ms) {
    ServoCmd cmd{rest_us, press_us, hold_ms};
    xQueueSend(cmd_queue_, &cmd, 0);
}

void Servo::task_fn(void *arg) {
    auto *self = static_cast<Servo *>(arg);
    ServoCmd cmd;
    while (true) {
        if (xQueueReceive(self->cmd_queue_, &cmd, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Press: rest=%lu press=%lu hold=%lums",
                     (unsigned long)cmd.rest_us, (unsigned long)cmd.press_us, (unsigned long)cmd.hold_ms);
            self->set_pulse_us(cmd.press_us);
            vTaskDelay(pdMS_TO_TICKS(cmd.hold_ms));
            self->set_pulse_us(cmd.rest_us);
        }
    }
}

void Servo::set_pulse_us(uint32_t us) {
    // 14-bit resolution at 50Hz → 16384 ticks per 20ms period
    uint32_t duty = us * 16384 / 20000;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel_, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel_);
}
