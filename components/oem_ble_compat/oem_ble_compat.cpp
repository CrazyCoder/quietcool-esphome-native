#include "oem_ble_compat.h"
#include "../fan_controller/fan_controller.h"
#include "../oem_nvs_reader/oem_nvs_reader_logic.h"  // shared OemThresholdSchema (hx_list keys/sentinels)
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble_server/ble_2902.h"
#include "esphome/components/esp32_improv/esp32_improv_component.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cJSON.h>
#include <esp_bt_device.h>
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#include <esp_coexist.h>  // Wi-Fi/BLE shared-radio coexistence preference
#endif

#include <nvs.h>
#include <cstdio>
#include <cstring>
#include <span>

namespace esphome {
namespace quietcool {

static const char *const TAG = "oem_ble_compat";

// ── Reach ESP32BLE's private BLEAdvertising to disable the structured scan
// response ──────────────────────────────────────────────────────────────────
// ESPHome's services_advertisement_() UNCONDITIONALLY duplicates the device
// name into the scan response (scan_response_data_.include_name = true,
// hardcoded) and re-runs on every advertising restart. That duplicate is the
// source of the OEM app's "<name> ATTI" leak. There is no public setter to turn
// it off, and the BLEAdvertising object is a private member of ESP32BLE. The
// explicit-instantiation idiom below is the standard, legal C++ way to obtain a
// pointer to that private member. If a future ESPHome renames advertising_, this
// fails to COMPILE rather than silently regressing at runtime.
namespace {
struct BLEAdvTag {
  using MemberPtr = esp32_ble::BLEAdvertising *esp32_ble::ESP32BLE::*;
  friend MemberPtr ble_adv_member(BLEAdvTag);
};
template<BLEAdvTag::MemberPtr M> struct BLEAdvThief {
  friend BLEAdvTag::MemberPtr ble_adv_member(BLEAdvTag) { return M; }
};
template struct BLEAdvThief<&esp32_ble::ESP32BLE::advertising_>;

esp32_ble::BLEAdvertising *oem_ble_advertising() {
  if (esp32_ble::global_ble == nullptr)
    return nullptr;
  return esp32_ble::global_ble->*ble_adv_member(BLEAdvTag{});
}
}  // namespace

// OEM GATT UUIDs — must match stock firmware for the Smart Control app to find us.
static const uint8_t SERVICE_UUID_RAW[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00};
static const uint8_t CHAR_UUID_RAW[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00};

// NVS preference hashes (stable, never reuse).
static const uint32_t FANINFO_PREF_HASH = 0x71636202;
static const uint32_t PRESET_PREF_HASH = 0x71636203;
static const uint32_t ACTIVE_PRESET_PREF_HASH = 0x71636204;

// Per-DIP-wiring NVS key mapping for OEM preset storage.
struct NvsPresetKeys {
  const char *value_prefix;   // "Med", "Low", "High"
  char name_char;             // 'm', 'l', 'h'
  const char *count_key;      // "medsize1", "lowsize1", "highsize1"
  const char *tag_key;        // "PresetsMed", "PresetsLow", "PresetsHigh"

