#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "freertos/semphr.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#ifdef CONFIG_BT_CLASSIC_ENABLED
#include "esp_gap_bt_api.h"
#else
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#endif
#include "nvs_flash.h"

// Maximum number of host slots for multi-host switching
static const uint8_t MAX_HOST_SLOTS = 10;

// ── Keyboard layout abstraction ──────────────────────────────────────────────
struct HidKeyMapping {
  uint8_t modifier;  // 0x00 none, 0x02 LShift, 0x40 RAlt/AltGr, etc.
  uint8_t keycode;   // USB HID usage; 0x00 = unmapped
  uint8_t followup_keycode{0};  // 0x00 = none; else send {0, followup_keycode} after main stroke (dead-key + space)
};

struct UnicodeKeyMapping {
  uint32_t codepoint;
  uint8_t modifier;
  uint8_t keycode;
  uint8_t followup_keycode{0};  // same semantics as HidKeyMapping
};

struct KeyboardLayout {
  const char *id;                        // "us", "uk", ...
  const char *display_name;              // "English (US)"
  const HidKeyMapping *ascii_map;        // 128 entries
  const UnicodeKeyMapping *unicode_map;  // may be nullptr if unicode_map_len == 0
  size_t unicode_map_len;
};

const KeyboardLayout *get_layout_by_id(const char *id);
const KeyboardLayout *default_layout();
size_t layout_count();
const KeyboardLayout *layout_at(size_t i);

class EspidfBleKeyboard {
 public:
  void setup();
  void loop();
  void send_string(const std::string &str);
  void send_ctrl_alt_del();
  void send_key_combo(uint8_t modifiers, uint8_t keycode);
  void send_sleep();
  void send_shutdown();
  void send_hibernate();
  void send_consumer(uint16_t usage);
  void send_power();
  void send_media_play_pause();
  void send_media_next();
  void send_media_prev();
  void send_media_stop();
  void send_volume_up();
  void send_volume_down();
  void send_mute();
  void send_mouse_click(uint8_t buttons);
  void send_mouse_move(int8_t x, int8_t y);
  void send_mouse_scroll(int8_t wheel);

  void set_passkey(uint32_t passkey) {
    passkey_ = passkey;
    has_passkey_ = true;
  }
  void set_passkey_secure_connections(bool enabled) { passkey_secure_connections_ = enabled; }
  bool has_passkey() const { return has_passkey_; }
  uint32_t passkey() const { return passkey_; }
  bool passkey_secure_connections() const { return passkey_secure_connections_; }

  void set_device_name(const std::string &name) { device_name_ = name; }
  const std::string &device_name() const { return device_name_; }

  void set_key_delay_ms(uint32_t ms) { key_delay_ms_ = ms; }
  uint32_t key_delay_ms() const { return key_delay_ms_; }

  void set_web_control(bool enabled) { web_control_enabled_ = enabled; }
  void set_host_slots(uint8_t slots) { host_slots_ = slots > MAX_HOST_SLOTS ? MAX_HOST_SLOTS : slots; }

  // Keyboard layout
  void set_keyboard_layout(const std::string &id);      // YAML default
  void set_runtime_layout(const std::string &id, bool persist = true);  // web UI persists; slot-driven does not
  const KeyboardLayout *active_layout() const { return active_layout_; }
  const char *active_layout_id() const { return active_layout_ != nullptr ? active_layout_->id : "us"; }

  // Web mouse sensitivity
  void set_mouse_sensitivity(float s) { mouse_sensitivity_ = s; }
  void set_mouse_accel(float a) { mouse_accel_ = a; }
  void set_mouse_max_speed(float m) { mouse_max_speed_ = m; }
  void set_scroll_sensitivity(float s) { scroll_sensitivity_ = s; }
  float mouse_sensitivity() const { return mouse_sensitivity_; }
  float mouse_accel() const { return mouse_accel_; }
  float mouse_max_speed() const { return mouse_max_speed_; }
  float scroll_sensitivity() const { return scroll_sensitivity_; }

