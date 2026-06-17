#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "wifi_mgmt.h"
#include "ota.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "logger.h"

#include "web_server.h"
#include "blekb.h"
#include "servo.h"

static constexpr const char* TAG = "WEB";

/* Embedded HTML file */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

esp_err_t WebServer::root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

esp_err_t WebServer::api_key_code_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *key_code = cJSON_GetObjectItem(root, "key_code");
    if (!cJSON_IsNumber(key_code)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing key_code");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Accepted\"}");

    ESP_LOGI(TAG, "received key code: %d", key_code->valueint);

    blekb.send_key_combo(0, key_code->valueint);

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t WebServer::api_wifi_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "pass");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(pass)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid/pass");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Connecting to new network...\"}");

    wifi_mgmt.set_sta_credentials(ssid->valuestring, pass->valuestring);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t WebServer::api_ota_post_handler(httpd_req_t *req) {
    return ota_upload_handler(req);
}

esp_err_t WebServer::api_status_get_handler(httpd_req_t *req) {
    Status last_status{};
    if (xQueuePeek(status_queue, &last_status, 0) == pdTRUE) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "timenow", last_status.timenow);

        char *json_str = cJSON_Print(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        cJSON_Delete(root);
        free(json_str);
        return ESP_OK;
    } else {
        return ESP_FAIL;
    }
}

esp_err_t WebServer::api_logs_get_handler(httpd_req_t *req) {
    // Get optional query parameter for max entries
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    size_t max_entries = 0;

    if (buf_len > 1) {
        char buf[buf_len]{};
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param_value[32]{};
            if (httpd_query_key_value(buf, "max", param_value, sizeof(param_value)) == ESP_OK) {
                max_entries = atoi(param_value);
            }
        }
    }

    char *json_logs = RingBufferLogger::instance().get_logs_json(max_entries);

    if (json_logs) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_logs);
        free(json_logs);
        return ESP_OK;
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate logs JSON");
        return ESP_FAIL;
    }
}

esp_err_t WebServer::api_logs_clear_handler(httpd_req_t *req) {
    RingBufferLogger::instance().clear();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "msg", "Logs cleared");

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    cJSON_Delete(root);
    free(json_str);

    return ESP_OK;
}

esp_err_t WebServer::api_servo_press_handler(httpd_req_t *req) {
    uint32_t rest = 832, press = 1100, hold = 1000;
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            cJSON *j;
            if ((j = cJSON_GetObjectItem(root, "rest")) && cJSON_IsNumber(j)) rest = j->valueint;
            if ((j = cJSON_GetObjectItem(root, "press")) && cJSON_IsNumber(j)) press = j->valueint;
            if ((j = cJSON_GetObjectItem(root, "hold")) && cJSON_IsNumber(j)) hold = j->valueint;
            cJSON_Delete(root);
        }
    }
    servo.press(rest, press, hold);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Power button pressed\"}");
    return ESP_OK;
}

WebServer::~WebServer() {
    if (server) {
        httpd_stop(server);
        server = {};
    }
}

void WebServer::start() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 14;
    config.stack_size = 8192;

    if (server) {
        ESP_LOGE(TAG, "HTTP server already started");
        return;
    }

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    auto make_uri = [this](const char *path, httpd_method_t method, esp_err_t (*fn)(httpd_req_t *)) {
        httpd_uri_t u = {};
        u.uri = path;
        u.method = method;
        u.handler = fn;
        u.user_ctx = this;
        return u;
    };

    httpd_uri_t uris[] = {
        make_uri("/", HTTP_GET, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->root_get_handler(req);
        }),
        make_uri("/api/keycode", HTTP_POST, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_key_code_post_handler(req);
        }),
        make_uri("/api/status", HTTP_GET, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_status_get_handler(req);
        }),
        make_uri("/api/wifi/config", HTTP_POST, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_wifi_post_handler(req);
        }),
        make_uri("/api/ota/update", HTTP_POST, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_ota_post_handler(req);
        }),
        make_uri("/api/logs", HTTP_GET, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_logs_get_handler(req);
        }),
        make_uri("/api/logs/clear", HTTP_POST, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_logs_clear_handler(req);
        }),
        make_uri("/api/servo/press", HTTP_POST, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_servo_press_handler(req);
        }),
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP server started");
}
