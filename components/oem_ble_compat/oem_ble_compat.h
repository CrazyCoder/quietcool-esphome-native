// OEM BLE compatibility component — reimplements the stock QuietCool V2 BLE
// protocol so the OEM Smart Control app can discover, pair, and control our ESPHome
// firmware exactly like stock. Single GATT service (000000ff-...) with one
// characteristic (0000ff01-..., write+notify). Runtime-toggleable via HA switch.

#pragma once

#include <cstring>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/esp32_ble_server/ble_server.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text/text.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/http_request/ota/ota_http_request.h"

#include "oem_ble_compat_logic.h"
#include "../fan_controller/fan_controller_logic.h"  // qc::SmartThreshold

#include <esp_gap_ble_api.h>

struct cJSON;  // forward — full definition in <cJSON.h>, used only in handlers

namespace esphome {
namespace esp32_improv {
class ESP32ImprovComponent;  // forward decl; full include in .cpp
}  // namespace esp32_improv

namespace quietcool {

class FanController;  // forward — avoids circular includes

class OemBleCompat : public Component {
 public:
  void set_fan_controller(FanController *fc) { fan_ = fc; }
  // The enable switch is pure USER INTENT ("I want Smart Control available").
  // The actual BLE service additionally yields to Improv-BLE — see want_active_().
  void set_enable_switch(switch_::Switch *s) { enable_switch_ = s; }
  // Optional Improv-BLE component. When wired, the OEM BLE service suspends
  // itself while Improv is advertising (the 31-byte advertising packet can't
  // hold both Improv's UUID and our OEM name) and resumes when Improv goes
  // idle — all without touching the user-intent enable switch.
  void set_improv(esp32_improv::ESP32ImprovComponent *c) { improv_ = c; }
  void set_pair_mode_timeout_s(uint32_t s) { pair_mode_timeout_ms_ = s * 1000; }
  void set_max_pair_ids(int n) { max_pair_ids_ = n; }
  // OTA engine (ota.http_request) the A=10 Upgrade handler drives to flash
  // custom firmware from a BLE-supplied URL. Optional — null-guarded in .cpp.
  void set_ota_component(http_request::OtaHttpRequestComponent *ota) { ota_ = ota; }

  // Sensor / entity wiring for building OEM-format responses.
  void set_temp_sensor(sensor::Sensor *s) { temp_sensor_ = s; }
  void set_humidity_sensor(sensor::Sensor *s) { humidity_sensor_ = s; }
  void set_smart_temp_high(number::Number *n) { smart_temp_high_ = n; }
  void set_smart_temp_med(number::Number *n) { smart_temp_med_ = n; }
  void set_smart_temp_low(number::Number *n) { smart_temp_low_ = n; }
  void set_smart_hum_high(number::Number *n) { smart_hum_high_ = n; }
  void set_smart_hum_low(number::Number *n) { smart_hum_low_ = n; }
  void set_smart_hum_response(select::Select *s) { smart_hum_response_ = s; }
  void set_runtime_remaining_sensor(sensor::Sensor *s) { runtime_remaining_ = s; }
  void set_default_run_number(number::Number *n) { default_run_number_ = n; }
  void set_smart_mode_status(text_sensor::TextSensor *s) { smart_mode_status_ = s; }

  // Fan info entities (editable from HA, synced with OEM BLE).
  void set_fan_name_text(text::Text *t) { fan_name_text_ = t; }
  void set_fan_model_select(select::Select *s) { fan_model_select_ = s; }
  void set_fan_serial_text(text::Text *t) { fan_serial_text_ = t; }
  void set_ble_mac_sensor(text_sensor::TextSensor *s) { ble_mac_sensor_ = s; }

  // Called from YAML on_value lambdas when fan info entities change.
  void set_fan_name(const std::string &name);
  void set_fan_model_by_display(const std::string &display_name);
  void set_fan_serial(const std::string &serial);

  // Pair mode control (HA switch, KEY2 long-press, or BLE PairMode command).
  void set_pair_mode_switch(switch_::Switch *s) { pair_mode_switch_ = s; }
  void enter_pair_mode();
  void exit_pair_mode();
  // HA-facing: wipe all stored pair-ids from NVS.
  void clear_pairings();
  // HA-facing: current number of stored pair-ids (reads NVS directly).
  int pair_count() { return nvs_pair_count_(); }

  void set_pair_count_sensor(sensor::Sensor *s) { pair_count_sensor_ = s; }
  void publish_pair_count_();

  // Preset CRUD — exposed as HA actions.
  void set_preset_select(select::Select *s) { preset_select_ = s; }
  void save_preset(const std::string &name);
  void delete_preset(const std::string &name);
  void rename_preset(const std::string &old_name, const std::string &new_name);

  // Called from YAML on_value lambdas when any threshold/timer/fan-info
  // entity changes. Marks the hx_list write-through dirty; actual NVS
  // write is debounced to once per 30s (or immediately on shutdown).
  void mark_hx_dirty() { hx_dirty_ = true; }
  // Threshold-specific variant: also deselects the active preset (since
  // live thresholds no longer match stored values). Skipped during
  // apply_preset_() to avoid self-deselection.
  void on_threshold_changed();

