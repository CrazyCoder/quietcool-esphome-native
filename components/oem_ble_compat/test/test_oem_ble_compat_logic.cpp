// Host-side unit tests for OemBleCompatLogic — pair-state machine, gate
// checks, field-format conversions, frame assembly, response chunking.
//
// Compile + run:
//   g++ -std=c++17 -I.. test_oem_ble_compat_logic.cpp -o test_oem_ble_compat_logic.exe
//   ./test_oem_ble_compat_logic.exe

#include "test_utils.h"
#include "../oem_ble_compat_logic.h"

using namespace qc;

// ── Pretty-printers ─────────────────────────────────────────────────

namespace qc {
inline std::ostream &operator<<(std::ostream &os, PairState s) {
  switch (s) {
    case PairState::Init:     return os << "Init";
    case PairState::Auth:     return os << "Auth";
    case PairState::PairMode: return os << "PairMode";
  }
  return os << "?";
}
inline std::ostream &operator<<(std::ostream &os, GateResult r) {
  switch (r) {
    case GateResult::Allowed:     return os << "Allowed";
    case GateResult::NeedAuth:    return os << "NeedAuth";
    case GateResult::OtaBlocked:  return os << "OtaBlocked";
    case GateResult::PreGateOnly: return os << "PreGateOnly";
  }
  return os << "?";
}
inline std::ostream &operator<<(std::ostream &os, UpgradeDecision d) {
  switch (d) {
    case UpgradeDecision::Reject:           return os << "Reject";
    case UpgradeDecision::BlockedOemDomain: return os << "BlockedOemDomain";
    case UpgradeDecision::Flash:            return os << "Flash";
  }
  return os << "?";
}
}  // namespace qc

// ============================================================================
// 1. validate_phone_id — input validation (production login/pair pre-checks)
// ============================================================================

TEST("validate_phone_id: normal id → true") {
  REQUIRE(validate_phone_id("abc123"));
}

TEST("validate_phone_id: empty → false") {
  REQUIRE(!validate_phone_id(""));
}

TEST("validate_phone_id: exactly 100 chars → true") {
  REQUIRE(validate_phone_id(std::string(100, 'y')));
}

TEST("validate_phone_id: 101 chars → false") {
  REQUIRE(!validate_phone_id(std::string(101, 'x')));
}

TEST("GetVersion exactly matches the OEM production channel") {
  REQUIRE_EQ(std::string(get_version_response()),
             std::string(R"({"A":3,"V":"IT-BLT-ATTICFAN_V4.1","P":100,"D":"2025.11.18","M":"online","H":"A"})"));
}

// ============================================================================
// 3. PairMachine — pair mode + timeout
// ============================================================================

TEST("enter_pair_mode: transitions to PairMode") {
  PairMachine pm;
  pm.enter_pair_mode(5000, 120000);
  REQUIRE_EQ(pm.state, PairState::PairMode);
  REQUIRE_EQ(pm.pair_mode_deadline_ms, uint32_t(125000));
}

TEST("pair mode timeout: reverts to Init when no prior auth") {
  PairMachine pm;
  pm.enter_pair_mode(1000, 2000);
  pm.check_timeout(2999);
  REQUIRE_EQ(pm.state, PairState::PairMode);  // not yet
  pm.check_timeout(3000);
  REQUIRE_EQ(pm.state, PairState::Init);
}

TEST("pair mode timeout: reverts to Auth when previously authenticated") {
  PairMachine pm;
  pm.state = PairState::Auth;
  pm.current_pair_id = "known";
  pm.enter_pair_mode(5000, 1000);
  REQUIRE_EQ(pm.state, PairState::PairMode);
  pm.check_timeout(6000);
  REQUIRE_EQ(pm.state, PairState::Auth);
}

TEST("check_timeout: no-op when not in PairMode") {
  PairMachine pm;
  pm.check_timeout(999999);
  REQUIRE_EQ(pm.state, PairState::Init);
}

// ============================================================================
// 4. Gate checks
// ============================================================================

TEST("gate: Login (A=13) always PreGateOnly regardless of state") {
  REQUIRE_EQ(check_gate(13, PairState::Init, false), GateResult::PreGateOnly);
  REQUIRE_EQ(check_gate(13, PairState::Auth, true),  GateResult::PreGateOnly);
}

