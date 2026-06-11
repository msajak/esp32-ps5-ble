/**
 * Classic Bluetooth HID Keyboard — ESP-IDF BT HID Device profile.
 * PS5-compatible alternative to the BLE (HOGP) implementation in blekb.cpp.
 * Selected at build time via CONFIG_BT_CLASSIC_ENABLED in sdkconfig.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt_device.h"
#include "esp_hidd_api.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <cstring>
#include <cstdio>
#include <vector>

#include "blekb.h"

static const char *TAG = "BTCKB";
static EspidfBleKeyboard *s_instance = nullptr;

static uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint8_t s_protocol_mode = 1; // 0=Boot, 1=Report
static bool s_app_registered = false;

static esp_err_t send_keyboard_input_report(const uint8_t *report, uint16_t len);

// ── HID Report Descriptor ────────────────────────────────────────────────────
static const uint8_t hid_report_map[] = {
    // Keyboard (Report ID 1)
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    // Consumer Control (Report ID 2)
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0x85, 0x02,
    0x15, 0x00, 0x26, 0xFF, 0x03,
    0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
    // System Control (Report ID 3)
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01,
    0x85, 0x03,
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x19, 0x00, 0x29, 0xFF,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x00,
    0xC0,
    // Mouse (Report ID 4)
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x85, 0x04, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
    0x75, 0x05, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0
};

static esp_hidd_app_param_t s_app_param = {};
static esp_hidd_qos_param_t s_in_qos = {};
static esp_hidd_qos_param_t s_out_qos = {};

// ── Classic BT GAP Callback ─────────────────────────────────────────────────
static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    ESP_LOGI(TAG, "GAP: ev %d", (int)event);
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "GAP: Auth success: %s", param->auth_cmpl.device_name);
                if (s_instance) {
                    s_instance->queue_paired_state(true);
                    s_instance->assign_host_slot_(s_instance->active_host_slot(),
                                                   param->auth_cmpl.bda);
                    s_instance->save_host_slots_();
                }
            } else {
                ESP_LOGE(TAG, "GAP: Auth failed (status=%d)", param->auth_cmpl.stat);
            }
            break;
        case ESP_BT_GAP_PIN_REQ_EVT: {
            ESP_LOGI(TAG, "GAP: Legacy PIN request");
            esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
            break;
        }
        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI(TAG, "GAP: Confirm request: %06lu", (unsigned long)param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "GAP: Passkey: %06lu", (unsigned long)param->key_notif.passkey);
            break;
        case ESP_BT_GAP_KEY_REQ_EVT:
            ESP_LOGI(TAG, "GAP: Passkey request");
            break;
        case ESP_BT_GAP_MODE_CHG_EVT:
            ESP_LOGD(TAG, "GAP: Mode change (mode=%d)", param->mode_chg.mode);
            break;
        default:
            break;
    }
}

// ── Classic BT HID Device Callback ──────────────────────────────────────────
static void hidd_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param) {
    ESP_LOGI(TAG, "HIDD: ev %d", (int)event);
    switch (event) {
        case ESP_HIDD_INIT_EVT:
            if (param->init.status == ESP_HIDD_SUCCESS) {
                ESP_LOGI(TAG, "HIDD: Initialized, registering app");
                s_app_param.name = s_instance ? s_instance->device_name().c_str() : "ESP32 KB";
                s_app_param.description = "Keyboard";
                s_app_param.provider = "Espressif";
                s_app_param.subclass = 0x40;
                s_app_param.desc_list = const_cast<uint8_t *>(hid_report_map);
                s_app_param.desc_list_len = sizeof(hid_report_map);
                memset(&s_in_qos, 0, sizeof(s_in_qos));
                memset(&s_out_qos, 0, sizeof(s_out_qos));
                esp_bt_hid_device_register_app(&s_app_param, &s_in_qos, &s_out_qos);
            } else {
                ESP_LOGE(TAG, "HIDD: Init failed (%d)", param->init.status);
            }
            break;
        case ESP_HIDD_REGISTER_APP_EVT:
            if (param->register_app.status == ESP_HIDD_SUCCESS) {
                ESP_LOGI(TAG, "HIDD: App registered, discoverable");
                s_app_registered = true;
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            } else {
                ESP_LOGE(TAG, "HIDD: App register failed (%d)", param->register_app.status);
            }
            break;
        case ESP_HIDD_OPEN_EVT:
            if (param->open.status == ESP_HIDD_SUCCESS) {
                ESP_LOGI(TAG, "HIDD: Connected to %02X:%02X:%02X:%02X:%02X:%02X",
                         param->open.bd_addr[0], param->open.bd_addr[1],
                         param->open.bd_addr[2], param->open.bd_addr[3],
                         param->open.bd_addr[4], param->open.bd_addr[5]);
                if (s_instance) {
                    s_instance->set_connected(true, 0);
                    memcpy(s_instance->peer_addr_, param->open.bd_addr, sizeof(esp_bd_addr_t));
                }
                s_protocol_mode = 1;
                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            }
            break;
        case ESP_HIDD_CLOSE_EVT:
            ESP_LOGI(TAG, "HIDD: Disconnected");
            if (s_instance) {
                s_instance->set_connected(false, 0);
                s_instance->queue_paired_state(false);
            }
            s_protocol_mode = 1;
            if (s_app_registered)
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            break;
        case ESP_HIDD_SEND_REPORT_EVT:
            if (param->send_report.status != ESP_HIDD_SUCCESS)
                ESP_LOGW(TAG, "HIDD: Report send failed (%d)", param->send_report.status);
            break;
        case ESP_HIDD_SET_PROTOCOL_EVT:
            s_protocol_mode = param->set_protocol.protocol_mode;
            ESP_LOGI(TAG, "HIDD: Protocol=%s", s_protocol_mode == 0 ? "Boot" : "Report");
            break;
        case ESP_HIDD_GET_REPORT_EVT: {
            uint8_t zero[8] = {0};
            uint8_t rid = param->get_report.report_id;
            uint16_t len = (rid == 2) ? 2 : (rid == 3) ? 1 : (rid == 4) ? 4 : 8;
            esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INPUT, rid, len, zero);
            break;
        }
        case ESP_HIDD_SET_REPORT_EVT:
            if (param->set_report.len > 0 && s_instance)
                s_instance->queue_led_state(param->set_report.data[0]);
            break;
        case ESP_HIDD_INTR_DATA_EVT:
            if (param->intr_data.len > 0 && s_instance)
                s_instance->queue_led_state(param->intr_data.data[0]);
            break;
        default:
            break;
    }
}

// ── Send Keyboard Report ────────────────────────────────────────────────────
static esp_err_t send_keyboard_input_report(const uint8_t *report, uint16_t len) {
    if (!s_instance || !s_instance->is_connected()) return ESP_FAIL;
    uint8_t report_id = (s_protocol_mode == 0) ? 0 : 1;
    return esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA,
                                          report_id, len, const_cast<uint8_t *>(report));
}

// ── Multi-Host Slot Management ──────────────────────────────────────────────

void EspidfBleKeyboard::generate_slot_addrs_() {
    const uint8_t *own = esp_bt_dev_get_address();
    if (own) {
        for (uint8_t i = 0; i < MAX_HOST_SLOTS; i++)
            memcpy(slot_addrs_[i], own, sizeof(esp_bd_addr_t));
    }
}

void EspidfBleKeyboard::load_host_slots_() {
    nvs_handle_t handle;
    if (nvs_open("espidf_ble_kb", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t slot_count = 0;
    if (nvs_get_u8(handle, "host_cnt", &slot_count) == ESP_OK) {
        for (uint8_t i = 0; i < slot_count && i < MAX_HOST_SLOTS; i++) {
            char key[16];
            snprintf(key, sizeof(key), "host%u_addr", i);
            size_t len = sizeof(esp_bd_addr_t);
            if (nvs_get_blob(handle, key, hosts_[i].addr, &len) == ESP_OK) {
                hosts_[i].occupied = true;
                ESP_LOGI(TAG, "Loaded host slot %u: %02X:%02X:%02X:%02X:%02X:%02X", i,
                         hosts_[i].addr[0], hosts_[i].addr[1], hosts_[i].addr[2],
                         hosts_[i].addr[3], hosts_[i].addr[4], hosts_[i].addr[5]);
            }
        }
    }
    uint8_t active = 0;
    if (nvs_get_u8(handle, "host_act", &active) == ESP_OK && active < MAX_HOST_SLOTS)
        active_slot_ = active;
    nvs_close(handle);
}

void EspidfBleKeyboard::save_host_slots_() {
    nvs_handle_t handle;
    if (nvs_open("espidf_ble_kb", NVS_READWRITE, &handle) != ESP_OK) return;
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_HOST_SLOTS; i++) {
        char key[16];
        if (hosts_[i].occupied) {
            snprintf(key, sizeof(key), "host%u_addr", i);
            nvs_set_blob(handle, key, hosts_[i].addr, sizeof(esp_bd_addr_t));
            count = i + 1;
        } else {
            snprintf(key, sizeof(key), "host%u_addr", i);
            nvs_erase_key(handle, key);
        }
    }
    nvs_set_u8(handle, "host_cnt", count);
    nvs_set_u8(handle, "host_act", active_slot_);
    nvs_commit(handle);
    nvs_close(handle);
}

void EspidfBleKeyboard::assign_host_slot_(uint8_t slot, const esp_bd_addr_t addr) {
    if (slot >= MAX_HOST_SLOTS) return;
    for (uint8_t i = 0; i < MAX_HOST_SLOTS; i++) {
        if (hosts_[i].occupied && memcmp(hosts_[i].addr, addr, sizeof(esp_bd_addr_t)) == 0) {
            if (i == slot) return;
            hosts_[i].occupied = false;
            break;
        }
    }
    memcpy(hosts_[slot].addr, addr, sizeof(esp_bd_addr_t));
    hosts_[slot].occupied = true;
    ESP_LOGI(TAG, "Host slot %u assigned: %02X:%02X:%02X:%02X:%02X:%02X", slot,
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

void EspidfBleKeyboard::switch_host(uint8_t slot) {
    if (slot >= host_slots_) {
        ESP_LOGW(TAG, "Invalid host slot %u (max %u)", slot, host_slots_ - 1);
        return;
    }
    active_slot_ = slot;
    save_host_slots_();
    if (!slot_layout_id_[slot].empty()) set_runtime_layout(slot_layout_id_[slot], false);
    ESP_LOGI(TAG, "Switching to host slot %u", slot);
    if (is_connected_) esp_bt_hid_device_disconnect();
    if (hosts_[slot].occupied)
        esp_bt_hid_device_connect(hosts_[slot].addr);
    else
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}

void EspidfBleKeyboard::forget_host(uint8_t slot) {
    if (slot >= MAX_HOST_SLOTS || !hosts_[slot].occupied) return;
    ESP_LOGI(TAG, "Forgetting host slot %u", slot);
    esp_bt_gap_remove_bond_device(hosts_[slot].addr);
    hosts_[slot].occupied = false;
    memset(hosts_[slot].addr, 0, sizeof(esp_bd_addr_t));
    hosts_[slot].name.clear();
    save_host_slots_();
    if (slot == active_slot_ && is_connected_) esp_bt_hid_device_disconnect();
}

bool EspidfBleKeyboard::get_active_slot_passkey(bool &has_passkey, uint32_t &passkey, bool &secure_connections) const {
    has_passkey = has_passkey_;
    passkey = passkey_;
    secure_connections = passkey_secure_connections_;
    return has_passkey_;
}

void EspidfBleKeyboard::update_rssi(int8_t rssi) {
    for (auto &cb : rssi_above_callbacks_) cb(rssi);
    for (auto &cb : rssi_below_callbacks_) cb(rssi);
}

// ── Macros (NVS-persisted) ──────────────────────────────────────────────────

void EspidfBleKeyboard::load_macros_() {
    nvs_handle_t handle;
    if (nvs_open("espidf_ble_kb", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t count = 0;
    if (nvs_get_u8(handle, "macro_cnt", &count) == ESP_OK) {
        for (uint8_t i = 0; i < count && i < MAX_MACROS; i++) {
            char key[16]; char buf[256]; size_t len;
            std::string name, action;
            snprintf(key, sizeof(key), "macro%u_name", i);
            len = sizeof(buf);
            if (nvs_get_str(handle, key, buf, &len) == ESP_OK) name = buf;
            snprintf(key, sizeof(key), "macro%u_act", i);
            len = sizeof(buf);
            if (nvs_get_str(handle, key, buf, &len) == ESP_OK) action = buf;
            if (!name.empty() && !action.empty()) macros_.push_back({name, action});
        }
    }
    nvs_close(handle);
}

void EspidfBleKeyboard::save_macros_() {
    nvs_handle_t handle;
    if (nvs_open("espidf_ble_kb", NVS_READWRITE, &handle) != ESP_OK) return;
    uint8_t count = macros_.size();
    nvs_set_u8(handle, "macro_cnt", count);
    for (uint8_t i = 0; i < MAX_MACROS; i++) {
        char key[16];
        if (i < count) {
            snprintf(key, sizeof(key), "macro%u_name", i);
            nvs_set_str(handle, key, macros_[i].name.c_str());
            snprintf(key, sizeof(key), "macro%u_act", i);
            nvs_set_str(handle, key, macros_[i].action.c_str());
        } else {
            snprintf(key, sizeof(key), "macro%u_name", i);
            nvs_erase_key(handle, key);
            snprintf(key, sizeof(key), "macro%u_act", i);
            nvs_erase_key(handle, key);
        }
    }
    nvs_commit(handle);
    nvs_close(handle);
}

bool EspidfBleKeyboard::add_macro(const std::string &name, const std::string &action) {
    if (macros_.size() >= MAX_MACROS) return false;
    macros_.push_back({name, action});
    save_macros_();
    return true;
}

bool EspidfBleKeyboard::update_macro(uint8_t index, const std::string &name, const std::string &action) {
    if (index >= macros_.size()) return false;
    macros_[index].name = name;
    macros_[index].action = action;
    save_macros_();
    return true;
}

bool EspidfBleKeyboard::delete_macro(uint8_t index) {
    if (index >= macros_.size()) return false;
    macros_.erase(macros_.begin() + index);
    save_macros_();
    return true;
}

// ── Setup ───────────────────────────────────────────────────────────────────
void EspidfBleKeyboard::setup() {
    s_instance = this;
    type_mutex_ = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    load_macros_();
    if (active_layout_ == nullptr) active_layout_ = default_layout();
    load_layout_();
    load_host_slots_();
    if (!slot_layout_id_[active_slot_].empty())
        set_runtime_layout(slot_layout_id_[active_slot_], false);
    generate_slot_addrs_();

    esp_bt_gap_set_device_name(device_name_.c_str());
    esp_bt_gap_register_callback(bt_gap_cb);

    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(iocap));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    esp_bt_hid_device_register_callback(hidd_cb);
    esp_bt_hid_device_init();

    set_connected(false, 0);
    set_paired(false);

    ESP_LOGI(TAG, "Classic BT HID keyboard initialized: \"%s\"", device_name_.c_str());
}

void EspidfBleKeyboard::update_led_state_(uint8_t) {}

// ── Loop ────────────────────────────────────────────────────────────────────
void EspidfBleKeyboard::loop() {
    if (pending_paired_update_.exchange(false))
        set_paired(pending_paired_state_.load());
    if (pending_led_update_.exchange(false))
        update_led_state_(pending_led_value_.load());
    if (pending_rssi_nan_.exchange(false)) {}
    if (pending_rssi_update_.exchange(false))
        update_rssi(pending_rssi_value_.load());

    if (!is_connected_ || type_mutex_ == nullptr) return;
    uint32_t now = millis();
    if (now < type_next_ms_) return;

    if (xSemaphoreTake(type_mutex_, 0) != pdTRUE) return;
    bool queue_empty = type_queue_.empty();
    bool key_up = type_key_up_pending_;
    HidKeyMapping mapping{0, 0, 0};
    if (!queue_empty && !key_up) mapping = type_queue_[type_index_];
    xSemaphoreGive(type_mutex_);

    if (queue_empty) return;

    uint8_t report[8] = {0};
    uint32_t half_delay = key_delay_ms_ / 2;

    if (key_up) {
        if (send_keyboard_input_report(report, 8) != ESP_OK) return;
        xSemaphoreTake(type_mutex_, portMAX_DELAY);
        type_key_up_pending_ = false;
        type_index_++;
        if (type_index_ >= type_queue_.size()) {
            type_queue_.clear();
            type_index_ = 0;
        }
        type_next_ms_ = now + half_delay;
        xSemaphoreGive(type_mutex_);
    } else {
        report[0] = mapping.modifier;
        report[2] = mapping.keycode;
        if (send_keyboard_input_report(report, 8) != ESP_OK) return;
        xSemaphoreTake(type_mutex_, portMAX_DELAY);
        type_key_up_pending_ = true;
        type_next_ms_ = now + half_delay;
        xSemaphoreGive(type_mutex_);
    }
}

// ── UTF-8 Decoding & Layout Resolution ──────────────────────────────────────

static uint32_t decode_utf8_(const std::string &bytes, size_t &i) {
    uint8_t b0 = static_cast<uint8_t>(bytes[i]);
    if (b0 < 0x80) { i++; return b0; }
    auto cont = [&](size_t off) -> int {
        if (i + off >= bytes.size()) return -1;
        uint8_t b = static_cast<uint8_t>(bytes[i + off]);
        return ((b & 0xC0) == 0x80) ? (b & 0x3F) : -1;
    };
    if ((b0 & 0xE0) == 0xC0) {
        int c1 = cont(1);
        if (c1 < 0) { i++; return 0xFFFD; }
        i += 2; return ((b0 & 0x1F) << 6) | c1;
    }
    if ((b0 & 0xF0) == 0xE0) {
        int c1 = cont(1), c2 = cont(2);
        if (c1 < 0 || c2 < 0) { i++; return 0xFFFD; }
        i += 3; return ((b0 & 0x0F) << 12) | (c1 << 6) | c2;
    }
    if ((b0 & 0xF8) == 0xF0) {
        int c1 = cont(1), c2 = cont(2), c3 = cont(3);
        if (c1 < 0 || c2 < 0 || c3 < 0) { i++; return 0xFFFD; }
        i += 4; return ((b0 & 0x07) << 18) | (c1 << 12) | (c2 << 6) | c3;
    }
    i++; return 0xFFFD;
}

static HidKeyMapping resolve_codepoint_(const KeyboardLayout *layout, uint32_t cp) {
    if (layout == nullptr) return {0, 0, 0};
    if (cp < 128) return layout->ascii_map[cp];
    for (size_t i = 0; i < layout->unicode_map_len; i++) {
        if (layout->unicode_map[i].codepoint == cp)
            return {layout->unicode_map[i].modifier, layout->unicode_map[i].keycode,
                    layout->unicode_map[i].followup_keycode};
    }
    return {0, 0, 0};
}

// ── Send Functions ──────────────────────────────────────────────────────────

void EspidfBleKeyboard::send_string(const std::string &str) {
    uint32_t now = millis();
    if (str == last_send_string_ && (now - last_send_string_ms_) < 30) return;
    last_send_string_ = str;
    last_send_string_ms_ = now;
    if (type_mutex_ == nullptr) return;
    if (active_layout_ == nullptr) active_layout_ = default_layout();

    std::vector<HidKeyMapping> strokes;
    strokes.reserve(str.size());
    size_t i = 0;
    while (i < str.size()) {
        uint32_t cp = decode_utf8_(str, i);
        HidKeyMapping m = resolve_codepoint_(active_layout_, cp);
        if (m.keycode != 0x00) {
            strokes.push_back(m);
            if (m.followup_keycode != 0x00)
                strokes.push_back({0x00, m.followup_keycode, 0x00});
        }
    }

    xSemaphoreTake(type_mutex_, portMAX_DELAY);
    type_queue_.insert(type_queue_.end(), strokes.begin(), strokes.end());
    xSemaphoreGive(type_mutex_);
}

void EspidfBleKeyboard::send_key_combo(uint8_t modifiers, uint8_t keycode) {
    uint32_t now = millis();
    uint16_t key_id = ((uint16_t) modifiers << 8) | keycode;
    if (key_id == last_send_key_id_ && (now - last_send_key_ms_) < 30) return;
    last_send_key_id_ = key_id;
    last_send_key_ms_ = now;
    if (!is_connected_) return;
    uint8_t report[8] = {0};
    report[0] = modifiers;
    report[2] = keycode;
    send_keyboard_input_report(report, 8);
    vTaskDelay(pdMS_TO_TICKS(30));
    memset(report, 0, 8);
    send_keyboard_input_report(report, 8);
}

void EspidfBleKeyboard::send_ctrl_alt_del() {
    if (!is_connected_) return;
    uint8_t report[8] = {0};
    report[0] = 0x05; report[2] = 0x4C;
    send_keyboard_input_report(report, 8);
    vTaskDelay(pdMS_TO_TICKS(50));
    memset(report, 0, 8);
    send_keyboard_input_report(report, 8);
}

void EspidfBleKeyboard::send_consumer(uint16_t usage) {
    if (!is_connected_) return;
    uint32_t now = millis();
    if (usage == last_consumer_usage_ && (now - last_consumer_ms_) < 30) return;
    last_consumer_usage_ = usage;
    last_consumer_ms_ = now;
    uint8_t report[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x02, 2, report);
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t release[2] = {0, 0};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x02, 2, release);
}

void EspidfBleKeyboard::send_sleep() {
    if (!is_connected_) return;
    uint8_t report[1] = {0x82};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x03, 1, report);
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t release[1] = {0};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x03, 1, release);
}

void EspidfBleKeyboard::send_shutdown() {
    if (!is_connected_) return;
    send_power();
}

void EspidfBleKeyboard::send_power() {
    if (!is_connected_) return;
    uint8_t report[1] = {0x81};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x03, 1, report);
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t release[1] = {0};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x03, 1, release);
}

void EspidfBleKeyboard::send_media_play_pause() { send_consumer(0x00CD); }
void EspidfBleKeyboard::send_media_next()        { send_consumer(0x00B5); }
void EspidfBleKeyboard::send_media_prev()        { send_consumer(0x00B6); }
void EspidfBleKeyboard::send_media_stop()        { send_consumer(0x00B7); }
void EspidfBleKeyboard::send_volume_up()         { send_consumer(0x00E9); }
void EspidfBleKeyboard::send_volume_down()       { send_consumer(0x00EA); }
void EspidfBleKeyboard::send_mute()              { send_consumer(0x00E2); }

void EspidfBleKeyboard::send_mouse_click(uint8_t buttons) {
    if (!is_connected_) return;
    uint32_t now = millis();
    if (buttons == last_mouse_click_ && (now - last_mouse_click_ms_) < 30) return;
    last_mouse_click_ = buttons;
    last_mouse_click_ms_ = now;
    uint8_t report[4] = {buttons, 0, 0, 0};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x04, 4, report);
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t release[4] = {0};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x04, 4, release);
}

void EspidfBleKeyboard::send_mouse_move(int8_t x, int8_t y) {
    if (!is_connected_) return;
    uint8_t report[4] = {0, static_cast<uint8_t>(x), static_cast<uint8_t>(y), 0};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x04, 4, report);
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t release[4] = {0};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x04, 4, release);
}

void EspidfBleKeyboard::send_mouse_scroll(int8_t wheel) {
    if (!is_connected_) return;
    uint8_t report[4] = {0, 0, 0, static_cast<uint8_t>(wheel)};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x04, 4, report);
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t release[4] = {0};
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x04, 4, release);
}

void EspidfBleKeyboard::send_hibernate() {
    if (!is_connected_) return;
    send_key_combo(0x08, 0x15);
    vTaskDelay(pdMS_TO_TICKS(600));
    send_string("shutdown /h\n");
}

// ── Keyboard Layout ─────────────────────────────────────────────────────────

void EspidfBleKeyboard::set_keyboard_layout(const std::string &id) {
    yaml_layout_id_ = id;
    const KeyboardLayout *lay = get_layout_by_id(id.c_str());
    active_layout_ = lay != nullptr ? lay : default_layout();
}

void EspidfBleKeyboard::set_runtime_layout(const std::string &id, bool persist) {
    const KeyboardLayout *lay = get_layout_by_id(id.c_str());
    if (lay == nullptr) return;
    active_layout_ = lay;
    if (persist) save_layout_(id);
}

void EspidfBleKeyboard::load_layout_() {
    nvs_handle_t handle;
    if (nvs_open("espidf_ble_kb", NVS_READONLY, &handle) != ESP_OK) return;
    char buf[16]; size_t len = sizeof(buf);
    if (nvs_get_str(handle, "layout", buf, &len) == ESP_OK) {
        const KeyboardLayout *lay = get_layout_by_id(buf);
        if (lay != nullptr) active_layout_ = lay;
    }
    nvs_close(handle);
}

void EspidfBleKeyboard::save_layout_(const std::string &id) {
    nvs_handle_t handle;
    if (nvs_open("espidf_ble_kb", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_str(handle, "layout", id.c_str());
    nvs_commit(handle);
    nvs_close(handle);
}

// ── Action Executor ─────────────────────────────────────────────────────────

void EspidfBleKeyboard::execute_action(const std::string &action) {
    if (action.find('|') != std::string::npos) {
        size_t start = 0;
        while (start < action.size()) {
            size_t end = action.find('|', start);
            if (end == std::string::npos) end = action.size();
            std::string step = action.substr(start, end - start);
            while (!step.empty() && step.front() == ' ') step.erase(step.begin());
            while (!step.empty() && step.back() == ' ') step.pop_back();
            if (!step.empty()) execute_action(step);
            start = end + 1;
            if (start < action.size() && step.find("delay:") != 0)
                vTaskDelay(pdMS_TO_TICKS(50));
        }
        return;
    }
    if (action.find("delay:") == 0) {
        int ms = 0;
        if (sscanf(action.c_str(), "delay:%i", &ms) == 1 && ms > 0 && ms <= 10000)
            vTaskDelay(pdMS_TO_TICKS(ms));
        return;
    }
    if (action.find("combo:") == 0) {
        int mod = 0, key = 0;
        if (sscanf(action.c_str(), "combo:%i:%i", &mod, &key) == 2)
            send_key_combo((uint8_t) mod, (uint8_t) key);
        return;
    }
    if (action.find("consumer:") == 0) {
        int usage = 0;
        if (sscanf(action.c_str(), "consumer:%i", &usage) == 1)
            send_consumer((uint16_t) usage);
        return;
    }
    if (action.find("mouse_click:") == 0) {
        int buttons = 0;
        if (sscanf(action.c_str(), "mouse_click:%i", &buttons) == 1)
            send_mouse_click((uint8_t) buttons);
        return;
    }
    if (action.find("mouse_move:") == 0) {
        int x = 0, y = 0;
        if (sscanf(action.c_str(), "mouse_move:%i:%i", &x, &y) == 2)
            send_mouse_move((int8_t) x, (int8_t) y);
        return;
    }
    if (action.find("mouse_scroll:") == 0) {
        int wheel = 0;
        if (sscanf(action.c_str(), "mouse_scroll:%i", &wheel) == 1)
            send_mouse_scroll((int8_t) wheel);
        return;
    }
    if (action.find("switch_host:") == 0) {
        int slot = 0;
        if (sscanf(action.c_str(), "switch_host:%i", &slot) == 1)
            switch_host((uint8_t) slot);
        return;
    }
    if (action.find("forget_host:") == 0) {
        int slot = 0;
        if (sscanf(action.c_str(), "forget_host:%i", &slot) == 1)
            forget_host((uint8_t) slot);
        return;
    }

    if (action == "ctrl_alt_del")      send_ctrl_alt_del();
    else if (action == "sleep")        send_sleep();
    else if (action == "shutdown")     send_shutdown();
    else if (action == "hibernate")    send_hibernate();
    else if (action == "power")        send_power();
    else if (action == "play_pause")   send_media_play_pause();
    else if (action == "next_track")   send_media_next();
    else if (action == "prev_track")   send_media_prev();
    else if (action == "stop")         send_media_stop();
    else if (action == "volume_up")    send_volume_up();
    else if (action == "volume_down")  send_volume_down();
    else if (action == "mute")         send_mute();
    else if (action == "left_click")   send_mouse_click(0x01);
    else if (action == "right_click")  send_mouse_click(0x02);
    else if (action == "middle_click") send_mouse_click(0x04);
#ifdef USE_TEXT
    else if (action == "send_custom_text" || action.find("send_custom_text:") == 0) {
        int idx = 0;
        if (action.find("send_custom_text:") == 0)
            sscanf(action.c_str(), "send_custom_text:%i", &idx);
        if (idx >= 0 && idx < (int) custom_texts_.size() && !custom_texts_[idx]->state.empty())
            send_string(custom_texts_[idx]->state);
    }
#endif
    else if (action.find("string:") == 0) send_string(action.substr(7));
    else send_string(action);
}

bool EspidfBleKeyboard::execute_macro(uint8_t index) {
    if (index >= macros_.size()) return false;
    execute_action(macros_[index].action);
    return true;
}

void EspidfBleKeyboardButton::press_action() {
    if (!parent_) return;
    parent_->execute_action(action_);
}
