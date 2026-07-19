// FanControllerLogic — pure, header-only state-machine for the QuietCool
// IT-AF-SMT replacement firmware. Encodes the OEM relay-control contract
// (one-hot relay drive, DIP-guarded speed selection).
// NO ESPHome includes — testable on host.
//
// The ESPHome-side wrapper (fan_controller.h/cpp) holds GPIO outputs and
// applies the decisions this class produces.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace qc {

// Logical speed in the OEM "channel number" encoding.
enum class Speed : uint8_t {
  Off = 0,
  Low = 1,   // GPIO 5
  Med = 2,   // GPIO 22
  High = 3,  // GPIO 23
};

// DIP wiring encoding (values match the OEM firmware's internal encoding).
// Names describe SPEED TAPS, not motor count (every IT-AF-SMT fan has one motor;
// the DIP picks which speed windings are wired). The OEM firmware calls these
// MOTOR_NO/ONE/TWO/THREE — renamed here for readability in app-level code.
enum class Dip : uint8_t {
  None = 0,        // invalid / unconfigured DIP — all speeds refused
  TwoSpeed = 1,    // 2-speed:  {Off, Low, High}
  ThreeSpeed = 2,  // 3-speed:  {Off, Low, Med, High}
  OneSpeed = 3,    // 1-speed:  {Off, High}
};

// Physical relay drive state — exactly one bit set when active, all zero on Off.
struct RelayState {
  bool low;
  bool med;
  bool high;
};

// One step of a dispatch plan: drive `target` for `duration_ms` then advance
// to the next step. duration_ms == 0 means "hold indefinitely" (terminal step).
struct PlanStep {
  Speed target;
  uint32_t duration_ms;
};

// A dispatch plan is a sequence of steps. Empty plan == refused transition.
using Plan = std::vector<PlanStep>;

// Countdown-timer state machine.
// `extend_runtime(delta_minutes)` produces one of these actions that the
// ESPHome wrapper applies. Negative deltas can reduce the timer or cancel
// it (turn off); positive deltas can start a timer on an idle fan.
enum class ExtendActionKind : uint8_t {
  NoOp,         // do nothing (defensive: delta=0, or off+negative)
  TurnOff,      // cancel timer, drive Speed::Off
  StartTimer,   // turn fan on at resume_speed AND set endpoint
  UpdateTimer,  // fan already on; just adjust endpoint (or start timer on a running fan)
};

struct ExtendAction {
  ExtendActionKind kind;
  uint32_t new_endpoint_ms;  // valid for StartTimer/UpdateTimer; ignored otherwise
  Speed resume_speed;        // valid for StartTimer only; ignored otherwise
};

// Reboot-resume policy.
// AlwaysOff matches OEM cold-boot behavior; RestoreLastState reads persisted
// NVS state and (if time has synced) resumes the fan with the correct
// remaining timer. See decide_restore_action() for the full decision matrix.
enum class RestoreMode : uint8_t {
  AlwaysOff = 0,         // current behavior — cold boot = fan OFF regardless of prior state
  RestoreLastState = 1,  // opt-in: drive last speed; if a timer was running, arm with remaining
};

enum class RestoreActionKind : uint8_t {
  StayOff,           // do nothing on boot — match cold-boot default
  ResumeIndefinite,  // drive resume_speed; no timer
  ResumeWithTimer,   // drive resume_speed + arm timer for timer_ms
};

struct RestoreAction {
  RestoreActionKind kind;
  Speed resume_speed;  // valid for Resume*; ignored for StayOff
  uint32_t timer_ms;   // valid for ResumeWithTimer; 0 otherwise
};

// Absolute "run at SPEED for MINUTES" command — distinct from ExtendAction's
// delta-based semantics. Used by the HA `set_runtime` action so automations
// can deterministically pin the fan to a known state without depending on
// last_speed or current endpoint.
enum class SetRuntimeActionKind : uint8_t {
  NoOp,                // invalid input (negative minutes); leave state alone
  TurnOff,             // explicit off — cancel timer, drive Speed::Off
  SetSpeedIndefinite,  // drive target_speed, cancel any running timer (manual mode)
  SetSpeedWithTimer,   // drive target_speed, arm timer to new_endpoint_ms
};

struct SetRuntimeAction {
  SetRuntimeActionKind kind;
  Speed target_speed;        // valid for SetSpeed*; Off otherwise
  uint32_t new_endpoint_ms;  // valid for SetSpeedWithTimer; 0 otherwise
};

