// Host-side unit tests for OemBleCompatLogic — pair-state machine, gate
// checks, NVS flush gating, BLE link recovery, field conversions, and framing.
//
// Compile + run:
//   g++ -std=c++17 -I.. test_oem_ble_compat_logic.cpp -o test_oem_ble_compat_logic.exe
//   ./test_oem_ble_compat_logic.exe

#include "test_utils.h"
#include "../oem_preset_storage.h"

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

TEST("GetWorkState: authenticated Smart Mode reports local control while idle") {
  REQUIRE_EQ(check_gate(1, PairState::Auth, false), GateResult::Allowed);
  REQUIRE_EQ(get_work_state_response(mode_to_oem(false, true, false), "OFF", true, 720, 45),
             std::string(R"({"A":1,"M":"TH","R":"OFF","S":"OK","T":720,"H":45,"C":0})"));
}

TEST("GetWorkState: running fan still reports local control") {
  REQUIRE_EQ(get_work_state_response(mode_to_oem(true, true, false), "HIGH", true, 950, 60),
             std::string(R"({"A":1,"M":"TH","R":"HIGH","S":"OK","T":950,"H":60,"C":0})"));
}

TEST("GetWorkState: sensor fault does not imply AirControl") {
  REQUIRE_EQ(get_work_state_response("Idle", "OFF", false, 0, 0),
             std::string(R"({"A":1,"M":"Idle","R":"OFF","S":"NG","T":0,"H":0,"C":0})"));
}

