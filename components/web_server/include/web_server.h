#pragma once

#include "esp_http_server.h"
#include "esp_err.h"
#include "shared_objs.h"

class Params;
class WifiMgmt;
class EspidfBleKeyboard;
class Servo;

class WebServer {

    httpd_handle_t server{};

    esp_err_t root_get_handler(httpd_req_t *req);
    esp_err_t api_key_code_post_handler(httpd_req_t *req);
    esp_err_t api_wifi_post_handler(httpd_req_t *req);
    esp_err_t api_ota_post_handler(httpd_req_t *req);
    esp_err_t api_status_get_handler(httpd_req_t *req);
    esp_err_t api_logs_get_handler(httpd_req_t *req);
    esp_err_t api_logs_clear_handler(httpd_req_t *req);
    esp_err_t api_servo_press_handler(httpd_req_t *req);

public:
    WebServer(QueueHandle_t& status_queue, WifiMgmt& wifi_mgmt, EspidfBleKeyboard& blekb, Servo& servo):
        status_queue(status_queue), wifi_mgmt(wifi_mgmt), blekb(blekb), servo(servo) {}
    ~WebServer();

    void start();

    QueueHandle_t& status_queue;
    WifiMgmt& wifi_mgmt;
    EspidfBleKeyboard& blekb;
    Servo& servo;
    Status last_status;
};