// Smart Mode threshold configuration. All temperatures in °C (HA auto-converts
// to the user's display unit via device_class: temperature). Defaults match the
// OEM firmware's (100/90/80 °F → 37.78/32.22/26.67 °C).
struct SmartConfig {
  float temp_high_c  = 37.78f;  // above this → HIGH speed
  float temp_med_c   = 32.22f;  // above this → MED (3-speed only)
  float temp_low_c   = 26.67f;  // above this → LOW
  float hum_high_pct = 90.0f;   // above this → STOP (condensation protection)
  float hum_low_pct  = 70.0f;   // above this → run at hum_response speed
  Speed hum_response = Speed::Low;  // speed on humidity trigger (Off/Low/Med/High)
  // Turn-off hysteresis (deadband). Applied ONLY while the fan is already
  // running: the temperature thresholds and hum_low run-trigger are lowered
  // by these amounts so the fan keeps running until the reading drops a band
  // below the turn-on point, instead of chattering at the boundary. 0 = no
  // deadband (exact OEM behavior: turn-on == turn-off). hum_high (condensation
  // cutoff) is a hard safety stop and is never relaxed.
  float temp_hyst_c  = 0.0f;    // °C deadband on the temperature thresholds
  float hum_hyst_pct = 0.0f;    // %RH deadband on the hum_low run-trigger
  bool temp_high_enabled = true;
  bool temp_med_enabled  = true;
  bool temp_low_enabled  = true;
  bool hum_high_enabled  = true;
  bool hum_low_enabled   = true;
};

// Smart Mode threshold identity — used to index the 5 parallel HA switches
// that gate each threshold's contribution to the decision tree (and to refer
// to each threshold across the fan_controller / oem_ble_compat boundary).
// `Count` is the array size — keep last; never persisted to NVS.
enum class SmartThreshold : uint8_t {
  TempHigh = 0,
  TempMed,
  TempLow,
  HumHigh,
  HumLow,
  Count,
};

// Watchdog and sensor timing constants.
static constexpr uint32_t WATCHDOG_MAX_RUNTIME_MS = 24u * 60u * 60u * 1000u;
static constexpr uint32_t SENSOR_STALE_MS         = 5u * 60u * 1000u;
static constexpr uint32_t SENSOR_STABILIZE_MS     = 60u * 1000u;

// OEM over-temperature safety cutoff: 182 °F = 83.33 °C.
// Two independent copies in OEM firmware — defense-in-depth.
// We implement one canonical check, called on every SHT30 update regardless
// of mode.
static constexpr float OVERTEMP_CUTOFF_C = 83.33f;

class FanControllerLogic {
 public:
  // Maps a logical speed to physical relay states.
  // One-hot invariant: at most one bit is true. Off => all false.
  static RelayState relays_for_speed(Speed s) {
    return RelayState{
        /*low=*/s == Speed::Low,
        /*med=*/s == Speed::Med,
        /*high=*/s == Speed::High,
    };
  }

  // Does the DIP wiring permit driving this speed?
  // Matches the OEM firmware's SetSpeed guards (LOW only on 2/3-speed wiring,
  // MED only on 3-speed wiring), with ONE deviation: we refuse HIGH on
  // None to close the OEM bug where HIGH fires regardless of DIP.
  // Off is always allowed.
  static bool dip_allows_speed(Dip dip, Speed s) {
    if (s == Speed::Off) return true;
    switch (dip) {
      case Dip::None:
        return false;  // closes OEM HIGH-unguarded bug
      case Dip::TwoSpeed:
        return s == Speed::Low || s == Speed::High;
      case Dip::ThreeSpeed:
        return s == Speed::Low || s == Speed::Med || s == Speed::High;
      case Dip::OneSpeed:
        return s == Speed::High;
    }
    return false;
  }

  // Result of a KEY1 short-press cycle step.
  struct CycleResult {
    Speed target;
    bool resets_counter;  // true => caller should reset counter to 0 for next press
  };