  // Per-slot keys: name "testname<c><slot+1>", value "<prefix><(slot+1)*10+j>".
  // OEM firmware schema; centralised so write+read can't drift apart.
  void format_name_key(int slot, char *out, size_t n) const {
    snprintf(out, n, "testname%c%d", name_char, slot + 1);
  }
  void format_value_key(int slot, int j, char *out, size_t n) const {
    snprintf(out, n, "%s%d", value_prefix, (slot + 1) * 10 + j);
  }
};

static const NvsPresetKeys *nvs_preset_keys_for_dip(uint8_t dip) {
  static const NvsPresetKeys med  = {"Med",  'm', "medsize1",  "PresetsMed"};
  static const NvsPresetKeys low  = {"Low",  'l', "lowsize1",  "PresetsLow"};
  static const NvsPresetKeys high = {"High", 'h', "highsize1", "PresetsHigh"};
  switch (dip) {
    case 1: return &med;    // TwoSpeed
    case 2: return &low;    // ThreeSpeed
    case 3: return &high;   // OneSpeed
    default: return nullptr; // None / invalid
  }
}

// ── Lifecycle ───────────────────────────────────────────────────────

void OemBleCompat::setup() {
  ESP_LOGD(TAG, "setup(): global_ble_server=%p", (void*) esp32_ble_server::global_ble_server);
  fan_info_pref_ = global_preferences->make_preference<FanInfo>(FANINFO_PREF_HASH, true);
  if (!fan_info_pref_.load(&fan_info_)) {
    fan_info_ = FanInfo{};
    import_fan_info_from_nvs_();
  }
  // fan_info_ is authoritative from here on, so entity-originated writes are
  // real user intent rather than a config entity publishing its own default.
  fan_info_loaded_ = true;

  // Presets: hx_list NVS is the source of truth (written by flush_hx_list_
  // on every 30s debounce + clean shutdown). ESPHome preference is only a
  // fallback for first boot before any hx_list data exists.
  preset_pref_ = global_preferences->make_preference<PresetStorage>(PRESET_PREF_HASH, true);
  presets_ = PresetStorage{};
  import_presets_from_nvs_();
  if (presets_.count == 0 && !preset_pref_.load(&presets_))
    presets_ = PresetStorage{};

  server_ = esp32_ble_server::global_ble_server;
  publish_pair_count_();

  // Publish initial fan info to HA entities (guard suppresses on_value feedback).
  syncing_fan_info_ = true;
  if (fan_name_text_) fan_name_text_->publish_state(fan_info_.name);
  if (fan_model_select_) fan_model_select_->publish_state(::qc::fan_model_display(fan_info_.model));
  if (fan_serial_text_) fan_serial_text_->publish_state(fan_info_.serial);
  syncing_fan_info_ = false;

  // Preset select: load active index, wire callback, publish initial options.
  active_preset_pref_ = global_preferences->make_preference<uint8_t>(ACTIVE_PRESET_PREF_HASH, true);
  if (!active_preset_pref_.load(&active_preset_idx_))
    active_preset_idx_ = (presets_.count > 0) ? 0 : 0xFF;
  if (preset_select_) {
    rebuild_preset_options_();
    preset_select_->add_on_state_callback([this](size_t index) {
      if (syncing_preset_) return;
      auto idx = static_cast<uint8_t>(index);
      if (idx < presets_.count)
        apply_preset_(idx);
    });
    // Deferred apply: override entity restore_value results with the active
    // preset's stored values. Must run after all component setups complete.
    boot_apply_pending_ = true;
    this->set_timeout("boot_apply", 500, [this]() {
      if (active_preset_idx_ < presets_.count)
        apply_preset_(active_preset_idx_);
      boot_apply_pending_ = false;
    });
  }
}

bool OemBleCompat::want_active_() const {
  // User intent: switch unset (always-on build) or switch ON.
  const bool user_wants = (enable_switch_ == nullptr || enable_switch_->state);
  // Yield the advertising packet to Improv-BLE while it's advertising — both
  // can't fit in 31 bytes. Resumes automatically when Improv goes idle.
  // should_start() closes the asynchronous startup gap: WiFiComponent requests
  // Improv before its service reaches an active state. Without this check OEM
  // BLE can start during that gap and leave ATTICFAN data in the scan response.
  const bool improv_busy =
      (improv_ != nullptr && (improv_->is_active() || improv_->should_start()));
  return user_wants && !improv_busy;
}

void OemBleCompat::loop() {
  bool want_active = want_active_();

  if (want_active && !service_created_) {
    if (server_ == nullptr) server_ = esp32_ble_server::global_ble_server;
    if (server_ != nullptr && server_->is_running()) {
      setup_ble_service_();
    }
  }

  if (service_created_) {
    if (want_active) {
      if (!service_started_) start_service_();
    } else if (service_started_) {
      stop_service_();
      pending_restart_ = true;
    }
  }
  // After stop(), the GATTS callback asynchronously transitions the service
  // to STOPPED state. Once there, we can safely call start() again.
  if (pending_restart_ && want_active && service_ &&
      !service_->is_running() && !service_->is_starting() && !service_->is_created()) {
    ESP_LOGI(TAG, "Restarting OEM BLE service after toggle");
    service_->start();
    pending_restart_ = false;
  }

  // Pair-mode timeout check — auto-off the switch when it expires.
  bool was_pair_mode = (pair_machine_.state == ::qc::PairState::PairMode);
  pair_machine_.check_timeout(millis());
  if (was_pair_mode && pair_machine_.state != ::qc::PairState::PairMode) {
    if (pair_mode_switch_) pair_mode_switch_->publish_state(false);
    ESP_LOGI(TAG, "Pair mode timed out");
  }

  // Debounced hx_list write-through.
  if (hx_dirty_) {
    if (hx_dirty_since_ms_ == 0) {
      hx_dirty_since_ms_ = millis() | 1;  // avoid 0 sentinel at boot/wrap
    } else if (millis() - hx_dirty_since_ms_ >= HX_FLUSH_DELAY_MS) {
      flush_hx_list_();
    }
  }
}

void OemBleCompat::dump_config() {
  ESP_LOGCONFIG(TAG, "OEM BLE Compat:");
  ESP_LOGCONFIG(TAG, "  Max pair IDs: %d (stored: %d)", max_pair_ids_, nvs_pair_count_());
  ESP_LOGCONFIG(TAG, "  Pair-mode timeout: %u s", pair_mode_timeout_ms_ / 1000);
  ESP_LOGCONFIG(TAG, "  Fan controller: %s", fan_ ? "OK" : "MISSING");
}

void OemBleCompat::on_shutdown() {
  if (hx_dirty_) flush_hx_list_();
}

// Flush current ESPHome entity state into OEM hx_list NVS keys so a
// stock-firmware restore picks up the user's latest settings.
void OemBleCompat::flush_hx_list_() {
  hx_dirty_ = false;
  hx_dirty_since_ms_ = 0;

  nvs_handle_t h;
  if (nvs_open("hx_list", NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGW(TAG, "hx_list flush: nvs_open failed");
    return;
  }

  // Smart Mode thresholds (°F int16, matching OEM schema). Keys + sentinels
  // come from the shared OemThresholdSchema so this write side can't drift from
  // the reader. Disabled thresholds write the OEM sentinel.
  using Schema = ::qc::OemThresholdSchema;
  auto en = threshold_enabled_();
  auto write_temp = [&](const char *key, number::Number *n, bool enabled) {
    if (!enabled) { nvs_set_i16(h, key, Schema::TEMP_SENTINEL); return; }
    if (!n || std::isnan(n->state)) return;
    nvs_set_i16(h, key, static_cast<int16_t>(::qc::threshold_c_to_f(n->state)));
  };
  write_temp(Schema::KEY_TEMP_HIGH, smart_temp_high_, en.th);
  write_temp(Schema::KEY_TEMP_MED, smart_temp_med_, en.tm);
  write_temp(Schema::KEY_TEMP_LOW, smart_temp_low_, en.tl);

  auto write_hum = [&](const char *key, number::Number *n, bool enabled) {
    if (!enabled) { nvs_set_u8(h, key, Schema::HUM_SENTINEL); return; }
    if (!n || std::isnan(n->state)) return;
    nvs_set_u8(h, key, static_cast<uint8_t>(n->state));
  };
  write_hum(Schema::KEY_HUM_HIGH, smart_hum_high_, en.hh);
  write_hum(Schema::KEY_HUM_LOW, smart_hum_low_, en.hl);

  if (smart_hum_response_ && smart_hum_response_->has_state()) {
    uint8_t rank = ::qc::oem_speed_to_internal(
        ::qc::hum_response_to_oem(smart_hum_response_->current_option().c_str()));
    nvs_set_u8(h, Schema::KEY_HUM_RANK, rank);
  }

  // Timer defaults.
  if (default_run_number_ && !std::isnan(default_run_number_->state)) {
    int total = static_cast<int>(default_run_number_->state);
    nvs_set_u8(h, "hour_set", static_cast<uint8_t>(total / 60));
    nvs_set_u8(h, "minute_set", static_cast<uint8_t>(total % 60));
  }

  // Fan info.
  nvs_set_str(h, "nnn", fan_info_.name);
  nvs_set_str(h, "mmm", fan_info_.model);
  nvs_set_str(h, "lll", fan_info_.serial);
  nvs_set_str(h, "GuideSetup", fan_info_.guide_setup);

  // Presets — write for current DIP wiring.
  const NvsPresetKeys *pkeys = nvs_preset_keys_for_dip(current_dip_());
  if (pkeys) {
    nvs_set_u8(h, pkeys->count_key, presets_.count);
    nvs_set_u8(h, pkeys->tag_key, 0x66);

    for (int slot = 0; slot < presets_.count && slot < 4; slot++) {
      auto &p = presets_.presets[slot];

      // Write primary name only. OEM firmware triplicates each name
      // (testnamem1 / testnamem11 / testnamem111) but only the primary copy is
      // read by GetPresets and our import_presets_from_nvs_(). Skipping the
      // duplicates saves ~16 NVS entry slots on the tight 16 KB partition.
      char key[20];
      pkeys->format_name_key(slot, key, sizeof(key));
      nvs_set_str(h, key, p.name);

      for (int j = 0; j < 6; j++) {
        char val_key[12];
        pkeys->format_value_key(slot, j, val_key, sizeof(val_key));
        nvs_set_i16(h, val_key, p.values[j]);
      }
    }
  }

  // OEM section tags.
  // "flag" (i8 0x61='a') = Smart Mode/timer section — NOT "flag_PhoneID" (pair sentinel).
  // "hubID" (u8 0x66='f') = fan-info section.
  nvs_set_i8(h, "flag", 0x61);
  nvs_set_u8(h, "hubID", 0x66);

  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "hx_list NVS synced (thresholds + fan info + presets + timer)");
}

// ── BLE service lifecycle ───────────────────────────────────────────

void OemBleCompat::setup_ble_service_() {
  if (service_created_) return;

  // advertise=false: don't add the 128-bit UUID to the advertising packet.
  // The OEM app discovers by name pattern (ATTICFAN_*), not UUID. Including
  // the UUID would overflow the 31-byte advertising limit (18B UUID + 23B
  // name + 3B flags = 44B). The UUID is still discoverable via GATT after
  // connection.
  service_ = server_->create_service(
      esp32_ble::ESPBTUUID::from_raw(SERVICE_UUID_RAW), false, 8);
  if (service_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create BLE service");
    return;
  }

  characteristic_ = service_->create_characteristic(
      esp32_ble::ESPBTUUID::from_raw(CHAR_UUID_RAW),
      esp32_ble_server::BLECharacteristic::PROPERTY_WRITE |
      esp32_ble_server::BLECharacteristic::PROPERTY_NOTIFY |
      esp32_ble_server::BLECharacteristic::PROPERTY_WRITE_NR);

  // Pre-size the characteristic value buffer so the ESP-IDF GATT layer
  // doesn't truncate incoming writes. OEM protocol's largest message is
  // ~150 bytes (GetParameter response / Upgrade URL).
  std::vector<uint8_t> init_val(256, 0);
  characteristic_->set_value(std::move(init_val));

  characteristic_->add_descriptor(new esp32_ble_server::BLE2902());
  characteristic_->on_write([this](std::span<const uint8_t> data, uint16_t conn_id) {
    (void) conn_id;
    std::vector<uint8_t> vec(data.begin(), data.end());
    this->on_ble_write_(vec);
  });

  service_created_ = true;
  ESP_LOGI(TAG, "OEM BLE service created (UUID 000000ff-...)");
}

void OemBleCompat::start_service_() {
  if (!service_) return;
  // Follow the esp32_improv pattern: call start() when is_created(),
  // then detect is_running() on a subsequent loop iteration.
  if (service_->is_created()) {
    service_->start();
  }
  if (service_->is_running() && !service_started_) {
    service_started_ = true;
#ifdef USE_WIFI
    // Bias the shared 2.4 GHz radio toward BLE while OEM BLE is live. The ESP32
    // time-shares one radio between Wi-Fi and BLE; under the default balanced
    // arbitration Wi-Fi can starve the BLE connection-setup window, so a
    // central's connect fails at the first GATT request with HCI 0x3E. ESPHome's
    // esp32_ble_tracker does this via software_coexistence for outbound
    // connections; we mirror it for our inbound (peripheral) connections, scoped
    // to when OEM BLE is actually enabled (reverted in stop_service_()).
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
#endif
    // Re-set the BLE name + restart advertising now that the service is live.
    const uint8_t *mac = esp_bt_dev_get_address();
    if (mac) {
      char mac_lower[MAC_ADDRESS_BUFFER_SIZE];
      format_mac_addr_lower_no_sep(mac, mac_lower);
      char name[22];
      snprintf(name, sizeof(name), "ATTICFAN_%s", mac_lower);
      // Set the GAP name (keeps GATT 0x2A00 + active scanners clean). Disable
      // ESPHome's structured scan response and install our padding scan response
      // BEFORE advertising starts, so the very first packet is already clean (no
      // boot-time window). Then let ESPHome start advertising so its lifecycle
      // (params, Improv handoff) stays intact — with scan_response_=false it only
      // configures the ADV packet (name once), never a name-bearing scan rsp.
      esp_ble_gap_set_device_name(name);
      // ESPHome configures its structured advertisement inside this call, so our
      // raw payload has to be written after it, not before, or it is discarded.
      // gap_event_handler re-asserts on every later advertising restart.
      esp32_ble::global_ble->advertising_set_service_data_and_name({}, true);
      apply_oem_raw_adv_();

      ESP_LOGI(TAG, "OEM BLE service started, name: %s (no-scanrsp-name)", name);
      if (ble_mac_sensor_) {
        char mac_str[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
        format_mac_addr_upper(mac, mac_str);
        ble_mac_sensor_->publish_state(mac_str);
      }
    } else {
      ESP_LOGI(TAG, "OEM BLE service started");
    }
  }
}

// The name and model change at runtime from the HA entities and from OEM
// SetFanInfo, and both are carried in the advertisement, so rebuild the packet
// instead of waiting for a reboot.
void OemBleCompat::refresh_adv_name_() {
  if (!service_started_)
    return;
  apply_oem_raw_adv_();
  ESP_LOGI(TAG, "OEM BLE advertisement rebuilt, model '%c', name '%s'", fan_info_.model[0],
           fan_info_.name);
}

void OemBleCompat::apply_oem_raw_adv_() {
  // The OEM app does NOT use the parsed BLE name. It byte-slices the raw scan
  // record (ExtendedDeviceAdapter.flashHolderView):
  //   name = recordStr.substring(6, 32); model = recordStr.substring(5, 6);
  //   if (model.equals("A")) name = "A" + name;  txt.setText(name.trim());
  //
  // ESPHome's own scan response duplicates the device name, which puts a second
  // copy inside substring(6,32) and makes the app render "ATTICFAN_<mac> ATTI".
  // It also reconfigures and restarts advertising on every connection teardown,
  // so overwriting the scan response afterwards leaves a window where the leaky
  // packet goes out. Disabling ESPHome's structured scan response entirely
  // (set_scan_response(false)) removes the code path rather than racing it:
  // services_advertisement_() then only configures the ADV packet, and both raw
  // payloads below are ours alone.
  esp32_ble::BLEAdvertising *adv = oem_ble_advertising();
  if (adv != nullptr)
    adv->set_scan_response(false);

  // Two things have to be true at once, and they cannot come from one AD
  // structure:
  //
  //   1. ScannerRepository.isSmartControlDevice keeps a scan result only when
  //      getDeviceName().startsWith("ATTICFAN"), so the advertised name must
  //      still begin with ATTICFAN. Putting the model digit in the name hides
  //      the hub from the app completely (tried in 833aac6, reverted).
  //   2. ExtendedDeviceAdapter.flashHolderView reads model = record[5:6] and
  //      name = record[6:32] out of the raw record, and getDeviceImgAttic maps
  //      only "1".."7" to a fan photo.
  //
  // So the digit comes from a manufacturer AD placed right after flags, whose
  // payload is <digit><fan name>: byte 5 is the digit and bytes 6..30 are the
  // label the adapter shows in the device list. The real name AD moves to the
  // scan response, which still feeds getDeviceName(). This is what the OEM
  // firmware builds too — it concatenates its model and name globals into a
  // 26-byte buffer and logs the result as "test_manufacturer", and its
  // esp_ble_adv_data_t carries that buffer with manufacturer_len 26.
  //
  // The payload is a FIXED 26 bytes, NUL padded, exactly as stock sizes it.
  // Sizing it to the name instead would leave the tail of substring(6,32)
  // reading into whatever follows — the raw scan-response name AD — and trim()
  // only strips the ends, so a short name would render as
  // "<name>...ATTICFAN_<mac>". The padding is safe because it sits inside a
  // length-prefixed AD: parsing skips the whole structure. A 0x00 where a
  // LENGTH byte is expected is the thing to avoid, since ScanRecord
  // .parseFromBytes stops there and the scan response is parsed from the same
  // concatenated buffer, which would erase the device name and trip
  // condition 1.
  const uint8_t *mac = esp_bt_dev_get_address();
  if (mac == nullptr)
    return;
  char mac_lower[MAC_ADDRESS_BUFFER_SIZE];
  format_mac_addr_lower_no_sep(mac, mac_lower);

  char ble_name[OEM_BLE_NAME_BUFFER_SIZE];  // "ATTICFAN_<mac>"
  const int name_len = snprintf(ble_name, sizeof(ble_name), "ATTICFAN_%s", mac_lower);
  if (name_len < 1 || name_len >= (int) sizeof(ble_name))
    return;

  const char m = fan_info_.model[0];
  const char digit = (m >= '0' && m <= '7') ? m : '0';
  // Fan info with no name would leave the device-list row blank, so fall back
  // to the BLE name.
  const char *label = fan_info_.name[0] != '\0' ? fan_info_.name : ble_name;
  const size_t label_room = OEM_ADV_MFG_PAYLOAD_LEN - 1;
  size_t label_len = strlen(label);
  if (label_len > label_room)
    label_len = label_room;

  uint8_t advraw[3 + 2 + OEM_ADV_MFG_PAYLOAD_LEN];
  size_t n = 0;
  advraw[n++] = 0x02;
  advraw[n++] = ESP_BLE_AD_TYPE_FLAG;
  advraw[n++] = 0x06;
  advraw[n++] = (uint8_t) (1 + OEM_ADV_MFG_PAYLOAD_LEN);
  advraw[n++] = ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE;
  advraw[n++] = (uint8_t) digit;
  memcpy(advraw + n, label, label_len);
  memset(advraw + n + label_len, 0, label_room - label_len);
  n += label_room;
  esp_ble_gap_config_adv_data_raw(advraw, n);

  // Name only, no trailing pad, so AD parsing walks cleanly off the end.
  uint8_t scanrsp[2 + OEM_BLE_NAME_BUFFER_SIZE];
  size_t s = 0;
  scanrsp[s++] = (uint8_t) (1 + name_len);  // type byte + the name
  scanrsp[s++] = ESP_BLE_AD_TYPE_NAME_CMPL;
  memcpy(scanrsp + s, ble_name, name_len);
  s += name_len;
  esp_ble_gap_config_scan_rsp_data_raw(scanrsp, s);
}

void OemBleCompat::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  // Re-assert our raw payloads on every advertising (re)start: ESPHome rebuilds
  // its structured advertisement on connection teardown, and a full BLE
  // disable/enable (Improv handoff) clears the controller's data outright.
  // Guarded by service_started_ so we never touch Improv's own advertisement
  // (Improv advertises while OEM BLE is stopped).
  //
  // This still leaves a brief window right after a teardown where ESPHome's own
  // packet is on air: BLEAdvertising::start() calls esp_ble_gap_config_adv_data
  // and esp_ble_gap_start_advertising back to back, so advertising begins before
  // any GAP event we can hook. In that window byte 5 is the 'A' of ATTICFAN, so
  // a scan landing there reads the generic photo until the next report. Closing
  // it needs ESPHome's structured path gone, not raced: bluedroid emits AD
  // structures in a fixed order (flags, appearance, name, manufacturer, TX
  // power), so a structured packet can never carry the model byte at offset 5.
  if (event == ESP_GAP_BLE_ADV_START_COMPLETE_EVT && service_started_) {
    apply_oem_raw_adv_();
  }
}

void OemBleCompat::stop_service_() {
  if (service_ && service_started_) {
    service_->stop();
    service_started_ = false;
    // Restore ESPHome's structured scan response so any next BLE consumer
    // (Improv on AP-fallback, esp32_ble_tracker, etc.) gets the scan-response
    // slot back. apply_oem_raw_adv_() turned it off to kill the OEM-app name
    // leak while OEM BLE was running; without this restore, an auto-started
    // Improv tries to cram its service UUID + the "ATTICFAN_<mac>" GAP name
    // into the 31-byte ADV alone and overflows (BTM "data exceed max adv
    // packet length"), making Improv undiscoverable at improv-wifi.com.
    // Next OEM BLE start will turn scan_response off again via
    // apply_oem_raw_adv_().
    esp32_ble::BLEAdvertising *adv = oem_ble_advertising();
    if (adv != nullptr)
      adv->set_scan_response(true);
    // The GAP name is global, not service-scoped. Do not let Improv inherit
    // ATTICFAN_<mac>; a short setup name keeps Improv's structured scan
    // response within the 31-byte budget and prevents the OEM app from
    // mistaking an onboarding advertisement for a controllable fan.
    esp_ble_gap_set_device_name("QuietCool Setup");
#ifdef USE_WIFI
    // OEM BLE no longer in use — stop biasing the radio against Wi-Fi.
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
#endif
    ESP_LOGI(TAG, "OEM BLE service stopped");
  }
}

// ── BLE write handler ───────────────────────────────────────────────

void OemBleCompat::on_ble_write_(const std::vector<uint8_t> &data) {
  ESP_LOGD(TAG, "BLE write received: %u bytes", (unsigned) data.size());
  if (!framer_.feed(data.data(), data.size())) {
    ESP_LOGW(TAG, "BLE frame exceeded %u bytes without completing — buffer dropped",
             (unsigned) ::qc::FrameAssembler::MAX_BUFFERED);
    return;
  }
  while (framer_.has_complete()) {
    auto msg = framer_.take();
    if (!msg.empty()) process_message_(msg);
  }
}

void OemBleCompat::process_message_(const std::vector<uint8_t> &msg) {
  ESP_LOGD(TAG, "Complete message: %u bytes: %.*s", (unsigned) msg.size(),
           (int) std::min(msg.size(), size_t(80)), reinterpret_cast<const char *>(msg.data()));
  if (msg.size() < 2) return;

  // Binary commands: check byte[1] for 0x1C / 0x1D.
  if (msg[0] == '{' && (msg[1] == 0x1C || msg[1] == 0x1D)) {
    if (pair_machine_.state != ::qc::PairState::Auth) return;
    if (msg[1] == 0x1C) handle_binary_get_record_data_(msg);
    else                 handle_binary_sync_time_(msg);
    return;
  }

  // JSON path.
  std::string json_str(msg.begin(), msg.end());
  std::string response = dispatch_json_(json_str.c_str());
  if (!response.empty()) send_response_(response);
}

void OemBleCompat::send_response_(const std::string &json) {
  ESP_LOGD(TAG, "BLE response: %s", json.c_str());
  auto wrapped = ::qc::qq_wrap(json);
  send_raw_response_(wrapped);
}

void OemBleCompat::send_raw_response_(const std::vector<uint8_t> &data) {
  if (!characteristic_) return;
  // Chunk at MTU-3 (default 20 bytes).
  auto chunks = ::qc::chunk_response(data, 23);
  for (auto &chunk : chunks) {
    characteristic_->set_value(std::move(chunk));
    characteristic_->notify();
  }
}

// ── JSON command dispatch ───────────────────────────────────────────

// ── Helper: extract cJSON string field (V1 or V2 key) ───────────────

static std::string json_str_field(cJSON *root, const char *v2_key, const char *v1_key) {
  cJSON *item = cJSON_GetObjectItem(root, v2_key);
  if (!item || !cJSON_IsString(item))
    item = cJSON_GetObjectItem(root, v1_key);
  return (item && cJSON_IsString(item)) ? std::string(item->valuestring) : std::string();
}

static int json_int_field(cJSON *root, const char *v2_key, const char *v1_key, int def = -1) {
  cJSON *item = cJSON_GetObjectItem(root, v2_key);
  if (!item || !cJSON_IsNumber(item))
    item = cJSON_GetObjectItem(root, v1_key);
  return (item && cJSON_IsNumber(item)) ? item->valueint : def;
}

std::string OemBleCompat::dispatch_json_(const char *json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root) return "";