TEST("gate: Pair (A=14) always PreGateOnly") {
  REQUIRE_EQ(check_gate(14, PairState::Init, false), GateResult::PreGateOnly);
}

TEST("gate: PairMode (A=15) requires auth (matches stock dispatcher)") {
  // Stock ble_v2_dispatcher_main only reaches PairMode inside the
  // pair_state==1 branch — remote PairMode needs an authenticated session.
  // An unpaired device can only enter pair mode via the physical KEY2 button.
  REQUIRE_EQ(check_gate(15, PairState::Init, false),     GateResult::NeedAuth);
  REQUIRE_EQ(check_gate(15, PairState::PairMode, false), GateResult::NeedAuth);
  REQUIRE_EQ(check_gate(15, PairState::Auth, false),     GateResult::Allowed);
  REQUIRE_EQ(check_gate(15, PairState::Auth, true),      GateResult::OtaBlocked);
}

TEST("gate: GetWorkState (A=1) needs auth") {
  REQUIRE_EQ(check_gate(1, PairState::Init, false), GateResult::NeedAuth);
  REQUIRE_EQ(check_gate(1, PairState::Auth, false), GateResult::Allowed);
}

TEST("gate: authed but OTA in progress → blocked for normal commands") {
  REQUIRE_EQ(check_gate(1, PairState::Auth, true), GateResult::OtaBlocked);
  REQUIRE_EQ(check_gate(9, PairState::Auth, true), GateResult::OtaBlocked);
}

TEST("gate: GetUpgradeState (A=5) allowed during OTA") {
  REQUIRE_EQ(check_gate(5, PairState::Auth, true), GateResult::Allowed);
}

TEST("gate: all state-query commands need auth") {
  int queries[] = {1, 2, 3, 4, 5, 8, 17, 19};
  for (int a : queries) {
    REQUIRE_EQ(check_gate(a, PairState::Init, false), GateResult::NeedAuth);
    REQUIRE_EQ(check_gate(a, PairState::Auth, false), GateResult::Allowed);
  }
}

TEST("gate: all state-mutating commands need auth + no OTA") {
  int mutations[] = {6, 7, 9, 10, 11, 16, 18, 20, 21, 22};
  for (int a : mutations) {
    REQUIRE_EQ(check_gate(a, PairState::Init, false), GateResult::NeedAuth);
    REQUIRE_EQ(check_gate(a, PairState::Auth, false), GateResult::Allowed);
    REQUIRE_EQ(check_gate(a, PairState::Auth, true),  GateResult::OtaBlocked);
  }
}

// ============================================================================
// 4b. SetRouter switch decision
// ============================================================================

TEST("setrouter_should_switch: skip when already on the same SSID") {
  // OEM app resends the current network during an update attempt — no switch.
  REQUIRE_EQ(setrouter_should_switch(true, "HomeNet", "HomeNet"), false);
}

TEST("setrouter_should_switch: switch when the SSID changes") {
  REQUIRE_EQ(setrouter_should_switch(true, "HomeNet", "OtherNet"), true);
}

TEST("setrouter_should_switch: switch when disconnected (recovery)") {
  // Fell off the network (e.g. AP password changed) — re-apply even if the
  // SSID name is unchanged.
  REQUIRE_EQ(setrouter_should_switch(false, "HomeNet", "HomeNet"), true);
  REQUIRE_EQ(setrouter_should_switch(false, "", "HomeNet"), true);
}

// ============================================================================
// 5. Field format conversions
// ============================================================================

TEST("temp_c_to_f_x10: 0°C = 320 (32.0°F × 10)") {
  REQUIRE_EQ(temp_c_to_f_x10(0.0f), 320);
}

TEST("temp_c_to_f_x10: 100°C = 2120 (212.0°F × 10)") {
  REQUIRE_EQ(temp_c_to_f_x10(100.0f), 2120);
}

TEST("temp_c_to_f_x10: 25.5°C ≈ 77.9°F → 779") {
  REQUIRE_EQ(temp_c_to_f_x10(25.5f), 779);
}

TEST("threshold_c_to_f: 37.8°C ≈ 100°F") {
  REQUIRE_EQ(threshold_c_to_f(37.8f), 100);
}

