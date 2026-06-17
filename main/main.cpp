#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "shared_objs.h"
#include "web_server.h"
#include "wifi_mgmt.h"
#include "blekb.h"
#include "servo.h"
#include "logger.h"

// ESP-IDF entry point
extern "C" {
    void app_main(void);
}

QueueHandle_t status_queue;

static constexpr const char* TAG = "MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "ESP32 PS5 BLE starting...");

    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS initialized");

    logger_init();
    ESP_LOGI(TAG, "Logger initialized");

    status_queue = xQueueCreate(1, sizeof(Status));
    if (!status_queue) {
        ESP_LOGE(TAG, "Failed to create status queue");
        return;
    }

    WifiMgmt wifi_mgmt;
    wifi_mgmt.start();

    EspidfBleKeyboard blekb;
    blekb.setup();

    Servo servo;
    servo.start(GPIO_NUM_13);

    WebServer server(status_queue, wifi_mgmt, blekb, servo);
    server.start();

    ESP_LOGI(TAG, "ESP32 PS5 BLE started! Web UI at http://%s/", wifi_mgmt.get_ip_str());

    xTaskCreate( [](void *arg) {
        auto *blekb = static_cast<EspidfBleKeyboard *>(arg);
        const TickType_t delay = pdMS_TO_TICKS(10);
        while (true) {
            blekb->loop();
            vTaskDelay(delay);
        }
    }, "blekb", 4096, &blekb, tskIDLE_PRIORITY + 1, nullptr);


    int i = 0;
    while (1) {
        Status current_status{};
        current_status.timenow = esp_timer_get_time();
        if (xQueueOverwrite(status_queue, &current_status) != pdPASS) {
            ESP_LOGE(TAG, "Status Queue write failed");
        }

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}