  struct ButtonInfo {
    std::string name;
    std::string action;
  };
  void register_button(const std::string &name, const std::string &action) {
    buttons_.push_back({name, action});
  }
  const std::vector<ButtonInfo> &get_buttons() const { return buttons_; }

  // User-editable macros (NVS-persisted, web-editable)
  static const uint8_t MAX_MACROS = 16;
  const std::vector<ButtonInfo> &get_macros() const { return macros_; }
  bool add_macro(const std::string &name, const std::string &action);
  bool update_macro(uint8_t index, const std::string &name, const std::string &action);
  bool delete_macro(uint8_t index);

  /// Execute an action string (combo:, consumer:, named actions, or literal text).
  /// Used by buttons, macros, web API, and YAML automations.
  void execute_action(const std::string &action);
  /// Execute a macro by index. Returns false if index is out of range.
  bool execute_macro(uint8_t index);

  void set_paired(bool paired) {
    is_paired_ = paired;
  }
  bool is_paired() const { return is_paired_; }

  void queue_paired_state(bool paired) {
    pending_paired_state_.store(paired);
    pending_paired_update_.store(true);
  }

  void queue_led_state(uint8_t led_byte) {
    pending_led_value_.store(led_byte);
    pending_led_update_.store(true);
  }

  void set_connected(bool connected, uint16_t conn_id) {
    is_connected_ = connected;
    conn_id_ = conn_id;
  }
  bool is_connected() const { return is_connected_; }
  uint16_t conn_id() const { return conn_id_; }

  // Multi-host switching
  void switch_host(uint8_t slot);
  void forget_host(uint8_t slot);
  uint8_t active_host_slot() const { return active_slot_; }
  uint8_t host_slots() const { return host_slots_; }

  struct HostSlotConfig {
    bool has_passkey{false};
    uint32_t passkey{0};
    bool secure_connections{false};  // true = secure_connections, false = legacy
  };

  struct HostSlot {
    bool occupied{false};
    esp_bd_addr_t addr{};
#ifndef CONFIG_BT_CLASSIC_ENABLED
    esp_ble_addr_type_t addr_type{BLE_ADDR_TYPE_PUBLIC};
#endif
    std::string name;  // friendly label
  };
  const HostSlot &get_host_slot(uint8_t slot) const { return hosts_[slot]; }
  const uint8_t *get_slot_addr(uint8_t slot) const { return slot_addrs_[slot]; }
#ifdef CONFIG_BT_CLASSIC_ENABLED
  void assign_host_slot_(uint8_t slot, const esp_bd_addr_t addr);
#else
  void assign_host_slot_(uint8_t slot, const esp_bd_addr_t addr, esp_ble_addr_type_t addr_type);
#endif
  void save_host_slots_();

  void set_host_slot_passkey(uint8_t slot, uint32_t passkey, bool secure_connections) {
    if (slot < MAX_HOST_SLOTS) {
      host_slot_configs_[slot].has_passkey = true;
      host_slot_configs_[slot].passkey = passkey;
      host_slot_configs_[slot].secure_connections = secure_connections;
    }
  }
  bool get_active_slot_passkey(bool &has_passkey, uint32_t &passkey, bool &secure_connections) const;
  const HostSlotConfig &get_host_slot_config(uint8_t slot) const { return host_slot_configs_[slot]; }

  void set_host_slot_layout(uint8_t slot, const std::string &id) {
    if (slot < MAX_HOST_SLOTS) slot_layout_id_[slot] = id;
  }
  const std::string &get_host_slot_layout(uint8_t slot) const { return slot_layout_id_[slot]; }

  // Custom text entities
#ifdef USE_TEXT
  void add_custom_text(text::Text *t) { custom_texts_.push_back(t); }
  const std::vector<text::Text *> &get_custom_texts() const { return custom_texts_; }
#endif

  void set_rssi_update_interval(uint32_t ms) { rssi_update_interval_ms_ = ms; }
  void update_rssi(int8_t rssi);
  void add_rssi_above_callback(std::function<void(int8_t)> cb) { rssi_above_callbacks_.push_back(std::move(cb)); }
  void add_rssi_below_callback(std::function<void(int8_t)> cb) { rssi_below_callbacks_.push_back(std::move(cb)); }

