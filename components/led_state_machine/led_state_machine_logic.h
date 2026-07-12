// LedStateMachineLogic — pure decision function mapping world state to
// per-LED blink patterns. NO ESPHome includes — testable on host.
//
// Priority (highest to lowest):
//   1. Dual-button "committing"     (>=10s both held)        -> all-LED VeryFast (about to fire STOCK RESTORE)
//   2. Dual-button "intermediate"   (5-10s both held)        -> all-LED SlowBlink (distinct from single-commit)
//   3. Single-button "committing"   (>=5s key held)          -> all-LED VeryFast (factory-reset / safe-mode)
//   4. Single-button "warning"      (3-5s key held)          -> per-button LED FastBlink
//   5. DIP invalid                                           -> all LEDs solid On (OEM parity)
//   6. BLE status (Improv / Pair)                            -> LED4 pattern overrides "off"
//   7. Fan state (default)                                   -> LED2/3 reflect on/off/timer
//
// The dual-button gesture (stock-firmware restore) deliberately shows the
// SlowBlink intermediate from 5s onward to communicate "keep holding — you're
// past single-commit but haven't reached dual-commit yet". The single-commit
// pattern (VeryFast) only fires at 10s for the dual gesture, signaling the
// release-now-to-fire moment. Distinct LED feedback prevents user confusion
// between "about to factory-reset" (5s single) and "about to stock-restore"
// (10s dual).

#pragma once

#include <cstdint>

#include "../fan_controller/dual_gesture_tracker.h"

namespace qc {

enum class LedPattern : uint8_t {
  Off = 0,
  On,
  SlowBlink,
  FastBlink,
  VeryFast,
};

struct LedState {
  LedPattern led2;
  LedPattern led3;
  LedPattern led4;
};

struct LedInput {
  bool fan_on;
  bool timer_running;
  bool improv_advertising;
  bool pair_mode_active;
  uint32_t key1_held_ms;
  uint32_t key2_held_ms;
  // Live dual-hold duration (0 unless both buttons currently held).
  // Driven by DualGestureTracker::dual_held_ms_now() in fan_controller.
  uint32_t dual_held_ms;
  bool dip_invalid;
};

class LedStateMachineLogic {
 public:
  static constexpr uint32_t WARNING_MS      = 3000u;
  static constexpr uint32_t COMMIT_MS       = 5000u;
  // Dual-button gesture (stock-firmware restore). Aliased from the canonical
  // source so the two can't drift.
  static constexpr uint32_t DUAL_WARNING_MS = DualGestureTracker::WARNING_MS;
  static constexpr uint32_t DUAL_COMMIT_MS  = DualGestureTracker::COMMIT_MS;

  static LedState compute_led_state(const LedInput &in) {
    // Priority 1: dual at commit -> all-LED panic (about to STOCK RESTORE).
    if (in.dual_held_ms >= DUAL_COMMIT_MS) {
      return LedState{LedPattern::VeryFast, LedPattern::VeryFast, LedPattern::VeryFast};
    }
    // Priority 2: dual in intermediate (5-10s) -> all-LED SlowBlink. Distinct
    // from single-commit VeryFast so the user knows they need to keep holding.
    if (in.dual_held_ms >= DUAL_WARNING_MS) {
      return LedState{LedPattern::SlowBlink, LedPattern::SlowBlink, LedPattern::SlowBlink};
    }
    // Priority 3: either single button at commit -> all-LED panic (factory reset / safe mode).
    if (in.key1_held_ms >= COMMIT_MS || in.key2_held_ms >= COMMIT_MS) {
      return LedState{LedPattern::VeryFast, LedPattern::VeryFast, LedPattern::VeryFast};
    }

    // Priority 5: DIP misconfigured -> all LEDs solid (OEM parity).
    if (in.dip_invalid) {
      LedState s{LedPattern::On, LedPattern::On, LedPattern::On};
      if (in.key1_held_ms >= WARNING_MS) s.led2 = LedPattern::FastBlink;
      if (in.key2_held_ms >= WARNING_MS) s.led4 = LedPattern::FastBlink;
      return s;
    }

    // Default = fan-state on LED2/3, BLE status on LED4.
    LedState s{};
    if (in.fan_on) {
      s.led2 = in.timer_running ? LedPattern::SlowBlink : LedPattern::On;
      s.led3 = LedPattern::On;
    } else {
      s.led2 = LedPattern::Off;
      s.led3 = LedPattern::Off;
    }
    s.led4 = ble_status_(in);

    // Priority 4: button warning. KEY1 grabs LED2; KEY2 grabs LED4.
    if (in.key1_held_ms >= WARNING_MS) {
      s.led2 = LedPattern::FastBlink;
    }
    if (in.key2_held_ms >= WARNING_MS) {
      s.led4 = LedPattern::FastBlink;  // overrides BLE status
    }
    return s;
  }

  static bool pattern_is_on(LedPattern p, uint32_t now_ms) {
    switch (p) {
      case LedPattern::Off:       return false;
      case LedPattern::On:        return true;
      case LedPattern::SlowBlink: return (now_ms / 500u) % 2u == 1u;
      case LedPattern::FastBlink: return (now_ms / 100u) % 2u == 1u;
      case LedPattern::VeryFast:  return (now_ms /  50u) % 2u == 1u;
    }
    return false;
  }

 private:
  // BLE status pattern for LED4 (OEM parity). Improv > Pair > idle.
  // Wi-Fi/HA connectivity is deliberately not shown on LEDs — better checked
  // via phone/dashboard.
  static LedPattern ble_status_(const LedInput &in) {
    if (in.improv_advertising) return LedPattern::VeryFast;
    if (in.pair_mode_active)   return LedPattern::SlowBlink;
    return LedPattern::Off;
  }
};

}  // namespace qc