  int a_code = -1;
  cJSON *a_item = cJSON_GetObjectItem(root, "A");
  if (a_item && cJSON_IsNumber(a_item)) {
    a_code = a_item->valueint;
  } else {
    // V1 fallback: map the "Api" verb to its V2 numeric code.
    cJSON *api = cJSON_GetObjectItem(root, "Api");
    if (api && cJSON_IsString(api)) {
      static constexpr struct { const char *verb; int code; } V1_VERBS[] = {
          {"GetWorkState", 1},    {"GetParameter", 2},  {"GetVersion", 3},
          {"GetRouter", 4},       {"GetUpgradeState", 5}, {"SetTempHumidity", 6},
          {"SetTime", 7},         {"GetRemainTime", 8},  {"SetMode", 9},
          {"Upgrade", 10},        {"SetRouter", 11},     {"Login", 13},
          {"Login2", 13},         {"Pair", 14},          {"PairMode", 15},
          {"SetFanInfo", 16},     {"GetFanInfo", 17},    {"SetSpeed", 18},
          {"GetPresets", 19},     {"SetPresets", 20},    {"SetGuideSetup", 21},
          {"Reset", 22},
      };
      for (const auto &v : V1_VERBS) {
        if (strcmp(api->valuestring, v.verb) == 0) { a_code = v.code; break; }
      }
    }
  }

