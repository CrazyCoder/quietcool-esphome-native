// ESPHome wrapper for the QuietCool OEM-NVS Wi-Fi credential importer.
// Runs once at boot before WIFI setup. Reads the ESP-IDF "nvs" namespace
// (where the OEM firmware stored Wi-Fi creds via esp_wifi_set_config),
// then injects them into ESPHome's WiFiComponent so a credential-free
// firmware build picks up the user's prior Wi-Fi config seamlessly.
//
// Decision logic lives in oem_nvs_reader_logic.h and is unit-tested on the
// host. This file is glue: NVS reads + ESPHome preference writes.

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "oem_nvs_reader_logic.h"

#include <string>
#include <vector>

namespace esphome {
namespace quietcool {

class OemNvsReader : public Component {
 public:
  // Setter populated by codegen (factory_ssid_blocklist YAML option).
  void add_blocked_ssid(const std::string &ssid) { blocklist_.push_back(ssid); }

  // Read OEM Smart Mode thresholds out of the `hx_list` NVS namespace.
  // Static — no instance needed; the read is stateless and the namespace name
  // + key set + sentinels are OEM-NVS schema knowledge owned by this component.
  // Returns false if the namespace can't be opened (fresh device or running
  // without ESP-IDF). On success, missing keys leave the corresponding field
  // at its sentinel (caller checks with any_valid() / per-field comparison).
  static bool read_smart_thresholds_from_nvs(qc::OemSmartThresholds *out);

  void setup() override;
  void dump_config() override;
  // Must run BEFORE WIFI (priority 250) so our preference write is in place
  // when WiFiComponent::start() does its make_preference + pref_.load() on
  // the same NVS key. HARDWARE (800) is safely above and matches the
  // semantic intent (we're talking to flash, not a downstream component).
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  // Reads ESP-IDF "nvs" namespace, keys sta.ssid / sta.password.
  // Returns true iff nvs_open succeeded; *ssid / *password are populated
  // with the values found (empty if a key was missing — not an error).
  // Returns false on hard read failure (corrupt NVS, partition missing).
  bool read_oem_nvs_(std::string *ssid, std::string *password);

  // Persists the imported creds into ESPHome's WiFiComponent preference slot
  // (the same NVS entry WiFiComponent::start() reads at priority 250). Uses
  // the hash key the WiFi component computes when YAML has no STA configured.
  void write_to_esphome_wifi_pref_(const std::string &ssid,
                                   const std::string &password);

  // One-shot cleanup: erase the OEM hx_list/Group_* record-data blobs to
  // reclaim ~2.7 KB of NVS partition. Gated by its own marker so it runs
  // exactly once per device. The erased keys are listed in
  // qc::OemNvsReaderLogic::group_history_keys() + preset_name_dup_keys(). The
  // OEM NVS partition is only 16 KB with limited compaction headroom, so this
  // reclaim matters.
  void cleanup_oem_group_blobs_();

  std::vector<std::string> blocklist_;
  ESPPreferenceObject marker_pref_;
  ESPPreferenceObject cleanup_marker_pref_;
};

}  // namespace quietcool
}  // namespace esphome
