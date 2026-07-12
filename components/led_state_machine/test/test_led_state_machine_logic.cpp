// Host-side unit tests for LedStateMachineLogic.
// Pure C++17, no ESPHome includes.

#include "test_utils.h"
#include "../led_state_machine_logic.h"

using qc::LedInput;
using qc::LedPattern;
using qc::LedState;
using qc::LedStateMachineLogic;

namespace qc {
inline const char* pattern_name(LedPattern p) {
  switch (p) {
    case LedPattern::Off:       return "Off";
    case LedPattern::On:        return "On";
    case LedPattern::SlowBlink: return "SlowBlink";
    case LedPattern::FastBlink: return "FastBlink";
    case LedPattern::VeryFast:  return "VeryFast";
  }
  return "??";
}
inline bool operator==(const LedState& a, const LedState& b) {
  return a.led2 == b.led2 && a.led3 == b.led3 && a.led4 == b.led4;
}
inline std::ostream& operator<<(std::ostream& os, LedPattern p) {
  return os << pattern_name(p);
}
inline std::ostream& operator<<(std::ostream& os, const LedState& s) {
  return os << "{led2=" << pattern_name(s.led2)
            << " led3=" << pattern_name(s.led3)
            << " led4=" << pattern_name(s.led4) << "}";
}
}  // namespace qc

// Baseline "everything OK, fan off" — all LEDs Off.
static LedInput baseline() {
  return LedInput{
      /*fan_on=*/false, /*timer_running=*/false,
      /*improv=*/false, /*pair=*/false,
      /*key1_held=*/0, /*key2_held=*/0, /*dual_held=*/0,
      /*dip_invalid=*/false,
  };
}

// ============================================================================
// Fan-state defaults (no button warnings, no BLE activity).
// ============================================================================

TEST("fan off, idle -> all LEDs Off") {
  auto s = LedStateMachineLogic::compute_led_state(baseline());
  REQUIRE_EQ(s, LedState{LedPattern::Off, LedPattern::Off, LedPattern::Off});
}

TEST("fan on, no timer -> LED2 solid On, LED3 solid On, LED4 Off") {
  auto in = baseline();
  in.fan_on = true;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::On, LedPattern::On, LedPattern::Off});
}

TEST("fan on + timer running -> LED2 SlowBlink (countdown indicator), LED3 On") {
  auto in = baseline();
  in.fan_on = true;
  in.timer_running = true;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::SlowBlink, LedPattern::On, LedPattern::Off});
}

// ============================================================================
// BLE status on LED4 (Improv > Pair > idle). Fan state on LED2/3 is unchanged.
// Wi-Fi/HA connectivity is deliberately NOT shown on LEDs — the "fan off ->
// all Off" and "fan on -> LED4 Off" cases above already pin that LED4 ignores
// everything but BLE.
// ============================================================================

TEST("BLE pair mode -> LED4 SlowBlink (OEM parity)") {
  auto in = baseline();
  in.pair_mode_active = true;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s.led4, LedPattern::SlowBlink);
}

TEST("Improv-BLE advertising -> LED4 VeryFast (highest BLE priority)") {
  auto in = baseline();
  in.improv_advertising = true;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::Off, LedPattern::Off, LedPattern::VeryFast});
}

// ============================================================================
// Button-warning overrides (priority 2).
// ============================================================================

TEST("KEY1 held just below warning (2999ms) -> no override yet") {
  auto in = baseline();
  in.key1_held_ms = 2999;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::Off, LedPattern::Off, LedPattern::Off});
}

TEST("KEY1 held to warning threshold (3000ms) -> LED2 FastBlink") {
  auto in = baseline();
  in.key1_held_ms = 3000;
  auto s = LedStateMachineLogic::compute_led_state(in);
  // LED2 is the warning indicator for KEY1 (factory reset).
  // LED3/4 remain in their default state (off/off here).
  REQUIRE_EQ(s.led2, LedPattern::FastBlink);
  REQUIRE_EQ(s.led3, LedPattern::Off);
  REQUIRE_EQ(s.led4, LedPattern::Off);
}

TEST("KEY1 warning preserves LED4 BLE state unless KEY2 also held") {
  auto in = baseline();
  in.key1_held_ms = 3500;
  in.pair_mode_active = true;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s.led2, LedPattern::FastBlink);   // KEY1 warning grabs LED2
  REQUIRE_EQ(s.led4, LedPattern::SlowBlink);   // pair mode still on LED4
}

TEST("KEY2 held to warning threshold -> LED4 FastBlink override") {
  auto in = baseline();
  in.key2_held_ms = 3500;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s.led4, LedPattern::FastBlink);
  // LED2/3 untouched.
  REQUIRE_EQ(s.led2, LedPattern::Off);
  REQUIRE_EQ(s.led3, LedPattern::Off);
}

TEST("KEY2 warning overrides BLE status on LED4") {
  // User intent (warning) should win over BLE status.
  auto in = baseline();
  in.key2_held_ms = 3500;
  in.pair_mode_active = true;  // would normally make LED4 SlowBlink
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s.led4, LedPattern::FastBlink);
}