  if (a_code < 0) { cJSON_Delete(root); return ""; }

  // Gate check. A=13 Login and A=14 Pair are reachable before auth (Pair's
  // handler self-enforces PairMode); A=15 PairMode is NOT exempt — it requires
  // an authenticated session, matching stock (an unpaired device can only enter
  // pair mode via the physical KEY2 long-hold).
  auto gate = ::qc::check_gate(a_code, pair_machine_.state, ota_in_progress_);
  if ((gate == ::qc::GateResult::NeedAuth && a_code != 13 && a_code != 14) ||
      gate == ::qc::GateResult::OtaBlocked) {
    cJSON_Delete(root);
    return "";
  }

  ESP_LOGD(TAG, "BLE cmd A=%d (pair_state=%d)", a_code, (int) pair_machine_.state);

  std::string result;
  switch (a_code) {
    case 1:  result = handle_get_work_state_(); break;
    case 2:  result = handle_get_parameter_(); break;
    case 3:  result = handle_get_version_(); break;
    case 4:  result = handle_get_router_(); break;
    case 5:  result = handle_get_upgrade_state_(); break;
    case 6:  result = handle_set_temp_humidity_(root); break;
    case 7:  result = handle_set_time_(root); break;
    case 8:  result = handle_get_remain_time_(); break;
    case 9:  result = handle_set_mode_(root); break;
    case 10: result = handle_upgrade_(root); break;
    case 11: result = handle_set_router_(root); break;
    case 13: result = handle_login_(root); break;
    case 14: result = handle_pair_(root); break;
    case 15: result = handle_pair_mode_(); break;
    case 16: result = handle_set_fan_info_(root); break;
    case 17: result = handle_get_fan_info_(); break;
    case 18: result = handle_set_speed_(root); break;
    case 19: result = handle_get_presets_(); break;
    case 20: result = handle_set_presets_(root); break;
    case 21: result = handle_set_guide_setup_(root); break;
    case 22: result = handle_reset_(); break;
  }
  cJSON_Delete(root);
  return result;
}

// ── Threshold helpers ───────────────────────────────────────────────

// OEM sentinel handling: 255 (wire) or 0x7FFF (stored) → disable the threshold;
// any valid value enables it and writes °C (after F→C). Caller is responsible
// for the "field absent / out-of-range" guard in handle_set_temp_humidity_.
void OemBleCompat::apply_oem_temp_(number::Number *n, int oem_f,
                                   ::qc::SmartThreshold which) {
  if (!n) return;
  if (oem_f == 255 || oem_f == 0x7FFF) {
    if (fan_) fan_->set_threshold_enabled(which, false);
    return;
  }
  if (fan_) fan_->set_threshold_enabled(which, true);
  auto call = n->make_call();
  call.set_value(::qc::threshold_f_to_c(oem_f));
  call.perform();
}

void OemBleCompat::apply_oem_hum_(number::Number *n, int oem_val,
                                  ::qc::SmartThreshold which) {
  if (oem_val == 255) {
    if (fan_) fan_->set_threshold_enabled(which, false);
    return;
  }
  if (fan_) fan_->set_threshold_enabled(which, true);
  if (!n) return;
  auto call = n->make_call();
  call.set_value(static_cast<float>(oem_val));
  call.perform();
}

OemBleCompat::ThresholdEnabled OemBleCompat::threshold_enabled_() const {
  using ::qc::SmartThreshold;
  auto on = [this](SmartThreshold t) { return !fan_ || fan_->is_threshold_enabled(t); };
  return {on(SmartThreshold::TempHigh), on(SmartThreshold::TempMed),
          on(SmartThreshold::TempLow), on(SmartThreshold::HumHigh),
          on(SmartThreshold::HumLow)};
}

// ── State snapshot helpers ──────────────────────────────────────────

uint8_t OemBleCompat::current_speed_() const {
  return fan_ ? fan_->current_speed_enum() : 0;
}

uint8_t OemBleCompat::last_speed_() const {
  return fan_ ? fan_->last_speed_enum() : 3;
}

uint8_t OemBleCompat::current_dip_() const {
  return fan_ ? fan_->current_dip_enum() : 0;
}

// ── Command handlers ────────────────────────────────────────────────

// A=13 Login
std::string OemBleCompat::handle_login_(cJSON *root) {
  auto phone_id = json_str_field(root, "P", "PhoneID");
  if (phone_id.empty() || !::qc::validate_phone_id(phone_id))
    return R"({"A":13,"R":"Fail"})";

  bool found = nvs_has_pair_id_(phone_id);
  if (found) {
    pair_machine_.state = ::qc::PairState::Auth;
    pair_machine_.current_pair_id = phone_id;
  }
  bool in_pair_mode = (pair_machine_.state == ::qc::PairState::PairMode);

  char buf[80];
  snprintf(buf, sizeof(buf), R"({"A":13,"R":"%s","P":"%s"})",
           found ? "Success" : "Fail",
           in_pair_mode ? "Yes" : "No");
  return buf;
}

// A=14 Pair
std::string OemBleCompat::handle_pair_(cJSON *root) {
  auto phone_id = json_str_field(root, "P", "PhoneID");
  if (phone_id.empty() || !::qc::validate_phone_id(phone_id))
    return R"({"A":14,"R":"Fail"})";
  if (pair_machine_.state != ::qc::PairState::PairMode)
    return R"({"A":14,"R":"Fail"})";

  int count = nvs_pair_count_();
  if (count >= max_pair_ids_) return R"({"A":14,"R":"Beyond"})";

  if (!nvs_add_pair_id_(phone_id)) return R"({"A":14,"R":"Fail"})";

  pair_machine_.state = ::qc::PairState::Auth;
  pair_machine_.current_pair_id = phone_id;
  publish_pair_count_();
  if (pair_mode_switch_ && pair_mode_switch_->state)
    pair_mode_switch_->publish_state(false);
  ESP_LOGI(TAG, "New pair-id registered (%d/%d)", nvs_pair_count_(), max_pair_ids_);
  return R"({"A":14,"R":"Success"})";
}

// A=15 PairMode
std::string OemBleCompat::handle_pair_mode_() {
  enter_pair_mode();
  return R"({"A":15,"R":"Success"})";
}

void OemBleCompat::enter_pair_mode() {
  pair_machine_.enter_pair_mode(millis(), pair_mode_timeout_ms_);
  if (pair_mode_switch_ && !pair_mode_switch_->state)
    pair_mode_switch_->publish_state(true);
  ESP_LOGI(TAG, "Pair mode entered (timeout %u s)", pair_mode_timeout_ms_ / 1000);
}

void OemBleCompat::exit_pair_mode() {
  if (pair_machine_.state == ::qc::PairState::PairMode) {
    pair_machine_.state = pair_machine_.current_pair_id.empty()
                              ? ::qc::PairState::Init
                              : ::qc::PairState::Auth;
  }
  if (pair_mode_switch_ && pair_mode_switch_->state)
    pair_mode_switch_->publish_state(false);
  ESP_LOGI(TAG, "Pair mode exited");
}

void OemBleCompat::clear_pairings() {
  nvs_clear_pairs_();
  pair_machine_.state = ::qc::PairState::Init;
  pair_machine_.current_pair_id.clear();
  publish_pair_count_();
  ESP_LOGW(TAG, "All BLE pair-ids cleared");
}

void OemBleCompat::set_fan_name(const std::string &name) {
  if (!fan_info_loaded_ || syncing_fan_info_) return;
  copy_bounded_(fan_info_.name, name.c_str());
  fan_info_pref_.save(&fan_info_);
  mark_hx_dirty();
  // The advertisement carries the name the app shows in its device list.
  refresh_adv_name_();
}

// Setting the model from a display name mirrors the name (OEM "model name ==
// fan name" behavior): the OEM picker shows the model display string in the
// fan-name field, so we surface it the same way.
void OemBleCompat::set_fan_model_by_display(const std::string &display_name) {
  if (!fan_info_loaded_ || syncing_fan_info_) return;
  copy_bounded_(fan_info_.model, ::qc::fan_model_index(display_name.c_str()));
  copy_bounded_(fan_info_.name, display_name.c_str());
  if (fan_name_text_) fan_name_text_->publish_state(fan_info_.name);
  fan_info_pref_.save(&fan_info_);
  mark_hx_dirty();
  // The advertisement carries the model digit the app reads for the fan photo,
  // and this also renames the fan.
  refresh_adv_name_();
}

void OemBleCompat::set_fan_serial(const std::string &serial) {
  if (!fan_info_loaded_ || syncing_fan_info_) return;
  copy_bounded_(fan_info_.serial, serial.c_str());
  fan_info_pref_.save(&fan_info_);
  mark_hx_dirty();
}

void OemBleCompat::publish_pair_count_() {
  if (pair_count_sensor_)
    pair_count_sensor_->publish_state(static_cast<float>(nvs_pair_count_()));
}

// A=1 GetWorkState
std::string OemBleCompat::handle_get_work_state_() {
  bool fan_on = fan_ && fan_->fan_is_on();
  bool smart = fan_ && fan_->is_smart_mode_active();
  bool timer = fan_ && fan_->timer_is_running();
  const char *mode = ::qc::mode_to_oem(fan_on, smart, timer);

  const char *speed_str = ::qc::speed_to_oem(current_speed_());
  bool sensor_ok = temp_sensor_ && !std::isnan(temp_sensor_->state);
  int temp_fx10 = sensor_ok ? ::qc::temp_c_to_f_x10(temp_sensor_->state) : 0;
  int hum = (humidity_sensor_ && !std::isnan(humidity_sensor_->state))
                ? static_cast<int>(humidity_sensor_->state) : 0;

  char buf[128];
  snprintf(buf, sizeof(buf),
           R"({"A":1,"M":"%s","R":"%s","S":"%s","T":%d,"H":%d,"C":%d})",
           mode, speed_str, sensor_ok ? "OK" : "NG",
           temp_fx10, hum,
           pair_machine_.state == ::qc::PairState::Auth ? 1 : 0);
  return buf;
}

// A=2 GetParameter
std::string OemBleCompat::handle_get_parameter_() {
  bool fan_on = fan_ && fan_->fan_is_on();
  bool smart = fan_ && fan_->is_smart_mode_active();
  bool timer = fan_ && fan_->timer_is_running();
  const char *mode = ::qc::mode_to_oem(fan_on, smart, timer);
  const char *fan_type = ::qc::dip_to_oem(current_dip_());

  auto read_temp = [](number::Number *p, bool enabled) -> int {
    if (!enabled) return 255;
    return (p && !std::isnan(p->state)) ? ::qc::threshold_c_to_f(p->state) : 0;
  };

  auto en = threshold_enabled_();
  int temp_h = read_temp(smart_temp_high_, en.th);
  int temp_m = read_temp(smart_temp_med_,  en.tm);
  int temp_l = read_temp(smart_temp_low_,  en.tl);
  int hum_h = !en.hh ? 255 :
              (smart_hum_high_ && !std::isnan(smart_hum_high_->state) ?
              static_cast<int>(smart_hum_high_->state) : 90);
  int hum_l = !en.hl ? 255 :
              (smart_hum_low_ && !std::isnan(smart_hum_low_->state) ?
              static_cast<int>(smart_hum_low_->state) : 70);

  const char *hum_range = "LOW";
  if (smart_hum_response_ && smart_hum_response_->has_state())
    hum_range = ::qc::hum_response_to_oem(smart_hum_response_->current_option().c_str());

  // Timer defaults.
  int timer_hours = 3, timer_minutes = 0;
  if (default_run_number_ && !std::isnan(default_run_number_->state)) {
    int total = static_cast<int>(default_run_number_->state);
    timer_hours = total / 60;
    timer_minutes = total % 60;
  }

  // L = GetTime_Range: the speed that timer mode runs at. Use last_speed_
  // so the app shows the correct speed even when the fan is currently off.
  const char *time_range = ::qc::speed_to_oem(last_speed_());

  // V2 keys: B=Mode, C=FanType, D=TempH, E=TempM, F=TempL, G=HumH,
  // H=HumL, I=HumRange, J=Hour, K=Minute, L=TimeRange, M=GuideSetup
  // (sequential B-M per OEM app's receiveGetParameter)
  char buf[256];
  snprintf(buf, sizeof(buf),
           R"({"A":2,"B":"%s","C":"%s","D":%d,"E":%d,"F":%d,"G":%d,"H":%d,"I":"%s","J":%d,"K":%d,"L":"%s","M":"%s"})",
           mode, fan_type, temp_h, temp_m, temp_l, hum_h, hum_l, hum_range,
           timer_hours, timer_minutes, time_range,
           ::qc::json_escape(fan_info_.guide_setup).c_str());
  return buf;
}

// A=3 GetVersion
// Report the production channel's exact OEM firmware version so the Smart Control
// app's broken inequality check (device-version != cloud-version => "update
// available") sees us as current and stops nagging. Our firmware is
// feature-equivalent regardless of this compatibility string.
std::string OemBleCompat::handle_get_version_() {
  return ::qc::get_version_response();
}

// A=4 GetRouter
std::string OemBleCompat::handle_get_router_() {
  const char *ssid = "";
#ifdef USE_WIFI
  static char ssid_buf[wifi::SSID_BUFFER_SIZE];
  if (wifi::global_wifi_component != nullptr)
    ssid = wifi::global_wifi_component->wifi_ssid_to(ssid_buf);
#endif
  const uint8_t *mac = esp_bt_dev_get_address();
  char mac_str[MAC_ADDRESS_PRETTY_BUFFER_SIZE] = "";
  if (mac)
    format_mac_addr_upper(mac, mac_str);

  // An SSID is arbitrary user text and can carry quotes or braces.
  return R"({"A":4,"S":")" + ::qc::json_escape(ssid) +
         R"(","P":"","M":")" + std::string(mac_str) + R"("})";
}

