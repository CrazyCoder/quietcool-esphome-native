#include "oem_nvs_reader.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/log.h"

#ifdef USE_ESP_IDF
#include "nvs.h"
#include "nvs_flash.h"
#endif

#include <algorithm>
#include <cstring>

namespace esphome {
namespace quietcool {

static const char *const TAG = "oem_nvs_reader";

// Marker preference — distinguishes "we've imported once" from "fresh device".
// Hash key 'qcnv' = QuietCool NVS reader; value 'OEM1' = imported marker v1.
// Both are stable across firmware versions — bump only on schema change.
static constexpr uint32_t MARKER_PREF_HASH = 0x71636E76;  // 'qcnv'
static constexpr uint32_t MARKER_VALUE_IMPORTED = 0x4F454D01;  // 'OEM' + 01

// Group_* blob cleanup marker — separate from the WiFi-import marker so the
// cleanup can be added retroactively to devices that already imported creds.
// Hash key 'qccg' = QuietCool cleanup-groups; value 'CLG2' = cleanup v2.
// Bumped from CLG1→CLG2 to re-run and erase triplicated preset-name keys.
static constexpr uint32_t CLEANUP_PREF_HASH = 0x71636367;  // 'qccg'
static constexpr uint32_t CLEANUP_VALUE_DONE = 0x434C4732;  // 'CLG2'

// Hash key the WiFiComponent uses for its SavedWifiSettings preference when
// no YAML-baked STA exists (see wifi_component.cpp WiFiComponent::start()).
// Writing to this slot is how we hand off credentials before WIFI setup runs.
static constexpr uint32_t WIFI_PREF_HASH_NO_YAML_STA = 88491487UL;

void OemNvsReader::setup() {
  // 1. Load our marker — has the OEM import already happened on a prior boot?
  this->marker_pref_ = global_preferences->make_preference<uint32_t>(
      MARKER_PREF_HASH, /*in_flash=*/true);
  uint32_t marker = 0;
  const bool marker_loaded = this->marker_pref_.load(&marker);

  // 2. Does the YAML build have its own STA? (a credential-bearing dev build
  //    has has_sta() true; the distribution build has it false.) If a YAML
  //    STA exists we defer — the dev meant their creds to win.
  const bool yaml_has_sta = (wifi::global_wifi_component != nullptr &&
                             wifi::global_wifi_component->has_sta());

  // 3. Build the decision input.
  qc::OemImportInput in;
  in.marker_already_set =
      marker_loaded && marker == MARKER_VALUE_IMPORTED;
  in.esphome_has_saved_sta = yaml_has_sta;
  in.blocklist = blocklist_;
  // Only read the OEM NVS when the result can change the decision. The marker
  // and saved-STA checks both rank above the NVS-derived reasons in
  // decide_import, so when either is set the read result is never inspected —
  // skip the flash open + two string reads on every steady-state boot.
  if (!in.marker_already_set && !in.esphome_has_saved_sta) {
    in.nvs_read_ok = this->read_oem_nvs_(&in.nvs_ssid, &in.nvs_password);
  }

  // 4. Pure logic decides.
  const auto plan = qc::OemNvsReaderLogic::decide_import(in);

  if (plan.decision == qc::OemImportDecision::ImportFromNvs) {
    // 5. Import: write the creds into ESPHome's WiFi preference slot, then
    //    set the marker so subsequent boots don't re-import.
    ESP_LOGCONFIG(TAG, "Importing OEM Wi-Fi creds: ssid='%s' (password %u chars)",
                  plan.import_ssid.c_str(),
                  static_cast<unsigned>(plan.import_password.size()));
    this->write_to_esphome_wifi_pref_(plan.import_ssid, plan.import_password);

    const uint32_t marker_val = MARKER_VALUE_IMPORTED;
    this->marker_pref_.save(&marker_val);
    global_preferences->sync();
  } else {
    ESP_LOGCONFIG(TAG, "Skipping OEM import: %s", plan.skip_reason.c_str());
  }

  // 6. Run the one-shot record-data cleanup. Independent of the wifi-import
  //    branch above (separate marker) so devices that already imported on a
  //    prior firmware version still get the space reclaim on first boot of
  //    this version.
  this->cleanup_oem_group_blobs_();
}

void OemNvsReader::dump_config() {
  ESP_LOGCONFIG(TAG, "QuietCool oem_nvs_reader:");
  ESP_LOGCONFIG(TAG, "  Setup priority: %.0f (must be > 250 = WIFI)",
                this->get_setup_priority());
  ESP_LOGCONFIG(TAG, "  Factory SSID blocklist: %u entries",
                static_cast<unsigned>(blocklist_.size()));
  for (const auto &ssid : blocklist_) {
    ESP_LOGCONFIG(TAG, "    - %s", ssid.c_str());
  }
  uint32_t cleanup_marker = 0;
  const bool loaded = this->cleanup_marker_pref_.load(&cleanup_marker);
  ESP_LOGCONFIG(TAG, "  Group_* NVS cleanup: %s",
                (loaded && cleanup_marker == CLEANUP_VALUE_DONE)
                    ? "done (marker set)" : "pending or failed");
}

bool OemNvsReader::read_oem_nvs_(std::string *ssid, std::string *password) {
  ssid->clear();
  password->clear();

#ifdef USE_ESP_IDF
  nvs_handle_t handle = 0;
  // esp_wifi persists wifi_config_t fields in its system namespace as fixed-
  // width blobs. Stock SetRouter reaches this path through esp_wifi_set_config.
  // These are not strings in a namespace called "nvs": live boot diagnostics
  // and partition dumps confirm nvs.net80211/{sta.ssid,sta.pswd}.
  esp_err_t err = nvs_open("nvs.net80211", NVS_READONLY, &handle);
  if (err != ESP_OK) {
    // ESP_ERR_NVS_NOT_FOUND is the normal "no nvs namespace" path on a fresh
    // device — log INFO not WARN to avoid spamming dump_config on every boot.
    ESP_LOGI(TAG, "nvs_open(\"nvs.net80211\", READONLY) returned %s — no OEM creds to import",
             esp_err_to_name(err));
    return false;
  }

  auto read_blob = [&](const char *key, size_t max_size, std::vector<uint8_t> *out) -> bool {
    size_t len = 0;
    esp_err_t e = nvs_get_blob(handle, key, nullptr, &len);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
      out->clear();
      return true;  // missing key is not a hard failure
    }
    if (e != ESP_OK) {
      ESP_LOGW(TAG, "nvs_get_str(\"%s\") sizing failed: %s", key,
               esp_err_to_name(e));
      return false;
    }
    if (len == 0 || len > max_size) {
      ESP_LOGW(TAG, "nvs_get_blob(\"%s\") returned implausible len=%u",
               key, static_cast<unsigned>(len));
      return false;
    }
    out->assign(len, 0);
    e = nvs_get_blob(handle, key, out->data(), &len);
    if (e != ESP_OK) {
      ESP_LOGW(TAG, "nvs_get_blob(\"%s\") read failed: %s", key,
               esp_err_to_name(e));
      return false;
    }
    return true;
  };