  // KEY1 short-press speed cycle — matches the OEM firmware's KEY1 cycle.
  // `counter` is the 1-based press counter (1 = first press after last cycle reset).
  // Cycles:
  //   TwoSpeed:    1->High, 2->Low, 3->Off+reset
  //   ThreeSpeed:  1->High, 2->Med, 3->Low, 4->Off+reset
  //   OneSpeed:    1->High, 2->Off+reset
  //   None:        any press -> Off+reset (invalid DIP forbids motor drive)
  static CycleResult cycle_next(uint8_t counter, Dip dip) {
    switch (dip) {
      case Dip::None:
        return {Speed::Off, true};
      case Dip::OneSpeed:
        if (counter == 1) return {Speed::High, false};
        return {Speed::Off, true};
      case Dip::TwoSpeed:
        if (counter == 1) return {Speed::High, false};
        if (counter == 2) return {Speed::Low,  false};
        return {Speed::Off, true};
      case Dip::ThreeSpeed:
        if (counter == 1) return {Speed::High, false};
        if (counter == 2) return {Speed::Med,  false};
        if (counter == 3) return {Speed::Low,  false};
        return {Speed::Off, true};
    }
    return {Speed::Off, true};
  }

  // Inputs from an HA fan::FanCall, flattened to POD so this is testable
  // host-side without ESPHome includes.
  struct CallInputs {
    bool has_state = false;       // call.get_state().has_value()
    bool state_value = false;     // call.get_state().value(); only valid if has_state
    const char *preset_mode = nullptr;  // nullptr/empty if not set
    bool has_speed = false;       // call.get_speed().has_value()
    int speed_idx = 0;            // 1-based; only valid if has_speed
  };

  // Translate an HA fan call to a concrete target Speed.
  //   - explicit state=false              -> Off
  //   - preset Low/Med/High               -> that speed (preset wins over numeric speed)
  //   - numeric speed_idx (1..N)          -> mapped per configured_speed_count
  //   - bare turn_on (no preset/speed)    -> last_non_off_speed (resume),
  //                                          else High if last is Off (first-boot UX)
  // last_non_off_speed must NEVER include Off in normal operation — the caller
  // is responsible for only writing non-Off values into the persisted last-speed
  // field. We accept Off here only as a defensive fallback.
  static Speed resolve_target_speed(const CallInputs &call,
                                    uint8_t configured_speed_count,
                                    Speed last_non_off_speed) {
    // Explicit off wins.
    if (call.has_state && !call.state_value) return Speed::Off;

    // Preset wins over numeric speed.
    if (call.preset_mode != nullptr && call.preset_mode[0] != '\0') {
      const char *p = call.preset_mode;
      if (p[0] == 'L' && p[1] == 'o' && p[2] == 'w' && p[3] == '\0') return Speed::Low;
      if (p[0] == 'M' && p[1] == 'e' && p[2] == 'd' && p[3] == '\0') return Speed::Med;
      if (p[0] == 'H' && p[1] == 'i' && p[2] == 'g' && p[3] == 'h' && p[4] == '\0') return Speed::High;
      // Unknown preset name — fall through to other resolution paths.
    }

    // Numeric speed index (1-based) — mapping depends on DIP-derived count.
    if (call.has_speed) {
      switch (configured_speed_count) {
        case 1: return Speed::High;
        case 2: return call.speed_idx == 1 ? Speed::Low : Speed::High;
        case 3:
          if (call.speed_idx == 1) return Speed::Low;
          if (call.speed_idx == 2) return Speed::Med;
          return Speed::High;
        default: return Speed::Off;
      }
    }

    // Bare turn_on. Resume the last non-Off speed; if none yet, default to
    // High (matches OEM button-cycle's "first press always HIGH" UX on first
    // boot, while preserving HA's "off->on restores last speed" expectation
    // for every subsequent toggle).
    if (last_non_off_speed != Speed::Off) return last_non_off_speed;
    return Speed::High;
  }

  // Generate the dispatch plan for `current -> target` on the given DIP.
  // Returns an empty plan if the transition is refused by dip_allows_speed.
  // louver_pop_open_ms: if > 0 AND going from Off to a non-High speed,
  //   prepend a HIGH pulse of that duration (mechanical-louver assist).
  //   Skipped if target == High (already starting on HIGH) or current != Off.
  static Plan plan_speed_transition(Speed current, Speed target, Dip dip,
                                    uint32_t louver_pop_open_ms) {
    if (!dip_allows_speed(dip, target)) return {};
    const bool pop_enabled = louver_pop_open_ms > 0;
    const bool starting_from_off = current == Speed::Off;
    const bool target_below_high = target != Speed::Off && target != Speed::High;
    if (pop_enabled && starting_from_off && target_below_high) {
      return {PlanStep{Speed::High, louver_pop_open_ms}, PlanStep{target, 0}};
    }
    return {PlanStep{target, 0}};
  }