// A=5 GetUpgradeState — minimal flash feedback (see handle_upgrade_).
std::string OemBleCompat::handle_get_upgrade_state_() {
  char buf[64];
  snprintf(buf, sizeof(buf), R"({"A":5,"S":"%s","P":"0"})",
           ::qc::upgrade_state_string(upgrade_state_));
  return buf;
}

// A=6 SetTempHumidity
std::string OemBleCompat::handle_set_temp_humidity_(cJSON *root) {
  // V2 request keys per OEM app's dealSetTempHumidity: B-G (shifted from response D-I)
  int temp_h = json_int_field(root, "B", "SetTemp_H");
  int temp_m = json_int_field(root, "C", "SetTemp_M");
  int temp_l = json_int_field(root, "D", "SetTemp_L");
  int hum_h  = json_int_field(root, "E", "SetHum_H");
  int hum_l  = json_int_field(root, "F", "SetHum_L");
  auto hum_range = json_str_field(root, "G", "SetHum_Range");

  bool ok = true;
  auto set_temp = [&](number::Number *n, int oem_f, ::qc::SmartThreshold which) {
    if (oem_f < 0) return;
    if (!::qc::validate_threshold(oem_f)) { ok = false; return; }
    apply_oem_temp_(n, oem_f, which);
  };
  auto set_hum = [&](number::Number *n, int oem_val, ::qc::SmartThreshold which) {
    if (oem_val < 0) return;
    if (!::qc::validate_threshold(oem_val)) { ok = false; return; }
    apply_oem_hum_(n, oem_val, which);
  };

  set_temp(smart_temp_high_, temp_h, ::qc::SmartThreshold::TempHigh);
  set_temp(smart_temp_med_,  temp_m, ::qc::SmartThreshold::TempMed);
  set_temp(smart_temp_low_,  temp_l, ::qc::SmartThreshold::TempLow);
  set_hum(smart_hum_high_, hum_h, ::qc::SmartThreshold::HumHigh);
  set_hum(smart_hum_low_,  hum_l, ::qc::SmartThreshold::HumLow);
  if (!hum_range.empty() && smart_hum_response_) {
    auto call = smart_hum_response_->make_call();
    call.set_option(::qc::oem_range_to_select(hum_range.c_str()));
    call.perform();
  }

  mark_hx_dirty();
  char buf[32];
  snprintf(buf, sizeof(buf), R"({"A":6,"F":"%s"})", ok ? "TRUE" : "FALSE");
  return buf;
}

// A=7 SetTime — arms the timer with the app's chosen duration + speed.
// The OEM app flow is: SetMode("Timer") → SetTime(H,M,R) → SetMode("Timer").
// SetTime is the actual "configure and arm" step.
std::string OemBleCompat::handle_set_time_(cJSON *root) {
  int hours = json_int_field(root, "H", "Hour");
  int minutes = json_int_field(root, "M", "Minute");
  auto speed_str = json_str_field(root, "R", "SetTime_Range");

  if (hours >= 0 && minutes >= 0 && fan_) {
    // Widen before multiplying and clamp to a day: the fields come straight off
    // the wire, and hours * 60 overflows a 32-bit int for large values.
    int64_t wide = static_cast<int64_t>(hours) * 60 + minutes;
    int total_min = static_cast<int>(std::min<int64_t>(wide, 1440));
    std::string spd = speed_str.empty() ? "low" : speed_str;
    // Arm the timer at the chosen speed + duration.
    fan_->set_runtime(spd, total_min);
    // Also update the default run number for future turn-ons.
    if (default_run_number_) {
      auto call = default_run_number_->make_call();
      call.set_value(static_cast<float>(total_min));
      call.perform();
    }
  }
  mark_hx_dirty();
  return R"({"A":7,"F":"TRUE"})";
}

// A=8 GetRemainTime
std::string OemBleCompat::handle_get_remain_time_() {
  int remain_min = 0;
  if (runtime_remaining_ && !std::isnan(runtime_remaining_->state))
    remain_min = static_cast<int>(runtime_remaining_->state);
  int h = remain_min / 60;
  int m = remain_min % 60;
  char buf[48];
  snprintf(buf, sizeof(buf), R"({"A":8,"H":%d,"M":%d,"S":0})", h, m);
  return buf;
}