  std::vector<uint8_t> ssid_blob;
  std::vector<uint8_t> password_blob;
  bool ok = read_blob("sta.ssid", 36, &ssid_blob) &&
            read_blob("sta.pswd", 65, &password_blob);
  if (ok && !ssid_blob.empty()) {
    // ESP-IDF stores the SSID as uint32 length + uint8_t ssid[32]. Confirmed
    // against the factory/post-OTA NVS image (36-byte blob).
    if (ssid_blob.size() != 36) {
      ESP_LOGW(TAG, "sta.ssid blob has unexpected size=%u",
               static_cast<unsigned>(ssid_blob.size()));
      ok = false;
    } else {
      uint32_t ssid_len = 0;
      std::memcpy(&ssid_len, ssid_blob.data(), sizeof(ssid_len));
      if (ssid_len > 32) {
        ESP_LOGW(TAG, "sta.ssid blob has invalid embedded length=%u",
                 static_cast<unsigned>(ssid_len));
        ok = false;
      } else {
        ssid->assign(reinterpret_cast<const char *>(ssid_blob.data() + 4), ssid_len);
      }
    }
  }
  if (ok && !password_blob.empty()) {
    // ESP-IDF stores a 65-byte null-terminated password buffer.
    auto end = std::find(password_blob.begin(), password_blob.end(), uint8_t{0});
    password->assign(reinterpret_cast<const char *>(password_blob.data()),
                     static_cast<size_t>(end - password_blob.begin()));
  }
  nvs_close(handle);
  return ok;
#else
  (void)ssid;
  (void)password;
  ESP_LOGW(TAG, "Built without ESP-IDF — OEM NVS read unavailable");
  return false;
#endif
}

bool OemNvsReader::read_smart_thresholds_from_nvs(qc::OemSmartThresholds *out) {
  if (!out) return false;
#ifdef USE_ESP_IDF
  using Schema = qc::OemThresholdSchema;
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(Schema::NAMESPACE, NVS_READONLY, &handle);
  if (err != ESP_OK) return false;
  auto read_i16 = [&](const char *key, int16_t *o) {
    esp_err_t e = nvs_get_i16(handle, key, o);
    if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
      ESP_LOGW(TAG, "nvs_get_i16(%s) failed: %s", key, esp_err_to_name(e));
  };
  auto read_u8 = [&](const char *key, uint8_t *o) {
    esp_err_t e = nvs_get_u8(handle, key, o);
    if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
      ESP_LOGW(TAG, "nvs_get_u8(%s) failed: %s", key, esp_err_to_name(e));
  };
  read_i16(Schema::KEY_TEMP_HIGH, &out->temp_high_f);
  read_i16(Schema::KEY_TEMP_MED, &out->temp_med_f);
  read_i16(Schema::KEY_TEMP_LOW, &out->temp_low_f);
  read_u8(Schema::KEY_HUM_HIGH, &out->hum_high);
  read_u8(Schema::KEY_HUM_LOW, &out->hum_low);
  read_u8(Schema::KEY_HUM_RANK, &out->hum_rank);
  nvs_close(handle);
  return true;
#else
  (void)out;
  return false;
#endif
}

void OemNvsReader::cleanup_oem_group_blobs_() {
  // Idempotent: gated by its own marker. If the marker write fails (because
  // NVS is already too full), the cleanup still runs — next boot will retry
  // both the erase and the marker write. The erase itself is also idempotent
  // since nvs_erase_key on a missing key returns ESP_ERR_NVS_NOT_FOUND, which
  // we treat as "already done, fine".
  this->cleanup_marker_pref_ = global_preferences->make_preference<uint32_t>(
      CLEANUP_PREF_HASH, /*in_flash=*/true);
  uint32_t marker = 0;
  if (this->cleanup_marker_pref_.load(&marker) && marker == CLEANUP_VALUE_DONE) {
    ESP_LOGCONFIG(TAG, "OEM Group_* cleanup already done (marker set)");
    return;
  }

#ifdef USE_ESP_IDF
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open("hx_list", NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    const auto &history = qc::OemNvsReaderLogic::group_history_keys();
    const auto &presets = qc::OemNvsReaderLogic::preset_name_dup_keys();
    size_t erased = 0;
    size_t missing = 0;
    auto erase_all = [&](const std::vector<std::string> &keys) {
      for (const auto &k : keys) {
        esp_err_t e = nvs_erase_key(handle, k.c_str());
        if (e == ESP_OK) {
          erased++;
        } else if (e == ESP_ERR_NVS_NOT_FOUND) {
          missing++;
        } else {
          ESP_LOGW(TAG, "nvs_erase_key(\"%s\") failed: %s", k.c_str(),
                   esp_err_to_name(e));
        }
      }
    };
    erase_all(history);
    erase_all(presets);
    err = nvs_commit(handle);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "nvs_commit after Group_* erase failed: %s",
               esp_err_to_name(err));
    }
    nvs_close(handle);
    ESP_LOGCONFIG(TAG, "OEM NVS cleanup: erased=%u missing=%u (of %u keys)",
                  static_cast<unsigned>(erased), static_cast<unsigned>(missing),
                  static_cast<unsigned>(history.size() + presets.size()));
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "hx_list namespace not present — nothing to clean up");
  } else {
    // Hard open failure (e.g. NVS too full to open RW): leave the marker unset
    // so the cleanup retries on the next boot.
    ESP_LOGW(TAG, "nvs_open(hx_list, RW) failed: %s — Group_* cleanup deferred",
             esp_err_to_name(err));
    return;
  }

  // Set the marker AFTER the erase (on success OR an absent namespace) so a
  // marker-write failure can't skip the actual cleanup on retry, and a fresh
  // device doesn't re-probe every boot.
  const uint32_t done = CLEANUP_VALUE_DONE;
  this->cleanup_marker_pref_.save(&done);
  global_preferences->sync();
#else
  ESP_LOGW(TAG, "Built without ESP-IDF — Group_* cleanup unavailable");
#endif
}

void OemNvsReader::write_to_esphome_wifi_pref_(const std::string &ssid,
                                               const std::string &password) {
  // Write directly to the WiFiComponent's preference slot rather than calling
  // save_wifi_sta(): our setup() runs at priority 800 (HARDWARE), before
  // WIFI=250, so WiFiComponent::pref_ hasn't been initialized via
  // make_preference yet — calling save_wifi_sta would write to a default-
  // constructed pref. By taking the same hash key directly we sidestep the
  // ordering issue: WiFiComponent::start() later does its own make_preference
  // on the same key and reads our saved value.
  auto wifi_pref = global_preferences->make_preference<wifi::SavedWifiSettings>(
      WIFI_PREF_HASH_NO_YAML_STA, /*in_flash=*/true);
  wifi::SavedWifiSettings save{};  // zero-initialized => null-terminated
  std::strncpy(save.ssid, ssid.c_str(), sizeof(save.ssid) - 1);
  std::strncpy(save.password, password.c_str(), sizeof(save.password) - 1);
  wifi_pref.save(&save);
  global_preferences->sync();
}

}  // namespace quietcool
}  // namespace esphome