  // Peer address and RSSI state — public so static GAP/GATTS handlers can access them directly
  esp_bd_addr_t peer_addr_{};
  bool rssi_pending_{false};
  std::atomic<bool> pending_rssi_nan_{false};
  std::atomic<bool> pending_rssi_update_{false};
  std::atomic<int8_t> pending_rssi_value_{0};

 protected:
  bool is_connected_{false};
  uint16_t conn_id_{0};
  bool is_paired_{false};
  std::atomic<bool> pending_paired_update_{false};
  std::atomic<bool> pending_paired_state_{false};
  std::atomic<bool> pending_led_update_{false};
  std::atomic<uint8_t> pending_led_value_{0};
  uint32_t passkey_{0};
  bool has_passkey_{false};
  bool passkey_secure_connections_{false};
  std::string device_name_{"ESP32 BLE KB"};
  uint32_t key_delay_ms_{80};

  bool web_control_enabled_{false};
  float mouse_sensitivity_{1.0f};
  float mouse_accel_{0.15f};
  float mouse_max_speed_{4.0f};
  float scroll_sensitivity_{2.0f};
  std::vector<ButtonInfo> buttons_;
  std::vector<ButtonInfo> macros_;   // user-editable, NVS-persisted
  void load_macros_();
  void save_macros_();

  // Multi-host state
  uint8_t host_slots_{MAX_HOST_SLOTS};
  uint8_t active_slot_{0};
  HostSlot hosts_[MAX_HOST_SLOTS];
  HostSlotConfig host_slot_configs_[MAX_HOST_SLOTS]{};
  std::string slot_layout_id_[MAX_HOST_SLOTS]{};  // YAML per-slot layout; empty = no override
  esp_bd_addr_t slot_addrs_[MAX_HOST_SLOTS]{};  // per-slot random BLE address
  void load_host_slots_();
  void generate_slot_addrs_();

#ifdef USE_BLE_KEYBOARD_WEB_CONTROL
  web_server_base::WebServerBase *web_server_base_{nullptr};
  BleKeyboardWebControl *web_control_{nullptr};
#endif

  // RSSI state (interval/timing/callbacks stay protected — only touched by member functions)
  uint32_t rssi_update_interval_ms_{10000};
  uint32_t rssi_last_poll_ms_{0};
  std::vector<std::function<void(int8_t)>> rssi_above_callbacks_;
  std::vector<std::function<void(int8_t)>> rssi_below_callbacks_;

  // Keyboard layout state
  const KeyboardLayout *active_layout_{nullptr};
  std::string yaml_layout_id_{"us"};
  void load_layout_();
  void save_layout_(const std::string &id);
  void update_led_state_(uint8_t led_byte);

  // Non-blocking string typing state machine (driven from loop())
  // Keystrokes are pre-resolved (UTF-8 decoded + layout-mapped) at enqueue time,
  // so a mid-type layout switch can't garble already-queued text.
  SemaphoreHandle_t type_mutex_{nullptr};
  std::vector<HidKeyMapping> type_queue_;
  size_t type_index_{0};
  bool type_key_up_pending_{false};
  uint32_t type_next_ms_{0};

  // Dedup guard — ESPHome API can deliver service calls twice
  uint32_t last_send_string_ms_{0};
  std::string last_send_string_;
  uint32_t last_send_key_ms_{0};
  uint16_t last_send_key_id_{0};  // (modifier << 8) | keycode
  uint32_t last_consumer_ms_{0};
  uint16_t last_consumer_usage_{0};
  uint32_t last_mouse_click_ms_{0};
  uint8_t last_mouse_click_{0};
#ifdef USE_TEXT
  std::vector<text::Text *> custom_texts_;
#endif
};

class EspidfBleKeyboardButton {
 public:
  void set_parent(EspidfBleKeyboard *parent) { parent_ = parent; }
  void press_action();
  void set_action(const std::string &action) { action_ = action; }
 protected:
  EspidfBleKeyboard *parent_{nullptr};
  std::string action_;
};