TEST("threshold_c_to_f: 32.2°C ≈ 90°F") {
  REQUIRE_EQ(threshold_c_to_f(32.2f), 90);
}

TEST("threshold_c_to_f: 26.7°C ≈ 80°F") {
  REQUIRE_EQ(threshold_c_to_f(26.7f), 80);
}

TEST("threshold_f_to_c: 100°F ≈ 37.78°C") {
  float c = threshold_f_to_c(100);
  REQUIRE(c > 37.7f && c < 37.9f);
}

TEST("dip_to_oem: TwoSpeed=1 → TWO, ThreeSpeed=2 → THREE, OneSpeed=3 → ONE") {
  REQUIRE_EQ(std::string(dip_to_oem(1)), std::string("TWO"));
  REQUIRE_EQ(std::string(dip_to_oem(2)), std::string("THREE"));
  REQUIRE_EQ(std::string(dip_to_oem(3)), std::string("ONE"));
  REQUIRE_EQ(std::string(dip_to_oem(0)), std::string("NO"));
}

TEST("speed_to_oem: Off/Low/Med/High → OFF/LOW/MEDIUM/HIGH") {
  REQUIRE_EQ(std::string(speed_to_oem(0)), std::string("OFF"));
  REQUIRE_EQ(std::string(speed_to_oem(1)), std::string("LOW"));
  REQUIRE_EQ(std::string(speed_to_oem(2)), std::string("MEDIUM"));
  REQUIRE_EQ(std::string(speed_to_oem(3)), std::string("HIGH"));
}

TEST("oem_speed_to_internal: round-trips") {
  REQUIRE_EQ(oem_speed_to_internal("HIGH"), uint8_t(3));
  REQUIRE_EQ(oem_speed_to_internal("MEDIUM"), uint8_t(2));
  REQUIRE_EQ(oem_speed_to_internal("LOW"), uint8_t(1));
  REQUIRE_EQ(oem_speed_to_internal("OFF"), uint8_t(0));
  REQUIRE_EQ(oem_speed_to_internal("MED"), uint8_t(2));
  REQUIRE_EQ(oem_speed_to_internal(nullptr), uint8_t(0));
}

TEST("mode_to_oem: all modes per OEM app Constants.java") {
  REQUIRE_EQ(std::string(mode_to_oem(false, false, false)), std::string("Idle"));
  REQUIRE_EQ(std::string(mode_to_oem(true, true, false)),   std::string("TH"));
  REQUIRE_EQ(std::string(mode_to_oem(true, false, true)),   std::string("Timer"));
  REQUIRE_EQ(std::string(mode_to_oem(true, false, false)),  std::string("Run"));
}

TEST("mode_to_oem: Smart Mode active but fan off → TH (not Idle)") {
  REQUIRE_EQ(std::string(mode_to_oem(false, true, false)), std::string("TH"));
  REQUIRE_EQ(std::string(mode_to_oem(false, true, true)),  std::string("TH"));
}

TEST("hum_response_to_oem: select → OEM range string") {
  REQUIRE_EQ(std::string(hum_response_to_oem("Off")),    std::string("CLOSE"));
  REQUIRE_EQ(std::string(hum_response_to_oem("Low")),    std::string("LOW"));
  REQUIRE_EQ(std::string(hum_response_to_oem("Medium")), std::string("MEDIUM"));
  REQUIRE_EQ(std::string(hum_response_to_oem("High")),   std::string("HIGH"));
  REQUIRE_EQ(std::string(hum_response_to_oem(nullptr)),   std::string("CLOSE"));
}

TEST("parse_oem_mode: per OEM app Constants.java") {
  auto th = parse_oem_mode("TH");
  REQUIRE(!th.turn_on);
  REQUIRE(th.smart_mode);
  REQUIRE(!th.timer_mode);

  auto timer = parse_oem_mode("Timer");
  REQUIRE(timer.turn_on);
  REQUIRE(!timer.smart_mode);
  REQUIRE(timer.timer_mode);

  auto run = parse_oem_mode("Run");
  REQUIRE(run.turn_on);
  REQUIRE(!run.smart_mode);
  REQUIRE(!run.timer_mode);

  auto idle = parse_oem_mode("Idle");
  REQUIRE(!idle.turn_on);
  REQUIRE(!idle.smart_mode);
  REQUIRE(!idle.timer_mode);
}

