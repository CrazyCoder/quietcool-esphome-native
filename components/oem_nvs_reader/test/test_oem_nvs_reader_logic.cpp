// Host-side unit tests for OemNvsReaderLogic.
// Pure C++17, no ESPHome includes.
// Compile + run via run_tests.bat (or g++ -std=c++17 -I.. ...).

#include "test_utils.h"
#include "../oem_nvs_reader_logic.h"

using qc::OemImportDecision;
using qc::OemImportInput;
using qc::OemImportPlan;
using qc::OemNvsReaderLogic;

// Pretty-printers + equality for diagnostic output on REQUIRE_EQ failures.
namespace qc {
inline const char* decision_name(OemImportDecision d) {
  switch (d) {
    case OemImportDecision::Skip:           return "Skip";
    case OemImportDecision::ImportFromNvs:  return "ImportFromNvs";
  }
  return "??";
}
inline bool operator==(const OemImportPlan& a, const OemImportPlan& b) {
  return a.decision == b.decision && a.import_ssid == b.import_ssid &&
         a.import_password == b.import_password;
  // skip_reason intentionally NOT compared — it's diagnostic text that
  // can evolve without breaking behavior.
}
inline std::ostream& operator<<(std::ostream& os, const OemImportPlan& p) {
  return os << "{decision=" << decision_name(p.decision)
            << " ssid='" << p.import_ssid << "'"
            << " pass='" << p.import_password << "'"
            << " skip_reason='" << p.skip_reason << "'}";
}
}  // namespace qc

// Convenience constructors so test bodies stay readable.
static OemImportPlan import_plan(const std::string &ssid, const std::string &pass) {
  return OemImportPlan{OemImportDecision::ImportFromNvs, ssid, pass, ""};
}
static OemImportPlan skip_plan() {
  return OemImportPlan{OemImportDecision::Skip, "", "", ""};
}

// ============================================================================
// 1. Priority order — first matching skip reason wins, even when later
//    conditions would also skip. Ensures the decision tree is stable.
// ============================================================================

TEST("decide_import: marker_set wins even when NVS has valid creds") {
  OemImportInput in;
  in.nvs_ssid = "MyHomeWiFi";
  in.nvs_password = "secret123";
  in.nvs_read_ok = true;
  in.marker_already_set = true;       // <-- short-circuit reason
  in.esphome_has_saved_sta = false;
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in), skip_plan());
}

TEST("decide_import: esphome_has_sta wins over a valid NVS import") {
  OemImportInput in;
  in.nvs_ssid = "MyHomeWiFi";
  in.nvs_password = "secret123";
  in.nvs_read_ok = true;
  in.marker_already_set = false;
  in.esphome_has_saved_sta = true;    // <-- Improv / YAML already set creds
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in), skip_plan());
}

TEST("decide_import: NVS read failure skips (no creds source)") {
  OemImportInput in;
  in.nvs_ssid = "ignored";
  in.nvs_password = "ignored";
  in.nvs_read_ok = false;             // <-- nvs_open or nvs_get_str failed
  in.marker_already_set = false;
  in.esphome_has_saved_sta = false;
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in), skip_plan());
}

TEST("decide_import: empty SSID in NVS skips") {
  OemImportInput in;
  in.nvs_ssid = "";
  in.nvs_password = "whatever";
  in.nvs_read_ok = true;
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in), skip_plan());
}

// ============================================================================
// 2. Happy path — valid SSID + password.
// ============================================================================

TEST("decide_import: valid SSID + password -> ImportFromNvs") {
  OemImportInput in;
  in.nvs_ssid = "MyHomeWiFi";
  in.nvs_password = "secret123";
  in.nvs_read_ok = true;
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in),
             import_plan("MyHomeWiFi", "secret123"));
}

TEST("decide_import: valid SSID + empty password (open network) -> ImportFromNvs") {
  // Open Wi-Fi networks have an empty password — must still import.
  OemImportInput in;
  in.nvs_ssid = "CafeFreeWiFi";
  in.nvs_password = "";
  in.nvs_read_ok = true;
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in),
             import_plan("CafeFreeWiFi", ""));
}

TEST("decide_import: SSID with spaces and special chars -> ImportFromNvs (preserved verbatim)") {
  OemImportInput in;
  in.nvs_ssid = "My Home 5G+";
  in.nvs_password = "p@ssw0rd!#&";
  in.nvs_read_ok = true;
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in),
             import_plan("My Home 5G+", "p@ssw0rd!#&"));
}

// ============================================================================
// 3. Blocklist — factory test SSIDs we must NEVER import.
// ============================================================================

TEST("decide_import: blocklisted SSID (exact match) -> Skip") {
  OemImportInput in;
  in.nvs_ssid = "HUAWEI";
  in.nvs_password = "yaye@2025/";
  in.nvs_read_ok = true;
  in.blocklist = {"HUAWEI"};
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in), skip_plan());
}

TEST("decide_import: blocklist match is case-insensitive (lowercase NVS, uppercase rule)") {
  OemImportInput in;
  in.nvs_ssid = "huawei";
  in.nvs_password = "yaye@2025/";
  in.nvs_read_ok = true;
  in.blocklist = {"HUAWEI"};
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in), skip_plan());
}

TEST("decide_import: blocklist match is case-insensitive (mixed case)") {
  OemImportInput in;
  in.nvs_ssid = "HuAwEi";
  in.nvs_password = "yaye@2025/";
  in.nvs_read_ok = true;
  in.blocklist = {"huawei"};
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in), skip_plan());
}

TEST("decide_import: blocklist with multiple entries — any match skips") {
  OemImportInput in;
  in.nvs_ssid = "FactoryTest";
  in.nvs_password = "pw";
  in.nvs_read_ok = true;
  in.blocklist = {"HUAWEI", "FactoryTest", "AsiaBright"};
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in), skip_plan());
}