  // Decide what to do when HA calls `extend_runtime(delta_minutes)`.
  // Pure decision function — wrapper is responsible for actually calling
  // set_timeout/set_interval/control() based on the returned action.
  //
  //   delta_minutes      signed; positive extends, negative reduces, 0 = no-op
  //   now_ms             monotonic clock (millis()) at call time
  //   fan_on             true if any non-Off relay is currently driven
  //   current_endpoint_ms  scheduled stop time; 0 means "no timer (run forever)"
  //   last_speed         speed to resume to when starting from Off. Off => fall back to High.
  //   max_run_ms         hard cap; endpoint never exceeds now_ms + max_run_ms
  //
  // All arithmetic uses int64 internally so a malicious/buggy HA call with
  // INT_MIN minutes can't underflow uint32 endpoints.
  static ExtendAction compute_extend_action(int32_t delta_minutes,
                                            uint32_t now_ms,
                                            bool fan_on,
                                            uint32_t current_endpoint_ms,
                                            Speed last_speed,
                                            uint32_t max_run_ms) {
    if (delta_minutes == 0) {
      return ExtendAction{ExtendActionKind::NoOp, 0, Speed::Off};
    }
    const int64_t delta_ms = static_cast<int64_t>(delta_minutes) * 60 * 1000;
    const int64_t now64 = static_cast<int64_t>(now_ms);
    const int64_t max_endpoint = now64 + static_cast<int64_t>(max_run_ms);

    // Branch 1: fan is OFF.
    if (!fan_on) {
      if (delta_minutes < 0) {
        // Reducing an off fan is meaningless — no-op (don't start it).
        return ExtendAction{ExtendActionKind::NoOp, 0, Speed::Off};
      }
      int64_t target = now64 + delta_ms;
      if (target > max_endpoint) target = max_endpoint;
      const Speed resume = (last_speed == Speed::Off) ? Speed::High : last_speed;
      return ExtendAction{ExtendActionKind::StartTimer,
                          static_cast<uint32_t>(target), resume};
    }

    // Branch 2: fan is ON, no timer (running indefinitely).
    if (current_endpoint_ms == 0) {
      if (delta_minutes < 0) {
        // Reducing an indefinite run means "stop it now".
        return ExtendAction{ExtendActionKind::TurnOff, 0, Speed::Off};
      }
      int64_t target = now64 + delta_ms;
      if (target > max_endpoint) target = max_endpoint;
      return ExtendAction{ExtendActionKind::UpdateTimer,
                          static_cast<uint32_t>(target), Speed::Off};
    }

    // Branch 3: fan is ON with active timer — adjust endpoint relative to current.
    int64_t target = static_cast<int64_t>(current_endpoint_ms) + delta_ms;
    if (target <= now64) {
      return ExtendAction{ExtendActionKind::TurnOff, 0, Speed::Off};
    }
    if (target > max_endpoint) target = max_endpoint;
    return ExtendAction{ExtendActionKind::UpdateTimer,
                        static_cast<uint32_t>(target), Speed::Off};
  }

  // Decide what to do on boot given persisted state.
  // Pure decision function — wrapper handles NVS load/save, time-sync waits,
  // and actually applying the restore (drive speed + arm timer).
  //
  //   mode                       YAML config — AlwaysOff disables the feature
  //   restored_speed             persisted last speed (Off means fan was off)
  //   restored_endpoint_unix_s   persisted wall-clock endpoint (0 = no timer)
  //   now_unix_s                 current wall-clock; ignored when !time_synced
  //   time_synced                did we get a valid NTP/HA time sync?
  //
  // The "no timer was running" path (endpoint=0) does NOT require time sync —
  // there's nothing to compute. Any timer-restore path DOES require sync
  // (without a clock we can't tell if the timer expired during the outage).
  static RestoreAction decide_restore_action(RestoreMode mode,
                                             Speed restored_speed,
                                             uint32_t restored_endpoint_unix_s,
                                             uint32_t now_unix_s,
                                             bool time_synced) {
    if (mode == RestoreMode::AlwaysOff) {
      return RestoreAction{RestoreActionKind::StayOff, Speed::Off, 0};
    }
    if (restored_speed == Speed::Off) {
      return RestoreAction{RestoreActionKind::StayOff, Speed::Off, 0};
    }
    if (restored_endpoint_unix_s == 0) {
      // No timer was running — safe to resume without a clock.
      return RestoreAction{RestoreActionKind::ResumeIndefinite, restored_speed, 0};
    }
    if (!time_synced) {
      // Timer was running but we can't compute remaining — fail safe.
      return RestoreAction{RestoreActionKind::StayOff, Speed::Off, 0};
    }
    if (restored_endpoint_unix_s <= now_unix_s) {
      // Timer would have expired during the outage.
      return RestoreAction{RestoreActionKind::StayOff, Speed::Off, 0};
    }
    const uint32_t remaining_s = restored_endpoint_unix_s - now_unix_s;
    return RestoreAction{RestoreActionKind::ResumeWithTimer, restored_speed,
                         remaining_s * 1000u};
  }