// A=9 SetMode
std::string OemBleCompat::handle_set_mode_(cJSON *root) {
  auto mode = json_str_field(root, "M", "Mode");
  if (mode.empty()) return R"({"A":9,"F":"FALSE"})";

  auto flags = ::qc::parse_oem_mode(mode.c_str());
  if (fan_) {
    if (flags.smart_mode) {
      fan_->activate_smart_mode();
    } else {
      if (fan_->is_smart_mode_active()) fan_->deactivate_smart_mode();
      if (flags.turn_on) {
        auto call = fan_->make_call();
        call.set_state(true);
        call.perform();
        if (!flags.timer_mode) {
          fan_->run_indefinitely();
        }
      } else {
        auto call = fan_->make_call();
        call.set_state(false);
        call.perform();
      }
    }
  }
  // App's receiveSetMode expects W (WorkMode) + F (Flag).
  bool fan_on = fan_ && fan_->fan_is_on();
  bool smart = fan_ && fan_->is_smart_mode_active();
  bool timer = fan_ && fan_->timer_is_running();
  const char *cur_mode = ::qc::mode_to_oem(fan_on, smart, timer);
  char buf[64];
  snprintf(buf, sizeof(buf), R"({"A":9,"W":"%s","F":"TRUE"})", cur_mode);
  return buf;
}

// A=10 Upgrade — real OTA flash from a client-supplied URL.
//
// Auth-gated (check_gate requires PairState::Auth; first pairing is physical
// KEY2). OEM-domain URLs are a deliberate no-op (ack TRUE) so the OEM Smart Control
// app can't flash stock firmware over this build. Any other valid http(s) URL
// triggers a real OTA via the shared ota.http_request engine, then reboots.
// NB: this does NOT wipe NVS (a custom-firmware update preserves config — the
// dual-button stock-restore is the only wipe path), and SetRouter's live Wi-Fi
// switch is unchanged.
std::string OemBleCompat::handle_upgrade_(cJSON *root) {
  auto url = json_str_field(root, "U", "URL");
  switch (::qc::classify_upgrade_url(url)) {
    case ::qc::UpgradeDecision::Reject:
      ESP_LOGW(TAG, "Upgrade URL rejected (empty/too long/bad scheme): %s", url.c_str());
      return R"({"A":10,"F":"FALSE"})";
    case ::qc::UpgradeDecision::BlockedOemDomain:
      ESP_LOGW(TAG, "Upgrade URL is an OEM domain — ignoring (no-op): %s", url.c_str());
      return R"({"A":10,"F":"TRUE"})";
    case ::qc::UpgradeDecision::Flash:
      break;
  }
  if (ota_ == nullptr) {
    ESP_LOGE(TAG, "Upgrade requested but ota.http_request not wired (ota_id missing)");
    return R"({"A":10,"F":"FALSE"})";
  }
  ESP_LOGW(TAG, "BLE Upgrade accepted — flashing from %s (fan will reboot)", url.c_str());
  upgrade_state_ = ::qc::UPGRADE_STATE_DOWNLOADING;
  upgrade_url_ = url;
  upgrade_flash_retries_ = 0;
  // Defer ~1s so the BLE F:TRUE notification flushes (and a client has a poll
  // window for A=5), then wait for Wi-Fi before downloading — a preceding
  // SetRouter may have just live-switched networks (case 3), so the new AP
  // might still be reconnecting.
  this->set_timeout("ble_upgrade_flash", 1000, [this]() { this->try_upgrade_flash_(); });
  return R"({"A":10,"F":"TRUE"})";
}

// Wait-for-Wi-Fi-then-flash for the deferred BLE Upgrade. A preceding SetRouter
// may have switched networks, so poll until the (possibly new) Wi-Fi connects
// before handing the URL to ota.http_request; give up after ~20s -> Download_Fail.
void OemBleCompat::try_upgrade_flash_() {
  bool wifi_up = false;
#ifdef USE_WIFI
  wifi_up = wifi::global_wifi_component != nullptr &&
            wifi::global_wifi_component->is_connected();
#endif
  if (wifi_up) {
    ota_->set_url(upgrade_url_);
    // ota.http_request mandates a checksum (there is no skip). Keep the BLE
    // Upgrade command OEM-identical (URL only) and source the MD5 firmware-side:
    // fetch it from the companion "<url>.md5" file the firmware host serves.
    ota_->set_md5_url(upgrade_url_ + ".md5");
    ota_->flash();  // downloads + reboots on success; returns here ONLY on failure
    upgrade_state_ = ::qc::UPGRADE_STATE_FAIL;
    ESP_LOGE(TAG, "BLE Upgrade flash failed (see http_request log)");
    return;
  }
  if (++upgrade_flash_retries_ > UPGRADE_FLASH_MAX_RETRIES) {
    upgrade_state_ = ::qc::UPGRADE_STATE_FAIL;
    ESP_LOGE(TAG, "BLE Upgrade: Wi-Fi not connected in time — aborting flash");
    return;
  }
  ESP_LOGD(TAG, "BLE Upgrade: waiting for Wi-Fi (attempt %d)", upgrade_flash_retries_);
  this->set_timeout("ble_upgrade_flash", 500, [this]() { this->try_upgrade_flash_(); });
}

// A=11 SetRouter
std::string OemBleCompat::handle_set_router_(cJSON *root) {
  auto ssid = json_str_field(root, "S", "Ssid");
  auto pwd  = json_str_field(root, "P", "Password");

  if (ssid.empty() || !::qc::validate_ssid(ssid))
    return R"({"A":11,"F":"FALSE"})";
  if (!::qc::validate_password(pwd))
    return R"({"A":11,"F":"FALSE"})";

#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    bool connected = wifi::global_wifi_component->is_connected();
    char ssid_buf[wifi::SSID_BUFFER_SIZE];
    const char *current = wifi::global_wifi_component->wifi_ssid_to(ssid_buf);
    if (::qc::setrouter_should_switch(connected, current, ssid)) {
      // Live Wi-Fi switch — no reboot. save_wifi_sta persists + reconfigures the
      // STA; disable()+enable() forces a reconnect to the new AP (connect_soon_
      // no-ops while still connected to the old one). BLE stays up (only the
      // Wi-Fi radio cycles — the path Improv/captive_portal use). A following
      // Upgrade waits for the new Wi-Fi before downloading (see handle_upgrade_).
      ESP_LOGI(TAG, "SetRouter: switching Wi-Fi to '%s' (live, no reboot)", ssid.c_str());
      wifi::global_wifi_component->save_wifi_sta(ssid.c_str(), pwd.c_str());
      wifi::global_wifi_component->disable();
      wifi::global_wifi_component->enable();
    } else {
      // Already on this SSID — the OEM app is just resending the current network
      // (its update flow). Nothing to do.
      ESP_LOGI(TAG, "SetRouter: already connected to '%s' — no change", ssid.c_str());
    }
  }
#endif
  return R"({"A":11,"F":"TRUE"})";
}

// A=16 SetFanInfo
std::string OemBleCompat::handle_set_fan_info_(cJSON *root) {
  // Copy a string field by its V2/V1 keys when present (preserves the OEM
  // semantic that a field set to "" explicitly clears the local value).
  auto copy_if_present = [&](const char *v2, const char *v1, auto &dest) {
    cJSON *item = cJSON_GetObjectItem(root, v2);
    if (!item || !cJSON_IsString(item))
      item = cJSON_GetObjectItem(root, v1);
    if (item && cJSON_IsString(item))
      copy_bounded_(dest, item->valuestring);
  };
  copy_if_present("N", "Name",      fan_info_.name);
  copy_if_present("M", "Model",     fan_info_.model);
  copy_if_present("S", "SerialNum", fan_info_.serial);

  fan_info_pref_.save(&fan_info_);
  mark_hx_dirty();

  // Push updated state to HA entities (guard suppresses on_value feedback).
  syncing_fan_info_ = true;
  if (fan_name_text_) fan_name_text_->publish_state(fan_info_.name);
  if (fan_model_select_) fan_model_select_->publish_state(::qc::fan_model_display(fan_info_.model));
  if (fan_serial_text_) fan_serial_text_->publish_state(fan_info_.serial);
  syncing_fan_info_ = false;

  // The advertisement carries the model digit the app reads for the fan photo.
  refresh_adv_name_();

  return R"({"A":16,"F":"TRUE"})";
}

// A=17 GetFanInfo — app reads N/M/S only (GuideSetup comes from GetParameter M key).
// Built with std::string, not a fixed buffer: escaping can double a field's
// length, and a truncated response would be unframeable by the app.
std::string OemBleCompat::handle_get_fan_info_() {
  return R"({"A":17,"N":")" + ::qc::json_escape(fan_info_.name) +
         R"(","M":")" + ::qc::json_escape(fan_info_.model) +
         R"(","S":")" + ::qc::json_escape(fan_info_.serial) + R"("})";
}

// A=18 SetSpeed
std::string OemBleCompat::handle_set_speed_(cJSON *root) {
  auto speed_str = json_str_field(root, "S", "Speed");
  if (speed_str.empty()) return R"({"A":18,"F":"FALSE"})";

  uint8_t spd = ::qc::oem_speed_to_internal(speed_str.c_str());
  if (fan_) {
    if (spd == 0) {
      auto call = fan_->make_call();
      call.set_state(false);
      call.perform();
    } else {
      auto call = fan_->make_call();
      call.set_state(true);
      call.set_speed(spd);
      call.perform();
    }
  }
  // App's receiveSetSpeed expects S (acknowledged speed) + F (flag). Echo the
  // canonical speed we actually applied rather than the client's raw string:
  // for every legitimate input the two are identical, and echoing raw text
  // would let an overlong or quote-bearing value truncate this response into
  // something the app can neither frame nor parse.
  char buf[48];
  snprintf(buf, sizeof(buf), R"({"A":18,"S":"%s","F":"TRUE"})",
           ::qc::speed_to_oem(spd));
  return buf;
}