  // GAP event callback (registered with esp32_ble via register_gap_event_handler).
  // Re-asserts our raw OEM advertising payload whenever ESPHome rewrites the
  // structured advertisement (which re-injects the name into the scan response).
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

  void setup() override;
  void loop() override;
  void on_shutdown() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  // ── BLE service lifecycle ──
  void setup_ble_service_();
  void start_service_();
  void stop_service_();
  // Disable ESPHome's structured (name-duplicating) scan response and install a
  // static all-trimmable padding scan response, so the OEM app's substring(6,32)
  // name parse stays clean and no name-bearing scan response is ever emitted.
  // Idempotent; called on service start and re-asserted via gap_event_handler on
  // every advertising restart (covers BLE disable/enable).
  // "<model digit>ATTICFAN_<mac>" + NUL. The digit leads the manufacturer AD so
  // it lands on record byte 5, which is where the OEM app reads the fan model.
  static constexpr size_t OEM_TAGGED_NAME_BUFFER_SIZE = 23;
  void apply_oem_raw_adv_();
  // Rebuild the advertisement after the model changes (HA select, SetFanInfo).
  void refresh_adv_name_();

  // ── BLE write handler + response ──
  void on_ble_write_(const std::vector<uint8_t> &data);
  void process_message_(const std::vector<uint8_t> &msg);
  void send_response_(const std::string &json);
  void send_raw_response_(const std::vector<uint8_t> &data);

  // ── JSON command dispatch (all 22 + 2 binary) ──
  // Handlers that need request fields receive the already-parsed cJSON root
  // owned by dispatch_json_; no handler should call cJSON_Parse or cJSON_Delete.
  std::string dispatch_json_(const char *json_str);
  std::string handle_get_work_state_();
  std::string handle_get_parameter_();
  std::string handle_get_version_();
  std::string handle_get_router_();
  std::string handle_get_upgrade_state_();
  std::string handle_set_temp_humidity_(cJSON *root);
  std::string handle_set_time_(cJSON *root);
  std::string handle_get_remain_time_();
  std::string handle_set_mode_(cJSON *root);
  std::string handle_upgrade_(cJSON *root);
  std::string handle_set_router_(cJSON *root);
  std::string handle_login_(cJSON *root);
  std::string handle_pair_(cJSON *root);
  std::string handle_pair_mode_();
  std::string handle_set_fan_info_(cJSON *root);
  std::string handle_get_fan_info_();
  std::string handle_set_speed_(cJSON *root);
  std::string handle_get_presets_();
  std::string handle_set_presets_(cJSON *root);
  std::string handle_set_guide_setup_(cJSON *root);
  std::string handle_reset_();

  void handle_binary_get_record_data_(const std::vector<uint8_t> &msg);
  void handle_binary_sync_time_(const std::vector<uint8_t> &msg);

  // Deferred wait-for-Wi-Fi-then-flash for the A=10 Upgrade handler (case 3:
  // a preceding SetRouter may have just live-switched networks).
  void try_upgrade_flash_();

  // ── Pair-id access (direct to OEM hx_list NVS namespace) ──
  bool nvs_has_pair_id_(const std::string &id);
  bool nvs_add_pair_id_(const std::string &id);
  int  nvs_pair_count_();
  void nvs_clear_pairs_();

  // ── One-shot OEM NVS import (first boot only) ──
  void import_fan_info_from_nvs_();
  void import_presets_from_nvs_();

  // ── State snapshot helpers ──
  uint8_t current_speed_() const;
  uint8_t last_speed_() const;
  uint8_t current_dip_() const;

  // strncpy-and-null-terminate into a fixed-size char array. No-op on null src.
  template<size_t N>
  static void copy_bounded_(char (&dest)[N], const char *src) {
    if (!src) return;
    std::strncpy(dest, src, N - 1);
    dest[N - 1] = '\0';
  }

  // Threshold helpers — own the "OEM sentinel 255/0x7FFF → disable; valid value
  // → enable + write" decision in one place so the 5 call sites (SetTempHumidity,
  // apply_preset, snapshot, flush_hx_list, GetParameter) can't drift apart on it.
  // `which` identifies the threshold so the enable-flag can be toggled via
  // FanController::set_threshold_enabled.
  void apply_oem_temp_(number::Number *n, int oem_f, ::qc::SmartThreshold which);
  void apply_oem_hum_(number::Number *n, int oem_val, ::qc::SmartThreshold which);

  // Snapshot of the five Smart Mode threshold enabled-flags. Each call site
  // currently inlines the `!fan_ || fan_->is_threshold_enabled(...)` fallback.
  struct ThresholdEnabled { bool th, tm, tl, hh, hl; };
  ThresholdEnabled threshold_enabled_() const;