// ============================================================================
// 6. Frame assembler
// ============================================================================

TEST("frame: single complete JSON in one chunk") {
  FrameAssembler fa;
  std::string json = R"({"A":1})";
  fa.feed(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  REQUIRE(fa.has_complete());
  auto msg = fa.take();
  REQUIRE_EQ(msg.size(), json.size());
  REQUIRE_EQ(std::string(msg.begin(), msg.end()), json);
  REQUIRE(!fa.has_complete());
}

TEST("frame: JSON split across two chunks") {
  FrameAssembler fa;
  std::string part1 = R"({"A":1)";
  std::string part2 = R"(3,"P":"abc"})";
  fa.feed(reinterpret_cast<const uint8_t *>(part1.data()), part1.size());
  REQUIRE(!fa.has_complete());
  fa.feed(reinterpret_cast<const uint8_t *>(part2.data()), part2.size());
  REQUIRE(fa.has_complete());
  auto msg = fa.take();
  REQUIRE_EQ(std::string(msg.begin(), msg.end()), part1 + part2);
}

TEST("frame: nested braces handled") {
  FrameAssembler fa;
  std::string json = R"({"A":19,"P":[["Summer",120]]})";
  fa.feed(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  REQUIRE(fa.has_complete());
}

TEST("frame: binary command (0x1C) detected") {
  FrameAssembler fa;
  uint8_t data[] = {'{', 0x1C, 0x05, '}'};
  fa.feed(data, 4);
  REQUIRE(fa.has_complete());
  auto msg = fa.take();
  REQUIRE_EQ(msg.size(), size_t(4));
  REQUIRE_EQ(msg[1], uint8_t(0x1C));
}

TEST("frame: binary command (0x1D SynchronizeTime) 13 bytes") {
  FrameAssembler fa;
  uint8_t data[13] = {'{', 0x1D, '1','7','4','8','2','8','0','0','0','0', '}'};
  fa.feed(data, 13);
  REQUIRE(fa.has_complete());
  auto msg = fa.take();
  REQUIRE_EQ(msg.size(), size_t(13));
}

TEST("frame: two JSON messages in sequence") {
  FrameAssembler fa;
  std::string both = R"({"A":1}{"A":2})";
  fa.feed(reinterpret_cast<const uint8_t *>(both.data()), both.size());
  REQUIRE(fa.has_complete());
  auto msg1 = fa.take();
  REQUIRE_EQ(std::string(msg1.begin(), msg1.end()), std::string(R"({"A":1})"));
  REQUIRE(fa.has_complete());
  auto msg2 = fa.take();
  REQUIRE_EQ(std::string(msg2.begin(), msg2.end()), std::string(R"({"A":2})"));
  REQUIRE(!fa.has_complete());
}

TEST("frame: braces inside JSON string cause premature completion (known limitation)") {
  FrameAssembler fa;
  std::string json = R"({"A":16,"N":"my{fan}"})";
  fa.feed(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  // The brace counter sees the } inside the string as depth=0.
  // This is a known limitation — validate_fan_name rejects braces.
  REQUIRE(fa.has_complete());
}

TEST("frame: clear resets state") {
  FrameAssembler fa;
  std::string partial = R"({"A":)";
  fa.feed(reinterpret_cast<const uint8_t *>(partial.data()), partial.size());
  REQUIRE(!fa.has_complete());
  fa.clear();
  REQUIRE(fa.buf.empty());
}

// ============================================================================
// 7. Response chunking
// ============================================================================

TEST("chunk: small response fits in one chunk at large MTU") {
  auto data = qq_wrap(R"({"A":15,"R":"Success"})");
  auto chunks = chunk_response(data, 200);  // large MTU → one chunk
  REQUIRE_EQ(chunks.size(), size_t(1));
  REQUIRE_EQ(chunks[0].size(), data.size());
}

TEST("chunk: typical Login response at default MTU splits into 2") {
  auto data = qq_wrap(R"({"A":13,"R":"Success","P":"No"})");
  // 2 + 31 = 33 bytes; MTU 23 → chunk_size 20 → ceil(33/20) = 2
  auto chunks = chunk_response(data, 23);
  REQUIRE_EQ(chunks.size(), size_t(2));
  size_t total = 0;
  for (auto &c : chunks) total += c.size();
  REQUIRE_EQ(total, data.size());
}

TEST("chunk: large response splits at MTU-3 boundary") {
  std::string json(60, 'x');
  auto data = qq_wrap(json);
  auto chunks = chunk_response(data, 23);  // chunk_size = 20
  REQUIRE_EQ(chunks.size(), size_t(4));  // ceil(62/20) = 4
  size_t total = 0;
  for (auto &c : chunks) total += c.size();
  REQUIRE_EQ(total, data.size());
}

TEST("chunk: empty data produces one empty chunk") {
  std::vector<uint8_t> empty;
  auto chunks = chunk_response(empty, 23);
  REQUIRE_EQ(chunks.size(), size_t(1));
  REQUIRE(chunks[0].empty());
}

TEST("qq_wrap: prepends QQ to JSON") {
  auto wrapped = qq_wrap(R"({"A":1})");
  REQUIRE_EQ(wrapped.size(), size_t(9));
  REQUIRE_EQ(wrapped[0], uint8_t('Q'));
  REQUIRE_EQ(wrapped[1], uint8_t('Q'));
  REQUIRE_EQ(wrapped[2], uint8_t('{'));
}

// ============================================================================
// 8. Input validation
// ============================================================================

TEST("validate_phone_id: empty → false, 100 chars → true, 101 → false") {
  REQUIRE(!validate_phone_id(""));
  REQUIRE(validate_phone_id(std::string(100, 'a')));
  REQUIRE(!validate_phone_id(std::string(101, 'a')));
}

TEST("validate_url: empty → false, 100 → true, 101 → false") {
  REQUIRE(!validate_url(""));
  REQUIRE(validate_url(std::string(100, 'u')));
  REQUIRE(!validate_url(std::string(101, 'u')));
}

TEST("validate_ssid: empty → false, 32 → true, 33 → false") {
  REQUIRE(!validate_ssid(""));
  REQUIRE(validate_ssid(std::string(32, 's')));
  REQUIRE(!validate_ssid(std::string(33, 's')));
}

TEST("validate_password: empty → true, 64 → true, 65 → false") {
  REQUIRE(validate_password(""));
  REQUIRE(validate_password(std::string(64, 'p')));
  REQUIRE(!validate_password(std::string(65, 'p')));
}

TEST("validate_fan_name: rejects braces (frame assembler safety)") {
  REQUIRE(!validate_fan_name("my{fan}"));
  REQUIRE(!validate_fan_name("{"));
  REQUIRE(validate_fan_name("Normal Fan Name"));
  REQUIRE(validate_fan_name(""));
}

TEST("validate_threshold: 0-255 valid, -1 and 256 invalid") {
  REQUIRE(validate_threshold(0));
  REQUIRE(validate_threshold(255));
  REQUIRE(!validate_threshold(-1));
  REQUIRE(!validate_threshold(256));
}

// ── Fan model catalogue ────────────────────────────────────────────

TEST("fan_model_display: index → display name") {
  REQUIRE(strcmp(fan_model_display("0"), "Generic") == 0);
  REQUIRE(strcmp(fan_model_display("1"), "AFG SMT PRO-2.0") == 0);
  REQUIRE(strcmp(fan_model_display("4"), "AFR SMT ES-2.0(1st Generation)") == 0);
  REQUIRE(strcmp(fan_model_display("7"), "AFR SMT ES-2.0(2nd Generation)") == 0);
  REQUIRE(strcmp(fan_model_display("99"), "Generic") == 0);
  REQUIRE(strcmp(fan_model_display(nullptr), "Generic") == 0);
}

TEST("fan_model_index: display name → index") {
  REQUIRE(strcmp(fan_model_index("Generic"), "0") == 0);
  REQUIRE(strcmp(fan_model_index("AFG SMT PRO-2.0"), "1") == 0);
  REQUIRE(strcmp(fan_model_index("AFR SMT ES-2.0(2nd Generation)"), "7") == 0);
  REQUIRE(strcmp(fan_model_index("Unknown Model"), "0") == 0);
  REQUIRE(strcmp(fan_model_index(nullptr), "0") == 0);
}

// ============================================================================
// classify_upgrade_url — Upgrade (A=10) URL gating
// ============================================================================

TEST("classify: empty → Reject") {
  REQUIRE_EQ(classify_upgrade_url(""), UpgradeDecision::Reject);
}
TEST("classify: over 100 chars → Reject") {
  REQUIRE_EQ(classify_upgrade_url("http://x.com/" + std::string(100, 'a')),
             UpgradeDecision::Reject);
}
TEST("classify: no scheme → Reject") {
  REQUIRE_EQ(classify_upgrade_url("example.com/fw.bin"), UpgradeDecision::Reject);
}
TEST("classify: ftp scheme → Reject") {
  REQUIRE_EQ(classify_upgrade_url("ftp://example.com/fw.bin"), UpgradeDecision::Reject);
}
TEST("classify: myquietcool.com → BlockedOemDomain") {
  REQUIRE_EQ(classify_upgrade_url("http://myquietcool.com/profile/x.bin"),
             UpgradeDecision::BlockedOemDomain);
}
TEST("classify: subdomain of myquietcool.com → BlockedOemDomain") {
  REQUIRE_EQ(classify_upgrade_url("https://cdn.myquietcool.com/x.bin"),
             UpgradeDecision::BlockedOemDomain);
}
TEST("classify: quietcool.com → BlockedOemDomain") {
  REQUIRE_EQ(classify_upgrade_url("http://quietcool.com/x.bin"),
             UpgradeDecision::BlockedOemDomain);
}
TEST("classify: mixed-case OEM host → BlockedOemDomain") {
  REQUIRE_EQ(classify_upgrade_url("HTTP://MyQuietCool.COM/x.bin"),
             UpgradeDecision::BlockedOemDomain);
}
TEST("classify: OEM host with port → BlockedOemDomain") {
  REQUIRE_EQ(classify_upgrade_url("http://myquietcool.com:8080/x.bin"),
             UpgradeDecision::BlockedOemDomain);
}
TEST("classify: userinfo before OEM host → BlockedOemDomain") {
  REQUIRE_EQ(classify_upgrade_url("http://user@myquietcool.com/x.bin"),
             UpgradeDecision::BlockedOemDomain);
}
TEST("classify: LAN IP host → Flash") {
  REQUIRE_EQ(classify_upgrade_url("http://10.0.0.28:8000/firmware.ota.bin"),
             UpgradeDecision::Flash);
}
TEST("classify: arbitrary https host → Flash") {
  REQUIRE_EQ(classify_upgrade_url("https://example.com/fw.bin"),
             UpgradeDecision::Flash);
}
TEST("classify: OEM label as prefix of attacker host → Flash") {
  REQUIRE_EQ(classify_upgrade_url("http://myquietcool.com.evil.com/x.bin"),
             UpgradeDecision::Flash);
}
TEST("classify: userinfo spoof (real host non-OEM) → Flash") {
  REQUIRE_EQ(classify_upgrade_url("http://myquietcool.com@evil.com/x.bin"),
             UpgradeDecision::Flash);
}

// ============================================================================
// upgrade_state_string — A=5 GetUpgradeState minimal feedback
// ============================================================================

TEST("upgrade_state_string: idle → Connect_NO") {
  REQUIRE(strcmp(upgrade_state_string(UPGRADE_STATE_IDLE), "Connect_NO") == 0);
}
TEST("upgrade_state_string: downloading → Downloading_Progress") {
  REQUIRE(strcmp(upgrade_state_string(UPGRADE_STATE_DOWNLOADING), "Downloading_Progress") == 0);
}
TEST("upgrade_state_string: fail → Download_Fail") {
  REQUIRE(strcmp(upgrade_state_string(UPGRADE_STATE_FAIL), "Download_Fail") == 0);
}
TEST("upgrade_state_string: unknown → Connect_NO (default)") {
  REQUIRE(strcmp(upgrade_state_string(99), "Connect_NO") == 0);
}

// ============================================================================
// main
// ============================================================================

int main() { return tu::run_all(); }
