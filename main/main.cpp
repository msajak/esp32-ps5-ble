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
#include "cdisplay.h"
#include "blekb.h"
#include "logger.h"

// ESP-IDF entry point
extern "C" {
    void app_main(void);
}

QueueHandle_t params_queue;
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

    params_queue = xQueueCreate(1, sizeof(Params));
    if (!params_queue) {
        ESP_LOGE(TAG, "Failed to create params queue");
        return;
    }

    status_queue = xQueueCreate(1, sizeof(Status));
    if (!status_queue) {
        ESP_LOGE(TAG, "Failed to create status queue");
        return;
    }

    WifiMgmt wifi_mgmt;
    wifi_mgmt.start();

    WebServer server(params_queue, status_queue, wifi_mgmt);
    server.start();

    ESP_LOGI(TAG, "ESP32 PS5 BLE started! Web UI at http://%s/", wifi_mgmt.get_ip());

    EspidfBleKeyboard blekb;
    blekb.setup();
    xTaskCreate( [](void *arg) {
        auto *blekb = static_cast<EspidfBleKeyboard *>(arg);
        const TickType_t delay = pdMS_TO_TICKS(10);
        while (true) {
            blekb->loop();
            vTaskDelay(delay);
        }
    }, "blekb", 4096, &blekb, tskIDLE_PRIORITY + 1, nullptr);

    CDisplay display;
    display.begin();

    char ip[20]; sprintf(ip, "%s", wifi_mgmt.get_ip());
    display.clear();
    display.text(ip, 0, 12);
    display.disp();

    int i = 0;
    while (1) {
        // for (int y=0; y<40; y+= 8)
        //   for (int x=(y&8); x<60; x+= 16)
        //     display.rect(x, y, 8, 8);
        char t[20]; sprintf(t, "test %d", i); display.text(t, 0, 32);
        // display.rect(60, 5, 10, 10);
        // display.rect(61, 6, 8, 8, false);
        // display.rect(62, 7, 6, 6);
        display.disp();
        ++i;

        // blekb.send_string("1234");

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}