  // Absolute "run at SPEED for MINUTES" decision. Caller is responsible for
  // checking dip_allows_speed before applying — this function doesn't know
  // the DIP, only the timer/speed math.
  //
  //   target_speed   Off (cancel) or Low/Med/High (drive)
  //   minutes        0 = indefinite, >0 = arm timer, <0 = NoOp (invalid)
  //   now_ms         monotonic clock at call time
  //   max_run_ms     hard cap on absolute duration (saturates above)
  //
  // Negative minutes are rejected as invalid input rather than coerced — they
  // could come from a templated HA value going wrong, and silently treating
  // them as 0 would hide the bug. Off+anything-minutes is always TurnOff.
  static SetRuntimeAction compute_set_runtime_action(Speed target_speed,
                                                     int32_t minutes,
                                                     uint32_t now_ms,
                                                     uint32_t max_run_ms) {
    if (target_speed == Speed::Off) {
      return SetRuntimeAction{SetRuntimeActionKind::TurnOff, Speed::Off, 0};
    }
    if (minutes < 0) {
      return SetRuntimeAction{SetRuntimeActionKind::NoOp, Speed::Off, 0};
    }
    if (minutes == 0) {
      return SetRuntimeAction{SetRuntimeActionKind::SetSpeedIndefinite,
                              target_speed, 0};
    }
    const int64_t delta_ms = static_cast<int64_t>(minutes) * 60 * 1000;
    const int64_t now64 = static_cast<int64_t>(now_ms);
    const int64_t max_endpoint = now64 + static_cast<int64_t>(max_run_ms);
    int64_t target = now64 + delta_ms;
    if (target > max_endpoint) target = max_endpoint;
    return SetRuntimeAction{SetRuntimeActionKind::SetSpeedWithTimer,
                            target_speed, static_cast<uint32_t>(target)};
  }

  // Smart Mode decision tree — a port of the OEM firmware's decision logic.
  // Given current sensor readings and thresholds, returns
  // the speed the fan should be driven at. The caller is responsible for
  // actually driving relays and handling mode transitions.
  //
  // Condensation protection: humidity ABOVE hum_high → STOP (all wirings).
  // Temperature cascade: temp_high → HIGH, temp_med → MED (3-speed only),
  //   temp_low → LOW. Humidity trigger: humidity >= hum_low → hum_response.
  // Fail-closed: Dip::None always returns Off.
  static Speed compute_smart_speed(const SmartConfig &cfg, Dip dip,
                                   float temp_c, float humidity_pct,
                                   bool fan_running = false) {
    if (dip == Dip::None) return Speed::Off;
    // Condensation cutoff is a hard safety stop — never relaxed by hysteresis.
    if (cfg.hum_high_enabled && humidity_pct > cfg.hum_high_pct) return Speed::Off;

    // Turn-off hysteresis: while the fan is already running, lower the
    // temperature thresholds and the hum_low run-trigger by the configured
    // deadband so the fan holds until the reading drops a band below the
    // turn-on point (prevents chatter at the boundary). When off, or when the
    // deadband is 0, these equal the raw thresholds (exact OEM turn-on == off).
    const float hyst_t = fan_running ? cfg.temp_hyst_c : 0.0f;
    const float hyst_h = fan_running ? cfg.hum_hyst_pct : 0.0f;
    const float th_high = cfg.temp_high_c - hyst_t;
    const float th_med  = cfg.temp_med_c - hyst_t;
    const float th_low  = cfg.temp_low_c - hyst_t;
    const float hl_low  = cfg.hum_low_pct - hyst_h;

    switch (dip) {
      case Dip::TwoSpeed:
        if (cfg.temp_high_enabled && temp_c >= th_high) return Speed::High;
        if (cfg.temp_low_enabled && temp_c >= th_low) return Speed::Low;
        if (cfg.hum_low_enabled && humidity_pct >= hl_low) {
          if (cfg.hum_response == Speed::High) return Speed::High;
          if (cfg.hum_response == Speed::Low) return Speed::Low;
        }
        return Speed::Off;

      case Dip::ThreeSpeed:
        if (cfg.temp_high_enabled && temp_c >= th_high) return Speed::High;
        if (cfg.temp_med_enabled && temp_c >= th_med) return Speed::Med;
        if (cfg.temp_low_enabled && temp_c >= th_low) return Speed::Low;
        if (cfg.hum_low_enabled && humidity_pct >= hl_low) {
          if (cfg.hum_response == Speed::High) return Speed::High;
          if (cfg.hum_response == Speed::Med) return Speed::Med;
          if (cfg.hum_response == Speed::Low) return Speed::Low;
        }
        return Speed::Off;

      case Dip::OneSpeed:
        if (cfg.temp_high_enabled && temp_c >= th_high) return Speed::High;
        if (cfg.hum_low_enabled && humidity_pct >= hl_low && cfg.hum_response == Speed::High)
          return Speed::High;
        return Speed::Off;

      default:
        return Speed::Off;
    }
  }