// ============================================================================
// Commit-threshold override (priority 1) — all 3 LEDs VeryFast.
// ============================================================================

TEST("KEY1 held to commit threshold (5000ms) -> all LEDs VeryFast") {
  auto in = baseline();
  in.key1_held_ms = 5000;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::VeryFast, LedPattern::VeryFast, LedPattern::VeryFast});
}

TEST("KEY2 held to commit threshold -> all LEDs VeryFast") {
  auto in = baseline();
  in.key2_held_ms = 5500;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::VeryFast, LedPattern::VeryFast, LedPattern::VeryFast});
}

TEST("commit-threshold overrides BLE status and fan state") {
  auto in = baseline();
  in.fan_on = true;
  in.timer_running = true;
  in.pair_mode_active = true;
  in.key1_held_ms = 5500;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::VeryFast, LedPattern::VeryFast, LedPattern::VeryFast});
}

// ============================================================================
// DIP-invalid override — OEM behavior: all LEDs solid when DIP misconfigured.
// Priority below button gestures, above fan-state + BLE status.
// ============================================================================

TEST("DIP invalid, no buttons -> all LEDs solid On") {
  auto in = baseline();
  in.dip_invalid = true;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::On, LedPattern::On, LedPattern::On});
}

TEST("DIP invalid overrides fan-on + BLE status") {
  auto in = baseline();
  in.dip_invalid = true;
  in.fan_on = true;
  in.timer_running = true;
  in.pair_mode_active = true;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::On, LedPattern::On, LedPattern::On});
}

TEST("DIP invalid, button at commit threshold -> button gestures still visible") {
  auto in = baseline();
  in.dip_invalid = true;
  in.key1_held_ms = 5000;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::VeryFast, LedPattern::VeryFast, LedPattern::VeryFast});
}

TEST("DIP invalid, button at warning threshold -> per-button warning overrides solid") {
  auto in = baseline();
  in.dip_invalid = true;
  in.key1_held_ms = 3500;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s.led2, LedPattern::FastBlink);
  REQUIRE_EQ(s.led3, LedPattern::On);
  REQUIRE_EQ(s.led4, LedPattern::On);
}

TEST("DIP valid (default) -> baseline unchanged (regression)") {
  auto in = baseline();
  in.dip_invalid = false;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::Off, LedPattern::Off, LedPattern::Off});
}

// ============================================================================
// pattern_is_on — time-modulo math.
// ============================================================================

TEST("pattern_is_on: Off is always false") {
  REQUIRE(!LedStateMachineLogic::pattern_is_on(LedPattern::Off, 0));
  REQUIRE(!LedStateMachineLogic::pattern_is_on(LedPattern::Off, 12345));
}

TEST("pattern_is_on: On is always true") {
  REQUIRE(LedStateMachineLogic::pattern_is_on(LedPattern::On, 0));
  REQUIRE(LedStateMachineLogic::pattern_is_on(LedPattern::On, 999999));
}

TEST("pattern_is_on: SlowBlink toggles every 500ms (1Hz, 50% duty)") {
  // SlowBlink period 1000ms, half-on / half-off.
  REQUIRE(!LedStateMachineLogic::pattern_is_on(LedPattern::SlowBlink, 0));
  REQUIRE(!LedStateMachineLogic::pattern_is_on(LedPattern::SlowBlink, 499));
  REQUIRE( LedStateMachineLogic::pattern_is_on(LedPattern::SlowBlink, 500));
  REQUIRE( LedStateMachineLogic::pattern_is_on(LedPattern::SlowBlink, 999));
  REQUIRE(!LedStateMachineLogic::pattern_is_on(LedPattern::SlowBlink, 1000));
}

TEST("pattern_is_on: FastBlink toggles every 100ms (5Hz)") {
  REQUIRE(!LedStateMachineLogic::pattern_is_on(LedPattern::FastBlink, 0));
  REQUIRE(!LedStateMachineLogic::pattern_is_on(LedPattern::FastBlink, 99));
  REQUIRE( LedStateMachineLogic::pattern_is_on(LedPattern::FastBlink, 100));
  REQUIRE( LedStateMachineLogic::pattern_is_on(LedPattern::FastBlink, 199));
  REQUIRE(!LedStateMachineLogic::pattern_is_on(LedPattern::FastBlink, 200));
}

TEST("pattern_is_on: VeryFast toggles every 50ms (10Hz)") {
  REQUIRE(!LedStateMachineLogic::pattern_is_on(LedPattern::VeryFast, 0));
  REQUIRE(!LedStateMachineLogic::pattern_is_on(LedPattern::VeryFast, 49));
  REQUIRE( LedStateMachineLogic::pattern_is_on(LedPattern::VeryFast, 50));
  REQUIRE(!LedStateMachineLogic::pattern_is_on(LedPattern::VeryFast, 100));
}