TEST("GetVersion exactly matches the OEM production channel") {
  REQUIRE_EQ(std::string(get_version_response()),
             std::string(R"({"A":3,"V":"IT-BLT-ATTICFAN_V4.4","P":100,"D":"2025.11.18","M":"online","H":"A"})"));
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
// 4c. BLE link-health recovery
// ============================================================================

TEST("BLE stale snapshot requires complete all-stale coverage") {
  REQUIRE(!all_tracked_links_stale(0, 0, 0));
  REQUIRE(!all_tracked_links_stale(2, 1, 1));
  REQUIRE(!all_tracked_links_stale(2, 2, 1));
  REQUIRE(all_tracked_links_stale(2, 2, 2));
}

TEST("BLE link monitor waits one interval before probing") {
  BleLinkHealthMonitor monitor;
  REQUIRE(!monitor.probe_due(true, false, 1, 1, 1000));
  REQUIRE(!monitor.probe_due(
      true, false, 1, 1,
      1000 + BleLinkHealthMonitor::PROBE_INTERVAL_MS - 1));
  REQUIRE(monitor.probe_due(
      true, false, 1, 1,
      1000 + BleLinkHealthMonitor::PROBE_INTERVAL_MS));
}

TEST("BLE link monitor never recycles a healthy idle client") {
  BleLinkHealthMonitor monitor;
  REQUIRE(!monitor.probe_due(true, false, 1, 1, 1000));
  constexpr uint32_t FIRST_PROBE =
      1000 + BleLinkHealthMonitor::PROBE_INTERVAL_MS;
  const bool all_stale = all_tracked_links_stale(1, 1, 0);
  REQUIRE(!all_stale);
  REQUIRE(monitor.probe_due(true, false, 1, 1, FIRST_PROBE));
  REQUIRE(!monitor.record_probe(FIRST_PROBE, all_stale));
  REQUIRE(monitor.probe_due(
      true, false, 1, 1,
      FIRST_PROBE + BleLinkHealthMonitor::PROBE_INTERVAL_MS));
  REQUIRE(!monitor.record_probe(
      FIRST_PROBE + BleLinkHealthMonitor::PROBE_INTERVAL_MS, all_stale));
}

TEST("BLE link monitor requires two all-stale probes") {
  BleLinkHealthMonitor monitor;
  REQUIRE(!monitor.probe_due(true, false, 2, 2, 1000));
  constexpr uint32_t FIRST_PROBE =
      1000 + BleLinkHealthMonitor::PROBE_INTERVAL_MS;
  const bool all_stale = all_tracked_links_stale(2, 2, 2);
  REQUIRE(all_stale);
  REQUIRE(monitor.probe_due(true, false, 2, 2, FIRST_PROBE));
  REQUIRE(!monitor.record_probe(FIRST_PROBE, all_stale));
  constexpr uint32_t SECOND_PROBE =
      FIRST_PROBE + BleLinkHealthMonitor::PROBE_INTERVAL_MS;
  REQUIRE(monitor.probe_due(true, false, 2, 2, SECOND_PROBE));
  REQUIRE(monitor.record_probe(SECOND_PROBE, all_stale));
}

TEST("BLE link monitor preserves live peers when another peer is stale") {
  BleLinkHealthMonitor monitor;
  REQUIRE(!monitor.probe_due(true, false, 2, 2, 1000));
  constexpr uint32_t FIRST_PROBE =
      1000 + BleLinkHealthMonitor::PROBE_INTERVAL_MS;
  const bool all_stale = all_tracked_links_stale(2, 2, 2);
  const bool mixed_live_and_stale = all_tracked_links_stale(2, 2, 1);
  REQUIRE(all_stale);
  REQUIRE(!mixed_live_and_stale);
  REQUIRE(monitor.probe_due(true, false, 2, 2, FIRST_PROBE));
  REQUIRE(!monitor.record_probe(FIRST_PROBE, all_stale));
  constexpr uint32_t MIXED_PROBE =
      FIRST_PROBE + BleLinkHealthMonitor::PROBE_INTERVAL_MS;
  REQUIRE(monitor.probe_due(true, false, 2, 2, MIXED_PROBE));
  REQUIRE(!monitor.record_probe(MIXED_PROBE, mixed_live_and_stale));
  constexpr uint32_t NEXT_STALE_PROBE =
      MIXED_PROBE + BleLinkHealthMonitor::PROBE_INTERVAL_MS;
  REQUIRE(monitor.probe_due(true, false, 2, 2, NEXT_STALE_PROBE));
  REQUIRE(!monitor.record_probe(NEXT_STALE_PROBE, all_stale));
}

TEST("BLE link monitor requires complete conn-id tracking") {
  BleLinkHealthMonitor monitor;
  REQUIRE(!monitor.probe_due(true, false, 2, 1, 1000));
  REQUIRE(!monitor.probe_due(
      true, false, 2, 1,
      1000 + BleLinkHealthMonitor::PROBE_INTERVAL_MS));
  REQUIRE(!monitor.probe_due(true, false, 2, 2, 10000));
  REQUIRE(monitor.probe_due(
      true, false, 2, 2,
      10000 + BleLinkHealthMonitor::PROBE_INTERVAL_MS));
}

TEST("BLE link monitor resets after all clients disconnect") {
  BleLinkHealthMonitor monitor;
  REQUIRE(!monitor.probe_due(true, false, 1, 1, 1000));
  constexpr uint32_t FIRST_PROBE =
      1000 + BleLinkHealthMonitor::PROBE_INTERVAL_MS;
  REQUIRE(monitor.probe_due(true, false, 1, 1, FIRST_PROBE));
  REQUIRE(!monitor.record_probe(
      FIRST_PROBE, all_tracked_links_stale(1, 1, 1)));
  REQUIRE(!monitor.probe_due(true, false, 0, 0, FIRST_PROBE + 1));
  REQUIRE(!monitor.probe_due(true, false, 1, 1, FIRST_PROBE + 2));
  constexpr uint32_t RECONNECTED_PROBE =
      FIRST_PROBE + 2 + BleLinkHealthMonitor::PROBE_INTERVAL_MS;
  REQUIRE(monitor.probe_due(true, false, 1, 1, RECONNECTED_PROBE));
  REQUIRE(!monitor.record_probe(
      RECONNECTED_PROBE, all_tracked_links_stale(1, 1, 1)));
}

TEST("BLE link monitor is suspended during OTA and Improv handoff") {
  BleLinkHealthMonitor monitor;
  REQUIRE(!monitor.probe_due(true, false, 1, 1, 1000));
  REQUIRE(!monitor.probe_due(
      true, true, 1, 1,
      1000 + BleLinkHealthMonitor::PROBE_INTERVAL_MS));
  REQUIRE(!monitor.probe_due(true, false, 1, 1, 10000));
  REQUIRE(!monitor.probe_due(
      false, false, 1, 1,
      10000 + BleLinkHealthMonitor::PROBE_INTERVAL_MS));
  REQUIRE(!monitor.probe_due(true, false, 1, 1, 20000));
}

TEST("BLE link probe interval handles millis wraparound") {
  BleLinkHealthMonitor monitor;
  constexpr uint32_t START = 0xFFFFFF00U;
  REQUIRE(!monitor.probe_due(true, false, 1, 1, START));
  REQUIRE(monitor.probe_due(
      true, false, 1, 1,
      START + BleLinkHealthMonitor::PROBE_INTERVAL_MS));
}

TEST("stall monitor ignores a condition that clears within the limit") {
  StallMonitor monitor(100);
  REQUIRE(!monitor.update(true, 1000));
  REQUIRE(!monitor.update(true, 1099));
  REQUIRE(!monitor.stalled());
  REQUIRE(!monitor.update(false, 1100));
  REQUIRE(!monitor.update(true, 1150));
  REQUIRE(!monitor.update(true, 1249));
  REQUIRE(!monitor.stalled());
}

TEST("stall monitor paces recovery attempts while stalled") {
  StallMonitor monitor(100);
  REQUIRE(!monitor.update(true, 1000));
  REQUIRE(monitor.update(true, 1100));
  REQUIRE(monitor.stalled());
  REQUIRE(!monitor.update(true, 1101));
  REQUIRE(!monitor.update(true, 1199));
  REQUIRE(monitor.stalled());
  REQUIRE(monitor.update(true, 1200));
  REQUIRE(!monitor.update(true, 1250));
}

TEST("stall monitor starts a new episode after the condition clears") {
  StallMonitor monitor(100);
  REQUIRE(!monitor.update(true, 1000));
  REQUIRE(monitor.update(true, 1100));
  REQUIRE(!monitor.update(false, 1150));
  REQUIRE(!monitor.stalled());
  REQUIRE(!monitor.update(true, 1160));
  REQUIRE(!monitor.update(true, 1259));
  REQUIRE(monitor.update(true, 1260));
}

TEST("stall monitor accepts millis zero and wraparound") {
  StallMonitor from_zero(100);
  REQUIRE(!from_zero.update(true, 0));
  REQUIRE(!from_zero.update(true, 99));
  REQUIRE(from_zero.update(true, 100));

  StallMonitor wrapping(100);
  constexpr uint32_t START = 0xFFFFFFC0U;
  REQUIRE(!wrapping.update(true, START));
  REQUIRE(!wrapping.update(true, START + 99));
  REQUIRE(wrapping.update(true, START + 100));
  REQUIRE(!wrapping.update(true, START + 199));
  REQUIRE(wrapping.update(true, START + 200));
}

// ============================================================================
// 4d. NVS flush gating
// ============================================================================

TEST("hx_list flush timer accepts millis zero as a real start time") {
  HxFlushTimer timer;
  constexpr uint32_t DELAY = 50;

  REQUIRE_EQ(timer.due(DELAY, DELAY, false), false);
  timer.start_if_needed(0);
  REQUIRE_EQ(timer.due(DELAY - 1, DELAY, false), false);
  timer.start_if_needed(DELAY - 1);
  REQUIRE_EQ(timer.due(DELAY, DELAY, true), false);
  REQUIRE_EQ(timer.due(DELAY, DELAY, false), true);
  timer.reset();
  REQUIRE_EQ(timer.due(2 * DELAY, DELAY, false), false);
}

TEST("hx_list flush timer handles millis wraparound") {
  HxFlushTimer timer;
  constexpr uint32_t START = 0xFFFFFFF0U;
  constexpr uint32_t DELAY = 32;
  timer.start_if_needed(START);
  REQUIRE_EQ(timer.due(START + DELAY, DELAY, false), true);
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

TEST("frame: braces inside a JSON string do not end the frame") {
  FrameAssembler fa;
  std::string json = R"({"A":16,"N":"my{fan}"})";
  fa.feed(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  REQUIRE(fa.has_complete());
  auto msg = fa.take();
  // The whole object, not a fragment cut at the '}' inside the string.
  REQUIRE_EQ(msg.size(), json.size());
  REQUIRE(fa.buf.empty());
}

TEST("frame: a '}' in a password does not truncate or poison the buffer") {
  FrameAssembler fa;
  // SetRouter carries arbitrary user text; validate_password permits braces.
  std::string json = R"({"A":11,"S":"net","P":"pa}ss{word"})";
  fa.feed(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  REQUIRE(fa.has_complete());
  REQUIRE_EQ(fa.take().size(), json.size());
  REQUIRE(fa.buf.empty());
  // The next command still frames cleanly.
  std::string next = R"({"A":1})";
  fa.feed(reinterpret_cast<const uint8_t *>(next.data()), next.size());
  REQUIRE(fa.has_complete());
  REQUIRE_EQ(fa.take().size(), next.size());
}

TEST("frame: escaped quote inside a JSON string is not a string terminator") {
  FrameAssembler fa;
  std::string json = R"({"A":16,"N":"say \"hi\" }now"})";
  fa.feed(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  REQUIRE(fa.has_complete());
  REQUIRE_EQ(fa.take().size(), json.size());
}

TEST("frame: stray leading '}' does not wedge the buffer") {
  FrameAssembler fa;
  std::string junk = "}";
  fa.feed(reinterpret_cast<const uint8_t *>(junk.data()), junk.size());
  REQUIRE(!fa.has_complete());
  std::string json = R"({"A":1})";
  fa.feed(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  REQUIRE(fa.has_complete());
  // Leading junk rides along and the parse fails, but the buffer is drained
  // rather than left holding an unmatchable prefix forever.
  fa.take();
  REQUIRE(fa.buf.empty());
}

TEST("frame: binary GetRecordData with day-index 0x7D frames as 4 bytes") {
  FrameAssembler fa;
  // 0x7D is '}' — scanning for a terminator would cut this one byte short.
  uint8_t data[] = {'{', 0x1C, 0x7D, '}'};
  fa.feed(data, 4);
  REQUIRE(fa.has_complete());
  auto msg = fa.take();
  REQUIRE_EQ(msg.size(), size_t(4));
  REQUIRE_EQ(msg[2], uint8_t(0x7D));
  REQUIRE(fa.buf.empty());
}

TEST("frame: binary command split across writes completes only when whole") {
  FrameAssembler fa;
  uint8_t first[] = {'{', 0x1D, '1', '7', '4', '8'};
  fa.feed(first, 6);
  REQUIRE(!fa.has_complete());
  uint8_t rest[] = {'2', '8', '0', '0', '0', '0', '}'};
  fa.feed(rest, 7);
  REQUIRE(fa.has_complete());
  REQUIRE_EQ(fa.take().size(), size_t(13));
}

TEST("frame: binary command followed by a queued JSON command") {
  FrameAssembler fa;
  uint8_t bin[] = {'{', 0x1C, 0x05, '}'};
  fa.feed(bin, 4);
  std::string json = R"({"A":1})";
  fa.feed(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  // The binary frame must not swallow the JSON that followed it.
  REQUIRE(fa.has_complete());
  REQUIRE_EQ(fa.take().size(), size_t(4));
  REQUIRE(fa.has_complete());
  REQUIRE_EQ(fa.take().size(), json.size());
  REQUIRE(fa.buf.empty());
}

TEST("frame: buffer is capped and dropped rather than grown without bound") {
  FrameAssembler fa;
  std::vector<uint8_t> junk(200, 'x');  // no braces — never completes
  size_t accepted = 0;
  for (int i = 0; i < 20; i++) {
    if (fa.feed(junk.data(), junk.size())) accepted++;
    REQUIRE(fa.buf.size() <= FrameAssembler::MAX_BUFFERED);
  }
  REQUIRE(accepted < 20);       // at least one write was refused
  REQUIRE(!fa.has_complete());
}

TEST("frame: a normal message still fits well under the cap") {
  FrameAssembler fa;
  // Worst realistic request: SetPresets with 4 fully-populated presets.
  std::string big = R"({"A":20,"P":[)";
  for (int i = 0; i < 4; i++) {
    if (i) big += ',';
    big += R"([")" + std::string(50, 'n') + R"(",120,100,80,90,70,"MEDIUM"])";
  }
  big += "]}";
  REQUIRE(big.size() < FrameAssembler::MAX_BUFFERED);
  REQUIRE(fa.feed(reinterpret_cast<const uint8_t *>(big.data()), big.size()));
  REQUIRE(fa.has_complete());
  REQUIRE_EQ(fa.take().size(), big.size());
}

TEST("frame: recovers on the next message after an overflow drop") {
  FrameAssembler fa;
  std::vector<uint8_t> junk(FrameAssembler::MAX_BUFFERED + 1, 'x');
  REQUIRE(!fa.feed(junk.data(), junk.size()));
  REQUIRE(fa.buf.empty());
  std::string json = R"({"A":1})";
  REQUIRE(fa.feed(reinterpret_cast<const uint8_t *>(json.data()), json.size()));
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

// ============================================================================
// 7b. Binary response framing (A=28 GetRecordData, A=29 SynchronizeTime)
// ============================================================================
//
// The OEM app's receive assembler only treats a "QQ"-prefixed buffer as a
// complete message once it ends with '}'. A binary response missing that
// terminator is buffered forever and the app's request queue stalls.

TEST("binary frame: QQ prefix, '{' + type envelope, '}' terminator") {
  auto f = binary_frame(0x1D, {0x01});
  REQUIRE_EQ(f.size(), size_t(6));
  REQUIRE_EQ(f[0], uint8_t('Q'));
  REQUIRE_EQ(f[1], uint8_t('Q'));
  REQUIRE_EQ(f[2], uint8_t('{'));
  REQUIRE_EQ(f[3], uint8_t(0x1D));  // app reads the type at index 3
  REQUIRE_EQ(f[4], uint8_t(0x01));  // SynchronizeTime success flag at index 4
  REQUIRE_EQ(f[5], uint8_t('}'));
}

TEST("binary frame: always terminated so the app can frame it") {
  REQUIRE_EQ(binary_frame(0x1D, {0x01}).back(), uint8_t('}'));
  REQUIRE_EQ(binary_frame(0x1C, std::vector<uint8_t>(75, 0xFF)).back(), uint8_t('}'));
  REQUIRE_EQ(binary_frame(0x1C, {}).back(), uint8_t('}'));
}

TEST("binary frame: GetRecordData payload survives as 75 values (25 groups of 3)") {
  auto f = binary_frame(0x1C, std::vector<uint8_t>(75, 0xFF));
  REQUIRE_EQ(f.size(), size_t(80));
  REQUIRE_EQ(f[3], uint8_t(0x1C));
  // The app emits index 4 then indices 5..size-2, and groups the result in
  // threes; a count not divisible by 3 makes its last group short and throws.
  size_t emitted = (f.size() - 1) - 4;
  REQUIRE_EQ(emitted, size_t(75));
  REQUIRE_EQ(emitted % 3, size_t(0));
}

// The app reads the type byte off the *first* notify packet
// (dataArray[0][3]) and tests completeness on the *accumulated* buffer
// (endsWith "}"). Chunking must not break either.
TEST("binary frame: chunked at default MTU, type stays in chunk 0, '}' ends the last") {
  auto f = binary_frame(0x1C, std::vector<uint8_t>(75, 0xFF));
  auto chunks = chunk_response(f, 23);  // 20-byte chunks
  REQUIRE_EQ(chunks.size(), size_t(4));
  REQUIRE(chunks[0].size() > 3);
  REQUIRE_EQ(chunks[0][3], uint8_t(0x1C));
  REQUIRE_EQ(chunks.back().back(), uint8_t('}'));
  // Reassembly must reproduce the frame byte-for-byte.
  std::vector<uint8_t> rejoined;
  for (auto &c : chunks) rejoined.insert(rejoined.end(), c.begin(), c.end());
  REQUIRE(rejoined == f);
}

TEST("binary frame: SynchronizeTime fits one notify packet") {
  auto chunks = chunk_response(binary_frame(0x1D, {0x01}), 23);
  REQUIRE_EQ(chunks.size(), size_t(1));
  REQUIRE_EQ(chunks[0][3], uint8_t(0x1D));
  REQUIRE_EQ(chunks[0].back(), uint8_t('}'));
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

// Response-side safety for user text (fan/preset names, SSID). This replaced
// validate_fan_name, which was never wired to a call site and so protected
// nothing; escaping also covers the HA-entity and NVS-import paths a validator
// on the BLE handler would have missed.
TEST("json_escape: passes ordinary text through unchanged") {
  REQUIRE_EQ(json_escape("Attic Fan 2"), std::string("Attic Fan 2"));
  REQUIRE_EQ(json_escape(""), std::string(""));
}

TEST("json_escape: escapes quote and backslash") {
  REQUIRE_EQ(json_escape(R"(my "big" fan)"), std::string(R"(my \"big\" fan)"));
  REQUIRE_EQ(json_escape(R"(a\b)"), std::string(R"(a\\b)"));
}

TEST("json_escape: drops braces (OEM app frames on a brace, quoting ignored)") {
  REQUIRE_EQ(json_escape("my{fan}"), std::string("myfan"));
  REQUIRE_EQ(json_escape("}"), std::string(""));
}

TEST("json_escape: escapes newline/tab, drops other control characters") {
  REQUIRE_EQ(json_escape("a\nb\tc"), std::string("a\\nb\\tc"));
  REQUIRE_EQ(json_escape(std::string("a\x01\x1f") + "b"), std::string("ab"));
}

TEST("json_escape: an escaped name still frames as one message") {
  // The whole point: the response must survive the app's assembler.
  auto body = R"({"A":17,"N":")" + json_escape(R"(fan"}evil)") + R"("})";
  FrameAssembler fa;
  fa.feed(reinterpret_cast<const uint8_t *>(body.data()), body.size());
  REQUIRE(fa.has_complete());
  REQUIRE_EQ(fa.take().size(), body.size());
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
// Preset storage and pairing persistence
// ============================================================================

TEST("preset banks match OEM one/two/three-speed wiring and every slot key") {
  const char *prefixes[] = {"Med", "High", "Low"};
  const char chars[] = {'m', 'h', 'l'};
  const char *counts[] = {"medsize1", "highsize1", "lowsize1"};
  for (uint8_t dip = 1; dip <= 3; ++dip) {
    const auto *keys = nvs_preset_keys_for_dip(dip);
    REQUIRE(keys != nullptr);
    REQUIRE_EQ(std::string(keys->value_prefix), std::string(prefixes[dip - 1]));
    REQUIRE_EQ(std::string(keys->tag_key), std::string("Presets") + prefixes[dip - 1]);
    REQUIRE_EQ(std::string(keys->count_key), std::string(counts[dip - 1]));
    for (int slot = 0; slot < 4; ++slot) {
      char key[20];
      keys->format_name_key(slot, key, sizeof(key));
      REQUIRE_EQ(std::string(key), std::string("testname") + chars[dip - 1] +
                 std::to_string(slot + 1));
      for (int value = 0; value < 6; ++value) {
        keys->format_value_key(slot, value, key, sizeof(key));
        REQUIRE_EQ(std::string(key), std::string(prefixes[dip - 1]) +
                   std::to_string((slot + 1) * 10 + value));
      }
    }
  }
  REQUIRE(nvs_preset_keys_for_dip(0) == nullptr);
  REQUIRE(nvs_preset_keys_for_dip(4) == nullptr);
  REQUIRE(nvs_preset_keys_for_dip(255) == nullptr);
}

TEST("first installs and migrated installs import the correct OEM bank") {
  for (uint8_t dip = 0; dip <= 4; ++dip) {
    REQUIRE_EQ(preset_import_dip(dip, false, false), dip);
    REQUIRE_EQ(preset_import_dip(dip, false, true), dip);
    REQUIRE_EQ(preset_import_dip(dip, true, true), dip);
  }
}

TEST("legacy installs read the reversed source until the migration commits") {
  REQUIRE_EQ(preset_import_dip(1, true, false), uint8_t(1));
  REQUIRE_EQ(preset_import_dip(2, true, false), uint8_t(3));
  REQUIRE_EQ(preset_import_dip(3, true, false), uint8_t(2));
  REQUIRE_EQ(preset_import_dip(0, true, false), uint8_t(0));
  REQUIRE_EQ(preset_import_dip(4, true, false), uint8_t(4));
  // Latest legacy hx_list data is copied to the corrected bank, not swapped
  // back again after reboot with the marker present.
  for (uint8_t dip : {uint8_t(2), uint8_t(3)}) {
    const auto *source = nvs_preset_keys_for_dip(preset_import_dip(dip, true, false));
    const auto *target = nvs_preset_keys_for_dip(preset_import_dip(dip, true, true));
    REQUIRE(std::string(source->tag_key) != target->tag_key);
    REQUIRE(target == nvs_preset_keys_for_dip(dip));
  }
}

TEST("pair persistence checks each write and commit, stopping at any failure") {
  for (int fail_at = 0; fail_at <= 4; ++fail_at) {
    std::vector<std::string> calls;
    auto record = [&](const std::string &call) {
      calls.push_back(call);
      return static_cast<int>(calls.size()) != fail_at;
    };
    const bool ok = persist_pair_id("new-id", 2,
        [&](const char *key, const char *value) { return record(std::string(key) + "=" + value); },
        [&](const char *key, uint8_t value) { return record(std::string(key) + "=" + std::to_string(value)); },
        [&]() { return record("commit"); });
    REQUIRE_EQ(ok, fail_at == 0);
    REQUIRE_EQ(calls.size(), size_t(fail_at == 0 ? 4 : fail_at));
    const std::vector<std::string> expected{"Phone3=new-id", "pair_num=3", "flag_PhoneID=65", "commit"};
    for (size_t i = 0; i < calls.size(); ++i) REQUIRE_EQ(calls[i], expected[i]);
  }
}

using StoredPresetTest = Preset;
using PresetBankTest = PresetStorage;

// Match production storage so passing a packed value by reference fails here.
static_assert(alignof(StoredPresetTest) == 1 && alignof(PresetBankTest) == 1);

TEST("preset import stages all fields and preserves destination on every read failure") {
  for (int fail_at = 0; fail_at <= 28; ++fail_at) {
    PresetBankTest bank{};
    bank.count = 1;
    std::strcpy(bank.presets[0].name, "cached");
    bank.presets[0].values[0] = 123;
    int reads = 0;
    const bool ok = read_preset_entries(bank, 4,
        [&](int slot, char *name, size_t capacity) {
          std::snprintf(name, capacity, "Preset%d", slot);
          return ++reads != fail_at;
        },
        [&](int slot, int index, int16_t &value) {
          value = slot * 10 + index;
          return ++reads != fail_at;
        });
    REQUIRE_EQ(ok, fail_at == 0);
    if (ok) {
      REQUIRE_EQ(bank.count, uint8_t(4));
      REQUIRE_EQ(std::string(bank.presets[3].name), std::string("Preset3"));
      REQUIRE_EQ(bank.presets[3].values[5], int16_t(35));
    } else {
      REQUIRE_EQ(reads, fail_at);
      REQUIRE_EQ(bank.count, uint8_t(1));
      REQUIRE_EQ(std::string(bank.presets[0].name), std::string("cached"));
      REQUIRE_EQ(bank.presets[0].values[0], int16_t(123));
    }
  }
}

TEST("preset import respects an empty bank and rejects out-of-range counts") {
  for (uint8_t count : {uint8_t(0), uint8_t(5), uint8_t(255)}) {
    PresetBankTest bank{};
    bank.count = 1;
    int reads = 0;
    const bool ok = read_preset_entries(bank, count,
        [&](int, char *, size_t) { ++reads; return true; },
        [&](int, int, int16_t &) { ++reads; return true; });
    REQUIRE_EQ(ok, count == 0);
    REQUIRE_EQ(reads, 0);
    REQUIRE_EQ(bank.count, uint8_t(count == 0 ? 0 : 1));
  }
}

TEST("pair persistence accepts the last slot and never writes beyond slot 50") {
  for (uint8_t count : {uint8_t(0), uint8_t(49), uint8_t(50), uint8_t(255)}) {
    int calls = 0;
    const bool ok = persist_pair_id("id", count,
        [&](const char *key, const char *) {
          ++calls;
          REQUIRE_EQ(std::string(key), "Phone" + std::to_string(count + 1));
          return true;
        },
        [&](const char *, uint8_t) { ++calls; return true; },
        [&]() { ++calls; return true; });
    REQUIRE_EQ(ok, count < 50);
    REQUIRE_EQ(calls, count < 50 ? 4 : 0);
  }
}

TEST("GetUpgradeState emits numeric zero in all supported states") {
  REQUIRE_EQ(get_upgrade_state_response(UPGRADE_STATE_IDLE),
             std::string(R"({"A":5,"S":"Connect_NO","P":0})"));
  REQUIRE_EQ(get_upgrade_state_response(UPGRADE_STATE_DOWNLOADING),
             std::string(R"({"A":5,"S":"Downloading_Progress","P":0})"));
  REQUIRE_EQ(get_upgrade_state_response(UPGRADE_STATE_FAIL),
             std::string(R"({"A":5,"S":"Download_Fail","P":0})"));
}

int main() { return tu::run_all(); }
