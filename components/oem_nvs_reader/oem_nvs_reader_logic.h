// OemNvsReaderLogic — pure, header-only decision function for the QuietCool
// "import OEM Wi-Fi credentials from ESP-IDF NVS into ESPHome" component.
// NO ESPHome includes — testable on host.
//
// The ESPHome-side wrapper (oem_nvs_reader.h/cpp) does the actual NVS
// reads via ESP-IDF and pushes the result into wifi::global_wifi_component.

#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace qc {

// On-disk schema for the OEM Smart Mode thresholds in the `hx_list` NVS
// namespace. Three components touch these exact keys + sentinels: the writer
// (oem_ble_compat::flush_hx_list_), the reader (read_smart_thresholds_from_nvs),
// and the consumer (fan_controller::import_oem_smart_thresholds_). Centralised
// here — the single OEM-NVS schema authority — so the write/read/interpret
// sides can't silently drift.
//
// A sentinel means "disabled / not set": the OEM writes it for an off
// threshold, and a missing key leaves OemSmartThresholds at the same value.
struct OemThresholdSchema {
  static constexpr const char *NAMESPACE = "hx_list";  // also holds non-threshold OEM keys
  static constexpr const char *KEY_TEMP_HIGH = "temper_set_h";
  static constexpr const char *KEY_TEMP_MED  = "temper_set_m";
  static constexpr const char *KEY_TEMP_LOW  = "temper_set_l";
  static constexpr const char *KEY_HUM_HIGH  = "hum_set_h";
  static constexpr const char *KEY_HUM_LOW   = "hum_set_l";
  static constexpr const char *KEY_HUM_RANK  = "hum_rank";
  static constexpr int16_t TEMP_SENTINEL = 0x7FFF;  // int16 temp keys
  static constexpr uint8_t HUM_SENTINEL  = 0xFF;    // uint8 humidity keys + hum_rank
};

// OEM Smart Mode thresholds (raw values read from the `hx_list` NVS namespace).
// Fields default to the OemThresholdSchema sentinels — the value a missing /
// disabled key resolves to.
struct OemSmartThresholds {
  int16_t temp_high_f = OemThresholdSchema::TEMP_SENTINEL;
  int16_t temp_med_f  = OemThresholdSchema::TEMP_SENTINEL;
  int16_t temp_low_f  = OemThresholdSchema::TEMP_SENTINEL;
  uint8_t hum_high    = OemThresholdSchema::HUM_SENTINEL;
  uint8_t hum_low     = OemThresholdSchema::HUM_SENTINEL;
  uint8_t hum_rank    = OemThresholdSchema::HUM_SENTINEL;
  bool any_valid() const {
    return temp_high_f != OemThresholdSchema::TEMP_SENTINEL ||
           temp_med_f != OemThresholdSchema::TEMP_SENTINEL ||
           temp_low_f != OemThresholdSchema::TEMP_SENTINEL ||
           hum_high != OemThresholdSchema::HUM_SENTINEL ||
           hum_low != OemThresholdSchema::HUM_SENTINEL ||
           hum_rank != OemThresholdSchema::HUM_SENTINEL;
  }
};

enum class OemImportDecision : uint8_t {
  Skip,           // do nothing (reason captured in OemImportPlan::skip_reason)
  ImportFromNvs,  // push import_ssid / import_password into ESPHome's wifi
};

// Input to the decision: everything the wrapper observed at boot time.
struct OemImportInput {
  std::string nvs_ssid;          // nvs.net80211 blob "sta.ssid"
  std::string nvs_password;      // nvs.net80211 blob "sta.pswd"
  bool nvs_read_ok = false;      // false if nvs_open or the get_str calls failed
  bool marker_already_set = false;     // qc_oem_imported_v1 already true
  bool esphome_has_saved_sta = false;  // ESPHome wifi already has its own creds
  std::vector<std::string> blocklist;  // case-insensitive SSID blocklist (e.g. ["HUAWEI"])
};

struct OemImportPlan {
  OemImportDecision decision;
  std::string import_ssid;       // valid only when ImportFromNvs
  std::string import_password;   // valid only when ImportFromNvs (may be empty for open networks)
  std::string skip_reason;       // diagnostic — set on Skip, empty on ImportFromNvs
};