// A=19 GetPresets
std::string OemBleCompat::handle_get_presets_() {
  // App crashes on empty preset array — always return at least the OEM default.
  if (presets_.count == 0) {
    return R"({"A":19,"P":[["Summer",120,100,80,90,70,"LOW"]]})";
  }
  std::string buf = R"({"A":19,"P":[)";
  for (int i = 0; i < presets_.count && i < 4; i++) {
    if (i > 0) buf += ',';
    auto &p = presets_.presets[i];
    // Only the numeric part goes through a fixed buffer; the escaped name is
    // appended directly, since escaping can outgrow any size chosen here.
    char nums[64];
    snprintf(nums, sizeof(nums), R"(",%d,%d,%d,%d,%d,"%s"])",
             p.values[1], p.values[2], p.values[3], p.values[4], p.values[5],
             ::qc::speed_to_oem(p.values[0]));
    buf += R"([")";
    buf += ::qc::json_escape(p.name);
    buf += nums;
  }
  buf += "]}";
  return buf;
}

// A=20 SetPresets
std::string OemBleCompat::handle_set_presets_(cJSON *root) {
  cJSON *arr = cJSON_GetObjectItem(root, "P");
  if (!arr) arr = cJSON_GetObjectItem(root, "Presets");
  if (!arr || !cJSON_IsArray(arr)) return R"({"A":20,"F":"FALSE"})";

  int arr_count = cJSON_GetArraySize(arr);
  if (arr_count > 4) arr_count = 4;
  uint8_t valid = 0;

  for (int i = 0; i < arr_count; i++) {
    cJSON *preset = cJSON_GetArrayItem(arr, i);
    if (!preset || !cJSON_IsArray(preset)) continue;
    auto &p = presets_.presets[valid];
    memset(&p, 0, sizeof(p));

    cJSON *name_item = cJSON_GetArrayItem(preset, 0);
    if (name_item && cJSON_IsString(name_item))
      copy_bounded_(p.name, name_item->valuestring);
    for (int j = 1; j <= 5; j++) {
      cJSON *val = cJSON_GetArrayItem(preset, j);
      if (val && cJSON_IsNumber(val)) p.values[j] = static_cast<int16_t>(val->valueint);
    }
    cJSON *speed_item = cJSON_GetArrayItem(preset, 6);
    if (speed_item && cJSON_IsString(speed_item))
      p.values[0] = ::qc::oem_speed_to_internal(speed_item->valuestring);
    valid++;
  }
  presets_.count = valid;

  preset_pref_.save(&presets_);
  active_preset_idx_ = 0;
  active_preset_pref_.save(&active_preset_idx_);
  rebuild_preset_options_();
  mark_hx_dirty();

  return R"({"A":20,"F":"TRUE"})";
}

// A=21 SetGuideSetup
std::string OemBleCompat::handle_set_guide_setup_(cJSON *root) {
  auto val = json_str_field(root, "G", "GuideSetup");
  if (!val.empty()) {
    copy_bounded_(fan_info_.guide_setup, val.c_str());
    fan_info_pref_.save(&fan_info_);
    mark_hx_dirty();
  }
  return R"({"A":21,"F":"TRUE"})";
}

// A=22 Reset
std::string OemBleCompat::handle_reset_() {
  ESP_LOGW(TAG, "BLE Reset command received — wiping NVS + reboot");
  // Defer the actual reset so the response flushes first.
  this->set_timeout("ble_reset", 500, []() {
    global_preferences->reset();
    App.safe_reboot();
  });
  return R"({"A":22,"F":"TRUE"})";
}

// ── Binary commands ─────────────────────────────────────────────────

// A=28 GetRecordData — stub (ESPHome users get history via HA recorder).
// The app's dealHubData groups values by 3 spaces; total must be exactly
// 3K (multiple of 3) or the trailing group has fewer than 3 values and
// crashes with IndexOutOfBoundsException. The OEM firmware sends 75 bytes:
// year(1) + month(1) + day(1) + 24h × 3 metrics(72) = 75 = 3×25. ✓
void OemBleCompat::handle_binary_get_record_data_(const std::vector<uint8_t> &msg) {
  uint8_t day_idx = (msg.size() >= 3) ? msg[2] : 0;
  ESP_LOGD(TAG, "GetRecordData day=%d (stub — zeroed hourly data)", day_idx);
  // 75 data bytes: year + month + day + 72 (24h × temp+hum+speed), all the
  // 0xFF "no data" sentinel.
  std::vector<uint8_t> payload(75, 0xFF);
  send_raw_response_(::qc::binary_frame(0x1C, payload));
}

// A=29 SynchronizeTime — stub (ESPHome uses SNTP).
void OemBleCompat::handle_binary_sync_time_(const std::vector<uint8_t> &msg) {
  ESP_LOGD(TAG, "SynchronizeTime (stub, using SNTP)");
  (void) msg;
  send_raw_response_(::qc::binary_frame(0x1D, {0x01}));  // 0x01 = success
}

// ── One-shot OEM NVS import (first boot only — pref load failure is the marker) ──

void OemBleCompat::import_fan_info_from_nvs_() {
  nvs_handle_t h;
  if (nvs_open("hx_list", NVS_READONLY, &h) != ESP_OK) return;

  uint8_t tag = 0;
  if (nvs_get_u8(h, "hubID", &tag) != ESP_OK || tag != 0x66) {
    nvs_close(h);
    ESP_LOGD(TAG, "No OEM fan info in NVS (hubID tag missing)");
    return;
  }

  auto read_str = [&](const char *key, char *dest, size_t max) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 1 && len <= max) {
      nvs_get_str(h, key, dest, &len);
    }
  };

  read_str("nnn", fan_info_.name, sizeof(fan_info_.name));
  read_str("mmm", fan_info_.model, sizeof(fan_info_.model));
  read_str("lll", fan_info_.serial, sizeof(fan_info_.serial));
  read_str("GuideSetup", fan_info_.guide_setup, sizeof(fan_info_.guide_setup));

  nvs_close(h);
  fan_info_pref_.save(&fan_info_);
  ESP_LOGI(TAG, "Imported fan info from OEM NVS: name='%s' model='%s'",
           fan_info_.name, fan_info_.model);
}

void OemBleCompat::import_presets_from_nvs_() {
  uint8_t dip = current_dip_();
  const NvsPresetKeys *keys = nvs_preset_keys_for_dip(dip);
  if (!keys) {
    ESP_LOGD(TAG, "No preset import: DIP=%d (invalid/none)", dip);
    return;
  }

  nvs_handle_t h;
  if (nvs_open("hx_list", NVS_READONLY, &h) != ESP_OK) return;

  uint8_t tag = 0;
  if (nvs_get_u8(h, keys->tag_key, &tag) != ESP_OK || tag != 0x66) {
    nvs_close(h);
    ESP_LOGD(TAG, "No OEM presets in NVS (%s tag missing)", keys->tag_key);
    return;
  }

  uint8_t count = 0;
  nvs_get_u8(h, keys->count_key, &count);
  if (count == 0 || count > 4) {
    nvs_close(h);
    return;
  }

  presets_ = PresetStorage{};
  for (int slot = 0; slot < count; slot++) {
    auto &p = presets_.presets[slot];

    char name_key[16];
    keys->format_name_key(slot, name_key, sizeof(name_key));
    size_t len = 0;
    if (nvs_get_str(h, name_key, nullptr, &len) == ESP_OK && len > 1) {
      if (len > sizeof(p.name)) len = sizeof(p.name);
      nvs_get_str(h, name_key, p.name, &len);
    }

    for (int j = 0; j < 6; j++) {
      char val_key[12];
      keys->format_value_key(slot, j, val_key, sizeof(val_key));
      int16_t val = 0;
      nvs_get_i16(h, val_key, &val);
      p.values[j] = val;
    }
  }
  presets_.count = count;

  nvs_close(h);
  preset_pref_.save(&presets_);
  ESP_LOGI(TAG, "Imported %d preset(s) from OEM NVS (prefix=%s)", count, keys->value_prefix);
}

// ── Preset select CRUD ─────────────────────────────────────────────

void OemBleCompat::rebuild_preset_options_() {
  if (!preset_select_) return;
  int named = 0;
  for (int i = 0; i < presets_.count && i < 4; i++) {
    if (presets_.presets[i].name[0] != '\0') named++;
  }
  if (named == 0) return;
  FixedVector<const char *> opts;
  opts.init(static_cast<size_t>(named));
  for (int i = 0; i < presets_.count && i < 4; i++) {
    if (presets_.presets[i].name[0] != '\0')
      opts.push_back(presets_.presets[i].name);
  }
  syncing_preset_ = true;
  preset_select_->traits.set_options(opts);
  if (active_preset_idx_ >= presets_.count)
    active_preset_idx_ = 0;
  preset_select_->publish_state(presets_.presets[active_preset_idx_].name);
  syncing_preset_ = false;
}

void OemBleCompat::apply_preset_(uint8_t idx) {
  if (idx >= presets_.count) return;
  syncing_preset_ = true;
  auto &p = presets_.presets[idx];

  apply_oem_temp_(smart_temp_high_, p.values[1], ::qc::SmartThreshold::TempHigh);
  apply_oem_temp_(smart_temp_med_,  p.values[2], ::qc::SmartThreshold::TempMed);
  apply_oem_temp_(smart_temp_low_,  p.values[3], ::qc::SmartThreshold::TempLow);
  apply_oem_hum_(smart_hum_high_, p.values[4], ::qc::SmartThreshold::HumHigh);
  apply_oem_hum_(smart_hum_low_,  p.values[5], ::qc::SmartThreshold::HumLow);

  if (smart_hum_response_) {
    auto call = smart_hum_response_->make_call();
    call.set_option(::qc::oem_range_to_select(::qc::speed_to_oem(p.values[0])));
    call.perform();
  }

  active_preset_idx_ = idx;
  active_preset_pref_.save(&active_preset_idx_);
  mark_hx_dirty();

  if (preset_select_) preset_select_->publish_state(p.name);
  syncing_preset_ = false;
  ESP_LOGI(TAG, "Applied preset '%s' (slot %d)", p.name, idx);
}