  static bool is_overtemp(float temp_c) {
    return temp_c >= OVERTEMP_CUTOFF_C;
  }

  // Speed <-> 1-based HA speed index for the given DIP-derived count.
  // OneSpeed exposes index 1 only (always High). TwoSpeed maps 1=Low, 2=High.
  // ThreeSpeed maps 1=Low, 2=Med, 3=High. Off encodes as 0.
  static uint8_t speed_to_ha_index(Speed s, uint8_t configured_speed_count) {
    if (s == Speed::Off || configured_speed_count == 0) return 0;
    switch (configured_speed_count) {
      case 1: return 1;
      case 2: return (s == Speed::Low) ? 1 : 2;
      case 3:
        if (s == Speed::Low) return 1;
        if (s == Speed::Med) return 2;
        return 3;
      default: return 0;
    }
  }

  static Speed ha_index_to_speed(uint8_t idx, uint8_t configured_speed_count) {
    if (idx == 0 || configured_speed_count == 0) return Speed::Off;
    switch (configured_speed_count) {
      case 1: return Speed::High;
      case 2: return (idx == 1) ? Speed::Low : Speed::High;
      case 3:
        if (idx == 1) return Speed::Low;
        if (idx == 2) return Speed::Med;
        return Speed::High;
      default: return Speed::Off;
    }
  }

  // User-facing label for the fan_speed text_sensor. On 2-speed wiring Med
  // collapses to "High". On any other count the canonical "Low"/"Medium"/"High"
  // string is returned. Returns "High" for Off as a defensive fallback — the
  // caller should pass the "ref" speed (current if running, else last).
  static const char *speed_label(Speed s, uint8_t configured_speed_count) {
    if (configured_speed_count == 2) {
      return (s == Speed::Low) ? "Low" : "High";
    }
    // 1- or 3-speed (or 0): use the canonical labels.
    if (s == Speed::Low) return "Low";
    if (s == Speed::Med) return "Medium";
    return "High";
  }

  // Case-insensitive parse of an HA-facing speed string.
  // Accepts "off" / "low" / "med" / "medium" / "high"; "medium" is an OEM-UI
  // alias for "med". Returns true on success and writes into *out; returns
  // false (and leaves *out untouched) otherwise. Pure-logic so the alias set
  // is unit-testable host-side.
  static bool parse_speed_string(const char *s, Speed *out) {
    if (!s || !out) return false;
    char buf[8];
    size_t i = 0;
    for (; s[i] != '\0'; ++i) {
      if (i >= sizeof(buf) - 1) return false;  // longer than any accepted token
      char c = s[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      buf[i] = c;
    }
    buf[i] = '\0';
    if (std::strcmp(buf, "off") == 0)    { *out = Speed::Off;  return true; }
    if (std::strcmp(buf, "low") == 0)    { *out = Speed::Low;  return true; }
    if (std::strcmp(buf, "med") == 0 ||
        std::strcmp(buf, "medium") == 0) { *out = Speed::Med;  return true; }
    if (std::strcmp(buf, "high") == 0)   { *out = Speed::High; return true; }
    return false;
  }
};

}  // namespace qc