// ============================================================================
// Dual-button hold (STOCK FIRMWARE RESTORE gesture).
// Priority above single-button COMMIT: when both buttons are simultaneously
// held, the LED needs to communicate "you're in the dual gesture, not the
// single one — keep holding to 10s for stock restore (or release to abort)".
//
// Pattern map:
//   dual_held_ms < DUAL_WARNING_MS (5s)   -> fall through to single-button logic
//   dual_held_ms in [5s, 10s)             -> all-LED SlowBlink (intermediate)
//   dual_held_ms >= DUAL_COMMIT_MS (10s)  -> all-LED VeryFast (about to fire)
// ============================================================================

TEST("dual_held below warning (4999ms) -> no dual override (falls through to single logic)") {
  auto in = baseline();
  in.key1_held_ms = 4999;
  in.key2_held_ms = 4999;
  in.dual_held_ms = 4999;
  auto s = LedStateMachineLogic::compute_led_state(in);
  // Below dual-warning: both single keys at >3s warning, neither at 5s commit
  // -> KEY1 grabs LED2 FastBlink, KEY2 grabs LED4 FastBlink.
  REQUIRE_EQ(s.led2, LedPattern::FastBlink);
  REQUIRE_EQ(s.led4, LedPattern::FastBlink);
}

TEST("dual_held at warning (5000ms) -> all LEDs SlowBlink (intermediate)") {
  auto in = baseline();
  in.key1_held_ms = 5000;
  in.key2_held_ms = 5000;
  in.dual_held_ms = 5000;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::SlowBlink, LedPattern::SlowBlink, LedPattern::SlowBlink});
}

TEST("dual_held intermediate (7500ms) -> all LEDs SlowBlink (still climbing)") {
  auto in = baseline();
  in.key1_held_ms = 7500;
  in.key2_held_ms = 7500;
  in.dual_held_ms = 7500;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::SlowBlink, LedPattern::SlowBlink, LedPattern::SlowBlink});
}

TEST("dual_held just below commit (9999ms) -> still SlowBlink") {
  auto in = baseline();
  in.dual_held_ms = 9999;
  in.key1_held_ms = 9999;
  in.key2_held_ms = 9999;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::SlowBlink, LedPattern::SlowBlink, LedPattern::SlowBlink});
}

TEST("dual_held at commit threshold (10000ms) -> all LEDs VeryFast (about to fire)") {
  auto in = baseline();
  in.dual_held_ms = 10000;
  in.key1_held_ms = 10000;
  in.key2_held_ms = 10000;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::VeryFast, LedPattern::VeryFast, LedPattern::VeryFast});
}

TEST("dual_held beyond commit (15000ms) -> still VeryFast") {
  auto in = baseline();
  in.dual_held_ms = 15000;
  in.key1_held_ms = 15000;
  in.key2_held_ms = 15000;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::VeryFast, LedPattern::VeryFast, LedPattern::VeryFast});
}

TEST("dual_held intermediate overrides single-button COMMIT (5500ms each)") {
  // Without dual logic, both single keys at 5500ms would fire single-COMMIT
  // (all-LED VeryFast). With dual logic, the SlowBlink intermediate wins:
  // distinguishes "intentional dual gesture" from "accidentally held both
  // past single-commit threshold".
  auto in = baseline();
  in.key1_held_ms = 5500;
  in.key2_held_ms = 5500;
  in.dual_held_ms = 5500;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::SlowBlink, LedPattern::SlowBlink, LedPattern::SlowBlink});
}

TEST("dual_held intermediate overrides fan-state + BLE status") {
  auto in = baseline();
  in.fan_on = true;
  in.timer_running = true;
  in.pair_mode_active = true;
  in.dual_held_ms = 6000;
  in.key1_held_ms = 6000;
  in.key2_held_ms = 6000;
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s, LedState{LedPattern::SlowBlink, LedPattern::SlowBlink, LedPattern::SlowBlink});
}

TEST("dual_held=0 with only single-button hold: single-button logic unchanged") {
  auto in = baseline();
  in.key1_held_ms = 6000;  // single COMMIT
  in.dual_held_ms = 0;     // not in dual
  auto s = LedStateMachineLogic::compute_led_state(in);
  // Existing behavior: single-button commit -> all-LED VeryFast.
  REQUIRE_EQ(s, LedState{LedPattern::VeryFast, LedPattern::VeryFast, LedPattern::VeryFast});
}

TEST("dual_held=0 + both single keys warning: existing per-button warning unchanged") {
  auto in = baseline();
  in.key1_held_ms = 3500;
  in.key2_held_ms = 3500;
  in.dual_held_ms = 0;  // dual not active (e.g. presses staggered, never overlapped enough)
  auto s = LedStateMachineLogic::compute_led_state(in);
  REQUIRE_EQ(s.led2, LedPattern::FastBlink);
  REQUIRE_EQ(s.led4, LedPattern::FastBlink);
}

TEST("DUAL thresholds aliased from DualGestureTracker (5000/10000)") {
  REQUIRE_EQ(LedStateMachineLogic::DUAL_WARNING_MS, qc::DualGestureTracker::WARNING_MS);
  REQUIRE_EQ(LedStateMachineLogic::DUAL_COMMIT_MS, qc::DualGestureTracker::COMMIT_MS);
  REQUIRE_EQ(LedStateMachineLogic::DUAL_COMMIT_MS, 10000u);
}

int main() { return tu::run_all(); }