void OemBleCompat::snapshot_thresholds_into_preset_(uint8_t idx) {
  if (idx >= 4) return;
  auto &p = presets_.presets[idx];

  p.values[0] = 0;
  if (smart_hum_response_ && smart_hum_response_->has_state())
    p.values[0] = ::qc::oem_speed_to_internal(
        ::qc::hum_response_to_oem(smart_hum_response_->current_option().c_str()));

  auto snap_temp = [](number::Number *n, bool enabled) -> int16_t {
    if (!enabled || !n || std::isnan(n->state)) return 255;
    return static_cast<int16_t>(::qc::threshold_c_to_f(n->state));
  };
  auto snap_hum = [](number::Number *n, bool enabled) -> int16_t {
    if (!enabled || !n || std::isnan(n->state)) return 255;
    return static_cast<int16_t>(n->state);
  };

  auto en = threshold_enabled_();
  p.values[1] = snap_temp(smart_temp_high_, en.th);
  p.values[2] = snap_temp(smart_temp_med_,  en.tm);
  p.values[3] = snap_temp(smart_temp_low_,  en.tl);
  p.values[4] = snap_hum(smart_hum_high_,   en.hh);
  p.values[5] = snap_hum(smart_hum_low_,    en.hl);
}

void OemBleCompat::save_preset(const std::string &name) {
  if (name.empty() || name.size() > 50) {
    ESP_LOGW(TAG, "save_preset: name empty or too long");
    return;
  }
  for (int i = 0; i < presets_.count; i++) {
    if (strcmp(presets_.presets[i].name, name.c_str()) == 0) {
      snapshot_thresholds_into_preset_(i);
      active_preset_idx_ = i;
      active_preset_pref_.save(&active_preset_idx_);
      preset_pref_.save(&presets_);
      mark_hx_dirty();
      rebuild_preset_options_();
      ESP_LOGI(TAG, "Updated preset '%s' (slot %d)", name.c_str(), i);
      return;
    }
  }
  if (presets_.count >= 4) {
    ESP_LOGW(TAG, "save_preset: all 4 slots full");
    return;
  }
  uint8_t slot = presets_.count;
  copy_bounded_(presets_.presets[slot].name, name.c_str());
  snapshot_thresholds_into_preset_(slot);
  presets_.count++;
  active_preset_idx_ = slot;
  active_preset_pref_.save(&active_preset_idx_);
  preset_pref_.save(&presets_);
  mark_hx_dirty();
  rebuild_preset_options_();
  ESP_LOGI(TAG, "Created preset '%s' (slot %d, count=%d)", name.c_str(), slot, presets_.count);
}

void OemBleCompat::delete_preset(const std::string &name) {
  if (presets_.count <= 1) {
    ESP_LOGW(TAG, "delete_preset: refusing — at least 1 preset must remain");
    return;
  }
  int found = -1;
  for (int i = 0; i < presets_.count; i++) {
    if (strcmp(presets_.presets[i].name, name.c_str()) == 0) {
      found = i;
      break;
    }
  }
  if (found < 0) {
    ESP_LOGW(TAG, "delete_preset: '%s' not found", name.c_str());
    return;
  }
  for (int i = found; i < presets_.count - 1; i++)
    presets_.presets[i] = presets_.presets[i + 1];
  presets_.count--;
  memset(&presets_.presets[presets_.count], 0, sizeof(Preset));

  if (active_preset_idx_ == static_cast<uint8_t>(found))
    active_preset_idx_ = 0;
  else if (active_preset_idx_ != 0xFF && active_preset_idx_ > static_cast<uint8_t>(found))
    active_preset_idx_--;
  active_preset_pref_.save(&active_preset_idx_);
  preset_pref_.save(&presets_);
  mark_hx_dirty();
  rebuild_preset_options_();
  ESP_LOGI(TAG, "Deleted preset '%s' (was slot %d, count=%d)", name.c_str(), found, presets_.count);
}

void OemBleCompat::rename_preset(const std::string &old_name, const std::string &new_name) {
  if (new_name.empty() || new_name.size() > 50) {
    ESP_LOGW(TAG, "rename_preset: new name empty or too long");
    return;
  }
  for (int i = 0; i < presets_.count; i++) {
    if (strcmp(presets_.presets[i].name, old_name.c_str()) == 0) {
      copy_bounded_(presets_.presets[i].name, new_name.c_str());
      preset_pref_.save(&presets_);
      mark_hx_dirty();
      rebuild_preset_options_();
      ESP_LOGI(TAG, "Renamed preset '%s' → '%s' (slot %d)", old_name.c_str(), new_name.c_str(), i);
      return;
    }
  }
  ESP_LOGW(TAG, "rename_preset: '%s' not found", old_name.c_str());
}

void OemBleCompat::on_threshold_changed() {
  if (!syncing_preset_ && !boot_apply_pending_ && active_preset_idx_ < presets_.count) {
    snapshot_thresholds_into_preset_(active_preset_idx_);
  }
  mark_hx_dirty();
}

// ── Pair-id NVS access (OEM hx_list namespace — single source of truth) ──

// OEM sentinel byte stored at "flag_PhoneID" when the pair store is active.
// Anything else is treated as an empty/uninitialised store.
static constexpr uint8_t PAIR_FLAG_SENTINEL = 0x41;

// Hard ceiling on Phone<N> slots in the OEM hx_list namespace. The scan/append/
// clear helpers below all bound themselves by this, so the configured
// max_pair_ids can never exceed it (the YAML schema caps it to the same value).
static constexpr int MAX_PAIR_SLOTS = 50;

// Opens hx_list RO iff the OEM pair-flag sentinel is present, and reads
// pair_num. Returns true with the handle live; false (handle closed, *num=0)
// when the namespace is missing or the sentinel byte doesn't match.
static bool open_pair_store_ro_(nvs_handle_t *h, uint8_t *num_out) {
  *num_out = 0;
  if (nvs_open("hx_list", NVS_READONLY, h) != ESP_OK) return false;
  uint8_t flag = 0;
  nvs_get_u8(*h, "flag_PhoneID", &flag);
  if (flag != PAIR_FLAG_SENTINEL) { nvs_close(*h); return false; }
  nvs_get_u8(*h, "pair_num", num_out);
  return true;
}

// Reads pair_num from hx_list. Returns 0 if namespace missing or flag invalid.
int OemBleCompat::nvs_pair_count_() {
  nvs_handle_t h;
  uint8_t num;
  if (!open_pair_store_ro_(&h, &num)) return 0;
  nvs_close(h);
  return num;
}

// Linear scan Phone1..Phone<pair_num> — same algorithm as OEM pair_id_lookup_in_nvs.
bool OemBleCompat::nvs_has_pair_id_(const std::string &id) {
  nvs_handle_t h;
  uint8_t num;
  if (!open_pair_store_ro_(&h, &num)) return false;

  // Pair-ids are capped at 100 chars by validate_phone_id; stack-buffer the
  // read so we don't churn the heap inside this scan loop (up to 50 entries).
  char val[101];
  bool found = false;
  for (int i = 1; i <= num && i <= MAX_PAIR_SLOTS; i++) {
    char key[10];
    snprintf(key, sizeof(key), "Phone%d", i);
    size_t len = sizeof(val);
    if (nvs_get_str(h, key, val, &len) != ESP_OK || len == 0) continue;
    size_t slen = (val[len - 1] == '\0') ? len - 1 : len;
    if (slen == id.size() && memcmp(val, id.data(), slen) == 0) {
      found = true;
      break;
    }
  }
  nvs_close(h);
  return found;
}

// Append a new pair-id: write Phone<pair_num+1>, increment pair_num, set flag.
bool OemBleCompat::nvs_add_pair_id_(const std::string &id) {
  nvs_handle_t h;
  if (nvs_open("hx_list", NVS_READWRITE, &h) != ESP_OK) return false;

  // Validate sentinel first — consistent with nvs_has_pair_id_/nvs_pair_count_.
  // If flag is invalid (e.g. after a clear), start fresh at pair_num=0.
  uint8_t flag = 0;
  nvs_get_u8(h, "flag_PhoneID", &flag);
  uint8_t num = 0;
  if (flag == PAIR_FLAG_SENTINEL)
    nvs_get_u8(h, "pair_num", &num);
  if (num >= MAX_PAIR_SLOTS) { nvs_close(h); return false; }

  num++;
  char key[10];
  snprintf(key, sizeof(key), "Phone%d", num);
  esp_err_t err = nvs_set_str(h, key, id.c_str());
  if (err != ESP_OK) { nvs_close(h); return false; }

  nvs_set_u8(h, "pair_num", num);
  nvs_set_u8(h, "flag_PhoneID", PAIR_FLAG_SENTINEL);
  nvs_commit(h);
  nvs_close(h);
  return true;
}

// Wipe all Phone* entries, reset pair_num to 0, clear the flag sentinel.
void OemBleCompat::nvs_clear_pairs_() {
  nvs_handle_t h;
  if (nvs_open("hx_list", NVS_READWRITE, &h) != ESP_OK) return;

  uint8_t num = 0;
  nvs_get_u8(h, "pair_num", &num);
  for (int i = 1; i <= num && i <= MAX_PAIR_SLOTS; i++) {
    char key[10];
    snprintf(key, sizeof(key), "Phone%d", i);
    nvs_erase_key(h, key);
  }
  nvs_set_u8(h, "pair_num", 0);
  nvs_set_u8(h, "flag_PhoneID", 0);
  nvs_commit(h);
  nvs_close(h);
}

}  // namespace quietcool
}  // namespace esphome