class OemNvsReaderLogic {
 public:
  // Decide whether to import. Priority of skip reasons (first match wins):
  //   1. marker_already_set   — we already imported on a prior boot
  //   2. esphome_has_saved_sta — user used Improv / wrote creds via YAML
  //   3. !nvs_read_ok          — couldn't read the OEM NVS namespace at all
  //   4. ssid empty            — no creds to import
  //   5. ssid is on blocklist  — factory test creds (e.g. HUAWEI)
  //   6. otherwise             — ImportFromNvs
  static OemImportPlan decide_import(const OemImportInput &in) {
    OemImportPlan p;
    if (in.marker_already_set) {
      p.decision = OemImportDecision::Skip;
      p.skip_reason = "already imported on a prior boot (marker set)";
      return p;
    }
    if (in.esphome_has_saved_sta) {
      p.decision = OemImportDecision::Skip;
      p.skip_reason = "ESPHome already has saved STA — Improv or YAML took over";
      return p;
    }
    if (!in.nvs_read_ok) {
      p.decision = OemImportDecision::Skip;
      p.skip_reason = "OEM NVS namespace unreadable";
      return p;
    }
    if (in.nvs_ssid.empty()) {
      p.decision = OemImportDecision::Skip;
      p.skip_reason = "no SSID in OEM NVS";
      return p;
    }
    if (is_blocked_ssid(in.nvs_ssid, in.blocklist)) {
      p.decision = OemImportDecision::Skip;
      p.skip_reason = "SSID on factory-test blocklist";
      return p;
    }
    p.decision = OemImportDecision::ImportFromNvs;
    p.import_ssid = in.nvs_ssid;
    p.import_password = in.nvs_password;
    return p;
  }

  // Case-insensitive membership check. ASCII-only — locale-free.
  // Used by decide_import for the factory SSID blocklist.
  static bool is_blocked_ssid(const std::string &ssid,
                              const std::vector<std::string> &blocklist) {
    if (ssid.empty() || blocklist.empty()) return false;
    for (const auto &candidate : blocklist) {
      if (candidate.size() != ssid.size()) continue;
      bool match = true;
      for (size_t i = 0; i < ssid.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(ssid[i]);
        const unsigned char b = static_cast<unsigned char>(candidate[i]);
        if (std::tolower(a) != std::tolower(b)) {
          match = false;
          break;
        }
      }
      if (match) return true;
    }
    return false;
  }

  // OEM record-data ("Group_*") history blobs in the `hx_list` NVS namespace.
  // These hold the 31-day hourly temperature/humidity history that the OEM app
  // reads via the A=28 getRecordData command. Together they occupy ~2.7 KB of
  // NVS — by far the largest single class of OEM-only data. After this firmware
  // has been running for any length of time the data is frozen anyway (no
  // logger writing to it), so erasing it loses nothing even if the user later
  // reverts to stock firmware (stock rebuilds history over its next 31 days of
  // runtime).
  //
  // This list must exactly match the `Group_*` blobs the OEM firmware writes to
  // `hx_list`: every such blob must be here and vice versa, so cleanup can't
  // silently miss one. A companion check (test/verify_group_keys_fixture.py)
  // validates the list 1:1 against an NVS partition dump. Keep it to
  // dump-verifiable `Group_*` keys only — preset-name dupes go in their own
  // list below.
  static const std::vector<std::string> &group_history_keys() {
    static const std::vector<std::string> keys = {
        "Group_Tem",    // hourly temperatures, 24 samples × 31 days (~800 B blob)
        "Group_Hem",    // hourly humidities, same shape (~800 B blob)
        "Group_Time",   // hourly timestamps (~800 B blob)
        "Group_Year",   // year-level rollup (~64 B blob)
        "Group_month",  // month-level rollup (~64 B blob; OEM uses lowercase 'm')
    };
    return keys;
  }

  // OEM triplicated preset-name keys in the `hx_list` namespace. The OEM
  // firmware writes each preset name 3× (e.g. testnamem1 / testnamem11 /
  // testnamem111) but only the primary copy is read by GetPresets and this
  // firmware's import; the duplicates waste ~16 NVS entry slots. This list
  // mirrors the OEM write path, which writes each preset name 3×, rather than a
  // dump — a device with fewer presets configured won't have all 24 present, so
  // these are intentionally not dump-verified (and aren't `Group_*`, so they'd
  // fail the fixture's check).
  static const std::vector<std::string> &preset_name_dup_keys() {
    static const std::vector<std::string> keys = {
        "testnamem11", "testnamem111", "testnamem22", "testnamem222",
        "testnamem33", "testnamem333", "testnamem44", "testnamem444",
        "testnamel11", "testnamel111", "testnamel22", "testnamel222",
        "testnamel33", "testnamel333", "testnamel44", "testnamel444",
        "testnameh11", "testnameh111", "testnameh22", "testnameh222",
        "testnameh33", "testnameh333", "testnameh44", "testnameh444",
    };
    return keys;
  }
};

}  // namespace qc