TEST("decide_import: SSID NOT on blocklist -> ImportFromNvs (sanity check)") {
  OemImportInput in;
  in.nvs_ssid = "MyHomeWiFi";
  in.nvs_password = "secret";
  in.nvs_read_ok = true;
  in.blocklist = {"HUAWEI"};
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in),
             import_plan("MyHomeWiFi", "secret"));
}

TEST("decide_import: empty blocklist never blocks") {
  OemImportInput in;
  in.nvs_ssid = "HUAWEI";  // would be blocked by default, but blocklist is empty here
  in.nvs_password = "pw";
  in.nvs_read_ok = true;
  in.blocklist = {};
  REQUIRE_EQ(OemNvsReaderLogic::decide_import(in),
             import_plan("HUAWEI", "pw"));
}

// ============================================================================
// 4. is_blocked_ssid helper — direct unit tests.
// ============================================================================

TEST("is_blocked_ssid: empty blocklist returns false") {
  REQUIRE(!OemNvsReaderLogic::is_blocked_ssid("anything", {}));
}

TEST("is_blocked_ssid: case-insensitive match") {
  REQUIRE(OemNvsReaderLogic::is_blocked_ssid("HUAWEI", {"huawei"}));
  REQUIRE(OemNvsReaderLogic::is_blocked_ssid("huawei", {"HUAWEI"}));
  REQUIRE(OemNvsReaderLogic::is_blocked_ssid("HuAwEi", {"HUAWEI"}));
}

TEST("is_blocked_ssid: empty SSID against non-empty blocklist returns false") {
  // Defensive — we expect callers to short-circuit on empty SSID already, but
  // the helper should still not match (an empty string isn't on a non-empty list).
  REQUIRE(!OemNvsReaderLogic::is_blocked_ssid("", {"HUAWEI"}));
}

TEST("is_blocked_ssid: partial-substring does NOT match (full string only)") {
  // "HUAWEI-5G" must NOT be blocked just because "HUAWEI" is in the blocklist —
  // we don't want false-positives on legitimate SSIDs that happen to share a prefix.
  REQUIRE(!OemNvsReaderLogic::is_blocked_ssid("HUAWEI-5G", {"HUAWEI"}));
  REQUIRE(!OemNvsReaderLogic::is_blocked_ssid("My-HUAWEI", {"HUAWEI"}));
}

// ============================================================================
// 5. group_history_keys() / preset_name_dup_keys() — the two static lists of
//    OEM keys we erase to free NVS space at first-ESPHome-boot.
// ============================================================================

TEST("group_history_keys: includes all 5 known OEM record-data blobs") {
  // The list must include every OEM hx_list/Group_* key that the firmware
  // writes. If the OEM firmware ever changes (new version adds a Group_ key,
  // or the case-sensitive spelling of Group_month changes), this list should
  // be updated. Matches an OEM NVS partition dump.
  const auto &keys = OemNvsReaderLogic::group_history_keys();
  std::vector<std::string> expected = {
      "Group_Tem",   // hourly temps, biggest blob (~800 B)
      "Group_Hem",   // hourly humidities (~800 B)
      "Group_Time",  // hourly timestamps (~800 B)
      "Group_Year",  // year-level rollup (~64 B)
      "Group_month", // month-level rollup (lowercase 'm' per OEM)
  };
  REQUIRE_EQ(keys.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    REQUIRE_EQ(keys[i], expected[i]);
  }
}

TEST("group_history_keys: case-sensitive — month is lowercase 'm'") {
  // The OEM is inconsistent: Group_Tem, Group_Hem, Group_Time, Group_Year
  // are all PascalCase, but Group_month uses a lowercase 'm'. nvs_erase_key is
  // case-sensitive so this spelling is load-bearing — if we erase
  // "Group_Month" instead the real key stays untouched.
  const auto &keys = OemNvsReaderLogic::group_history_keys();
  bool found_lowercase_month = false;
  for (const auto &k : keys) {
    if (k == "Group_month") found_lowercase_month = true;
    REQUIRE(k != "Group_Month");  // PascalCase variant must NOT be in the list
  }
  REQUIRE(found_lowercase_month);
}

TEST("preset_name_dup_keys: 24 triplicated keys, all testname*, none Group_*") {
  // The OEM writes each preset name 3× (testname{m,l,h}{1..4} plus *11/*111
  // suffixed dupes). These mirror the OEM firmware write path, not a dump, so
  // they live in a separate list from group_history_keys() and the fixture's
  // dump check skips them by design.
  const auto &keys = OemNvsReaderLogic::preset_name_dup_keys();
  REQUIRE_EQ(keys.size(), static_cast<size_t>(24));
  for (const auto &k : keys) {
    REQUIRE(k.rfind("testname", 0) == 0);  // every key starts with "testname"
    REQUIRE(k.rfind("Group_", 0) != 0);    // none are history blobs
  }
}

// ============================================================================
// OemSmartThresholds (moved from fan_controller — schema belongs with the reader)
// ============================================================================

TEST("OemSmartThresholds: all sentinels -> any_valid() = false") {
  qc::OemSmartThresholds t;
  REQUIRE_EQ(t.any_valid(), false);
}

TEST("OemSmartThresholds: one real temp value -> any_valid() = true") {
  qc::OemSmartThresholds t;
  t.temp_high_f = 100;
  REQUIRE_EQ(t.any_valid(), true);
}

TEST("OemSmartThresholds: hum_rank set alone -> any_valid() = true") {
  qc::OemSmartThresholds t;
  t.hum_rank = 2;
  REQUIRE_EQ(t.any_valid(), true);
}

int main() { return tu::run_all(); }