  // True when the OEM BLE service should be running right now: the user wants
  // it on (enable switch unset or ON) AND Improv-BLE isn't currently
  // advertising or has been requested and is still starting. Computed fresh
  // each loop() — no persisted "was on" flag.
  bool want_active_() const;

  // ── Members ──
  FanController *fan_ = nullptr;
  switch_::Switch *enable_switch_ = nullptr;
  esp32_improv::ESP32ImprovComponent *improv_ = nullptr;
  http_request::OtaHttpRequestComponent *ota_ = nullptr;
  sensor::Sensor *temp_sensor_ = nullptr;
  sensor::Sensor *humidity_sensor_ = nullptr;
  number::Number *smart_temp_high_ = nullptr;
  number::Number *smart_temp_med_ = nullptr;
  number::Number *smart_temp_low_ = nullptr;
  number::Number *smart_hum_high_ = nullptr;
  number::Number *smart_hum_low_ = nullptr;
  select::Select *smart_hum_response_ = nullptr;
  sensor::Sensor *runtime_remaining_ = nullptr;
  number::Number *default_run_number_ = nullptr;
  text_sensor::TextSensor *smart_mode_status_ = nullptr;
  sensor::Sensor *pair_count_sensor_ = nullptr;
  text::Text *fan_name_text_ = nullptr;
  select::Select *fan_model_select_ = nullptr;
  text::Text *fan_serial_text_ = nullptr;
  text_sensor::TextSensor *ble_mac_sensor_ = nullptr;

  // BLE
  esp32_ble_server::BLEServer *server_ = nullptr;
  esp32_ble_server::BLEService *service_ = nullptr;
  esp32_ble_server::BLECharacteristic *characteristic_ = nullptr;
  bool service_created_ = false;
  bool service_started_ = false;
  bool pending_restart_ = false;

  // Protocol state
  ::qc::PairMachine pair_machine_;
  ::qc::FrameAssembler framer_;
  bool ota_in_progress_ = false;
  uint8_t upgrade_state_ = ::qc::UPGRADE_STATE_IDLE;  // A=5 GetUpgradeState feedback
  std::string upgrade_url_;                           // captured for the deferred flash
  int upgrade_flash_retries_ = 0;
  static constexpr int UPGRADE_FLASH_MAX_RETRIES = 40;  // ~20s @ 500ms wait-for-Wi-Fi
  bool syncing_fan_info_ = false;  // suppress on_value feedback during BLE→HA push
  // Config entities are set up at HARDWARE priority (800), long before this
  // component's AFTER_WIFI (200) setup(). A template select with restore_value
  // publishes its stored-or-initial option from its own setup(), and that
  // fires on_value. Without this flag an entity default ("Generic" -> model
  // "0") lands in fan_info_ before setup() has loaded it, and also overwrites
  // the fan name. That write does not reach flash, because fan_info_pref_ has
  // no backend until setup() assigns it and save() returns false on a null
  // backend. setup() then overwrites the struct anyway, so the residue today
  // is only a stray hx_dirty_. All of that correctness rests on setup order,
  // so ignore entity-originated writes until setup() establishes fan_info_.
  bool fan_info_loaded_ = false;

  switch_::Switch *pair_mode_switch_ = nullptr;
  int max_pair_ids_ = 50;
  uint32_t pair_mode_timeout_ms_ = 120000;

  // Debounced write-through: current ESPHome entity state → OEM hx_list NVS.
  // Covers thresholds, timer defaults, fan info, presets, guide_setup.
  bool hx_dirty_ = false;
  uint32_t hx_dirty_since_ms_ = 0;
  static constexpr uint32_t HX_FLUSH_DELAY_MS = 30000;
  void flush_hx_list_();

  // Fan info (user-settable name/model/serial, persisted)
  struct FanInfo {
    char name[33] = "Generic";
    char model[33] = "0";
    char serial[33] = "";
    char guide_setup[4] = "No";
  } __attribute__((packed));
  FanInfo fan_info_;
  ESPPreferenceObject fan_info_pref_;

  // Presets storage (up to 4 presets × 7 fields)
  struct Preset {
    char name[51] = "";
    int16_t values[6] = {};  // [speed_enum, temp_h, temp_m, temp_l, hum_h, hum_l]
  } __attribute__((packed));
  struct PresetStorage {
    uint8_t count = 0;
    Preset presets[4] = {};
  } __attribute__((packed));
  PresetStorage presets_;
  ESPPreferenceObject preset_pref_;

  // Preset select (active-preset picker exposed to HA).
  select::Select *preset_select_ = nullptr;
  uint8_t active_preset_idx_ = 0xFF;  // 0xFF = no active preset
  ESPPreferenceObject active_preset_pref_;
  bool syncing_preset_ = false;
  bool boot_apply_pending_ = false;

  void rebuild_preset_options_();
  void apply_preset_(uint8_t idx);
  void snapshot_thresholds_into_preset_(uint8_t idx);
};

}  // namespace quietcool
}  // namespace esphome
