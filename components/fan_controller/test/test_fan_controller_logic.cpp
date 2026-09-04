// Host-side unit tests for FanControllerLogic.
// Pure C++17, no ESPHome includes.
// Compile + run:
//   g++ -std=c++17 -I.. test_fan_controller_logic.cpp -o test_fan_controller_logic.exe
//   ./test_fan_controller_logic.exe

#include "test_utils.h"
#include "../fan_controller_logic.h"

using qc::Dip;
using qc::FanControllerLogic;
using qc::RelayState;
using qc::Speed;

// Pretty-printer + equality for RelayState (needed by REQUIRE_EQ).
namespace qc {
inline bool operator==(const RelayState& a, const RelayState& b) {
  return a.low == b.low && a.med == b.med && a.high == b.high;
}
inline std::ostream& operator<<(std::ostream& os, const RelayState& s) {
  return os << "{low=" << s.low << " med=" << s.med << " high=" << s.high << "}";
}

inline const char* speed_name(Speed s) {
  switch (s) {
    case Speed::Off:  return "Off";
    case Speed::Low:  return "Low";
    case Speed::Med:  return "Med";
    case Speed::High: return "High";
  }
  return "??";
}
inline std::ostream& operator<<(std::ostream& os, qc::Speed s) {
  return os << speed_name(s);
}
inline bool operator==(const qc::PlanStep& a, const qc::PlanStep& b) {
  return a.target == b.target && a.duration_ms == b.duration_ms;
}
inline std::ostream& operator<<(std::ostream& os, const qc::PlanStep& p) {
  return os << "{" << speed_name(p.target) << ", " << p.duration_ms << "ms}";
}
inline std::ostream& operator<<(std::ostream& os, const qc::Plan& plan) {
  os << "[";
  for (size_t i = 0; i < plan.size(); ++i) {
    if (i) os << ", ";
    os << plan[i];
  }
  return os << "]";
}
}  // namespace qc

// ============================================================================
// 1. relays_for_speed — maps logical speed to physical relay states.
// ============================================================================

TEST("relays_for_speed: Off drives nothing") {
  REQUIRE_EQ(FanControllerLogic::relays_for_speed(Speed::Off),
             RelayState{false, false, false});
}

TEST("relays_for_speed: Low drives only the LOW relay (one-hot)") {
  REQUIRE_EQ(FanControllerLogic::relays_for_speed(Speed::Low),
             RelayState{true, false, false});
}

TEST("relays_for_speed: Med drives only the MED relay (one-hot)") {
  REQUIRE_EQ(FanControllerLogic::relays_for_speed(Speed::Med),
             RelayState{false, true, false});
}

TEST("relays_for_speed: High drives only the HIGH relay (one-hot)") {
  REQUIRE_EQ(FanControllerLogic::relays_for_speed(Speed::High),
             RelayState{false, false, true});
}

// ============================================================================
// 2. dip_allows_speed — DIP wiring guards per the OEM firmware's SetSpeed guards,
//    EXCEPT HIGH+None (closes an OEM bug where HIGH fires regardless of DIP).
// ============================================================================

// Off is universally allowed (every dispatcher must accept "turn it off").
TEST("dip_allows_speed: Off allowed on every DIP") {
  REQUIRE(FanControllerLogic::dip_allows_speed(Dip::None,    Speed::Off));
  REQUIRE(FanControllerLogic::dip_allows_speed(Dip::TwoSpeed,   Speed::Off));
  REQUIRE(FanControllerLogic::dip_allows_speed(Dip::ThreeSpeed, Speed::Off));
  REQUIRE(FanControllerLogic::dip_allows_speed(Dip::OneSpeed,   Speed::Off));
}

// None (invalid DIP): everything except Off must be refused.
// (OEM has a bug where HIGH still fires — we refuse it.)
TEST("dip_allows_speed: None refuses every speed (closes OEM HIGH bug)") {
  REQUIRE(!FanControllerLogic::dip_allows_speed(Dip::None, Speed::Low));
  REQUIRE(!FanControllerLogic::dip_allows_speed(Dip::None, Speed::Med));
  REQUIRE(!FanControllerLogic::dip_allows_speed(Dip::None, Speed::High));
}

// TwoSpeed: {Off, Low, High} allowed; Med refused.
TEST("dip_allows_speed: TwoSpeed allows Low+High, refuses Med") {
  REQUIRE( FanControllerLogic::dip_allows_speed(Dip::TwoSpeed, Speed::Low));
  REQUIRE(!FanControllerLogic::dip_allows_speed(Dip::TwoSpeed, Speed::Med));
  REQUIRE( FanControllerLogic::dip_allows_speed(Dip::TwoSpeed, Speed::High));
}

// ThreeSpeed: all speeds allowed.
TEST("dip_allows_speed: ThreeSpeed allows every speed") {
  REQUIRE(FanControllerLogic::dip_allows_speed(Dip::ThreeSpeed, Speed::Low));
  REQUIRE(FanControllerLogic::dip_allows_speed(Dip::ThreeSpeed, Speed::Med));
  REQUIRE(FanControllerLogic::dip_allows_speed(Dip::ThreeSpeed, Speed::High));
}

// OneSpeed: only High allowed.
TEST("dip_allows_speed: OneSpeed allows only High") {
  REQUIRE(!FanControllerLogic::dip_allows_speed(Dip::OneSpeed, Speed::Low));
  REQUIRE(!FanControllerLogic::dip_allows_speed(Dip::OneSpeed, Speed::Med));
  REQUIRE( FanControllerLogic::dip_allows_speed(Dip::OneSpeed, Speed::High));
}

// ============================================================================
// 3. plan_speed_transition — produces the dispatch plan (channel + duration_ms).
//
// Baseline (no louver pop-open) — every transition is a single step holding
// the target indefinitely (duration_ms = 0 means "hold").
// ============================================================================

TEST("plan: Off->Low (no pop-open) is single-step hold") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Off, Speed::Low, Dip::TwoSpeed, /*louver_pop_open_ms=*/0);
  REQUIRE_EQ(plan, qc::Plan{{Speed::Low, 0}});
}

TEST("plan: Low->High (no pop-open) is single-step hold") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Low, Speed::High, Dip::TwoSpeed, 0);
  REQUIRE_EQ(plan, qc::Plan{{Speed::High, 0}});
}

TEST("plan: anything->Off is single-step hold at Off") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::High, Speed::Off, Dip::ThreeSpeed, 0);
  REQUIRE_EQ(plan, qc::Plan{{Speed::Off, 0}});
}

TEST("plan: refused transition (Med on TwoSpeed) yields empty plan") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Off, Speed::Med, Dip::TwoSpeed, 0);
  REQUIRE(plan.empty());
}

TEST("plan: refused transition (any speed on None) yields empty plan") {
  REQUIRE(FanControllerLogic::plan_speed_transition(
      Speed::Off, Speed::Low,  Dip::None, 0).empty());
  REQUIRE(FanControllerLogic::plan_speed_transition(
      Speed::Off, Speed::High, Dip::None, 0).empty());
}

TEST("plan: Off is always allowed even on None") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Low, Speed::Off, Dip::None, 0);
  REQUIRE_EQ(plan, qc::Plan{{Speed::Off, 0}});
}

// ============================================================================
// 4. Louver pop-open — when transitioning FROM Off TO a non-High speed on a
//    louvered fan, briefly drive HIGH first to pop the spring-loaded shutters
//    open via air pressure. ONLY active when louver_pop_open_ms > 0.
// ============================================================================

TEST("pop-open: Off->Low (TwoSpeed) prepends HIGH pulse") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Off, Speed::Low, Dip::TwoSpeed, /*pop=*/5000);
  REQUIRE_EQ(plan, qc::Plan({{Speed::High, 5000}, {Speed::Low, 0}}));
}

TEST("pop-open: Off->Med (ThreeSpeed) prepends HIGH pulse") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Off, Speed::Med, Dip::ThreeSpeed, 5000);
  REQUIRE_EQ(plan, qc::Plan({{Speed::High, 5000}, {Speed::Med, 0}}));
}

TEST("pop-open: Off->High does NOT prepend (already starting on HIGH)") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Off, Speed::High, Dip::TwoSpeed, 5000);
  REQUIRE_EQ(plan, qc::Plan{{Speed::High, 0}});
}

TEST("pop-open: Off->Off does NOT prepend (going off, not starting)") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Off, Speed::Off, Dip::TwoSpeed, 5000);
  REQUIRE_EQ(plan, qc::Plan{{Speed::Off, 0}});
}

TEST("pop-open: Low->High does NOT prepend (already running)") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Low, Speed::High, Dip::TwoSpeed, 5000);
  REQUIRE_EQ(plan, qc::Plan{{Speed::High, 0}});
}

TEST("pop-open: High->Low does NOT prepend (already running)") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::High, Speed::Low, Dip::TwoSpeed, 5000);
  REQUIRE_EQ(plan, qc::Plan{{Speed::Low, 0}});
}

TEST("pop-open: disabled (ms=0) means no prepend even from Off") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Off, Speed::Low, Dip::TwoSpeed, 0);
  REQUIRE_EQ(plan, qc::Plan{{Speed::Low, 0}});
}

TEST("pop-open: refused target still returns empty even with pop-open enabled") {
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Off, Speed::Med, Dip::TwoSpeed, 5000);  // Med refused on 2-speed
  REQUIRE(plan.empty());
}

TEST("pop-open: Off->Low on OneSpeed refused (Low not allowed) — even though pop would be HIGH (allowed)") {
  // Guard test: don't accidentally let pop-open bypass the dip guard on the
  // final target. OneSpeed allows only High; Low is refused regardless of pop.
  auto plan = FanControllerLogic::plan_speed_transition(
      Speed::Off, Speed::Low, Dip::OneSpeed, 5000);
  REQUIRE(plan.empty());
}

// ============================================================================
// 5. cycle_next — KEY1 short-press speed cycle (matches the OEM KEY1 cycle).
//    Counter starts at 0; advance by 1 on each button press. Returns the
//    speed the fan should be in AFTER this press. resets_counter=true means
//    "you just completed a cycle — reset the counter to 0 for the next press".
// ============================================================================

TEST("cycle: TwoSpeed press 1 -> High") {
  auto r = FanControllerLogic::cycle_next(1, Dip::TwoSpeed);
  REQUIRE_EQ(r.target, Speed::High);
  REQUIRE(!r.resets_counter);
}

TEST("cycle: TwoSpeed press 2 -> Low") {
  auto r = FanControllerLogic::cycle_next(2, Dip::TwoSpeed);
  REQUIRE_EQ(r.target, Speed::Low);
  REQUIRE(!r.resets_counter);
}

TEST("cycle: TwoSpeed press 3 -> Off + reset") {
  auto r = FanControllerLogic::cycle_next(3, Dip::TwoSpeed);
  REQUIRE_EQ(r.target, Speed::Off);
  REQUIRE(r.resets_counter);
}

TEST("cycle: ThreeSpeed press 1/2/3/4") {
  REQUIRE_EQ(FanControllerLogic::cycle_next(1, Dip::ThreeSpeed).target, Speed::High);
  REQUIRE_EQ(FanControllerLogic::cycle_next(2, Dip::ThreeSpeed).target, Speed::Med);
  REQUIRE_EQ(FanControllerLogic::cycle_next(3, Dip::ThreeSpeed).target, Speed::Low);
  auto r4 = FanControllerLogic::cycle_next(4, Dip::ThreeSpeed);
  REQUIRE_EQ(r4.target, Speed::Off);
  REQUIRE(r4.resets_counter);
}

TEST("cycle: OneSpeed press 1 -> High; press 2 -> Off + reset") {
  REQUIRE_EQ(FanControllerLogic::cycle_next(1, Dip::OneSpeed).target, Speed::High);
  auto r2 = FanControllerLogic::cycle_next(2, Dip::OneSpeed);
  REQUIRE_EQ(r2.target, Speed::Off);
  REQUIRE(r2.resets_counter);
}

TEST("cycle: None every press -> Off + reset (button can't drive a relay)") {
  for (uint8_t c = 1; c <= 5; ++c) {
    auto r = FanControllerLogic::cycle_next(c, Dip::None);
    REQUIRE_EQ(r.target, Speed::Off);
    REQUIRE(r.resets_counter);
  }
}

// ============================================================================
// 6. compute_extend_action — countdown timer math for the HA `extend_runtime`
//    service. Pure decision function: given the current state + a signed delta
//    in minutes, return the action the wrapper should apply.
//
//    Semantics:
//       delta == 0                    -> NoOp
//       fan OFF, delta > 0            -> StartTimer (resume_speed = last_speed
//                                       or High if last_speed == Off)
//       fan OFF, delta < 0            -> NoOp
//       fan ON,  no timer, delta > 0  -> UpdateTimer (start timer; fan stays on)
//       fan ON,  no timer, delta < 0  -> TurnOff (reducing an indefinite run stops it)
//       fan ON,  timer N,  delta > 0  -> UpdateTimer (endpoint += delta, cap at max)
//       fan ON,  timer N,  delta < 0  -> TurnOff if endpoint <= now; else UpdateTimer
//    Cap: target endpoint never exceeds now + max_run_ms.
// ============================================================================

using qc::ExtendAction;
using qc::ExtendActionKind;

// Pretty-printer for assertion failures.
namespace qc {
inline const char* kind_name(ExtendActionKind k) {
  switch (k) {
    case ExtendActionKind::NoOp:        return "NoOp";
    case ExtendActionKind::TurnOff:     return "TurnOff";
    case ExtendActionKind::StartTimer:  return "StartTimer";
    case ExtendActionKind::UpdateTimer: return "UpdateTimer";
  }
  return "??";
}
inline bool operator==(const ExtendAction& a, const ExtendAction& b) {
  return a.kind == b.kind && a.new_endpoint_ms == b.new_endpoint_ms &&
         a.resume_speed == b.resume_speed;
}
inline std::ostream& operator<<(std::ostream& os, const ExtendAction& a) {
  return os << "{kind=" << kind_name(a.kind) << " endpoint=" << a.new_endpoint_ms
            << " resume=" << speed_name(a.resume_speed) << "}";
}
}  // namespace qc

// Shorthand: minutes -> milliseconds (host-side, plain int).
static constexpr uint32_t MIN = 60u * 1000u;
static constexpr uint32_t MAX_RUN = 1440u * MIN;  // 24h, matches default cap
static constexpr uint32_t NOW = 1'000'000u;       // arbitrary `now` for tests

TEST("extend: delta=0 is NoOp regardless of state") {
  // Fan off, no timer.
  REQUIRE_EQ(FanControllerLogic::compute_extend_action(
                 0, NOW, /*fan_on=*/false, /*endpoint=*/0, Speed::High, MAX_RUN),
             ExtendAction{ExtendActionKind::NoOp, 0, Speed::Off});
  // Fan on with timer.
  REQUIRE_EQ(FanControllerLogic::compute_extend_action(
                 0, NOW, true, NOW + 10*MIN, Speed::Low, MAX_RUN),
             ExtendAction{ExtendActionKind::NoOp, 0, Speed::Off});
}

TEST("extend: fan OFF + positive delta starts timer at last speed") {
  auto a = FanControllerLogic::compute_extend_action(
      30, NOW, /*fan_on=*/false, /*endpoint=*/0, /*last=*/Speed::Low, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::StartTimer, NOW + 30*MIN, Speed::Low});
}

TEST("extend: fan OFF + positive delta + last_speed=Off falls back to High") {
  auto a = FanControllerLogic::compute_extend_action(
      30, NOW, false, 0, Speed::Off, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::StartTimer, NOW + 30*MIN, Speed::High});
}

TEST("extend: fan OFF + negative delta is NoOp") {
  auto a = FanControllerLogic::compute_extend_action(
      -30, NOW, false, 0, Speed::High, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::NoOp, 0, Speed::Off});
}

TEST("extend: fan ON + no timer + positive delta starts the timer (fan stays on)") {
  auto a = FanControllerLogic::compute_extend_action(
      30, NOW, /*fan_on=*/true, /*endpoint=*/0, Speed::High, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::UpdateTimer, NOW + 30*MIN, Speed::Off});
}

TEST("extend: fan ON + no timer + negative delta turns off (reducing indefinite stops it)") {
  auto a = FanControllerLogic::compute_extend_action(
      -1, NOW, true, 0, Speed::High, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::TurnOff, 0, Speed::Off});
}

TEST("extend: fan ON + timer + positive delta extends endpoint by delta") {
  // 10 minutes remaining, add 5 -> 15 minutes from now.
  auto a = FanControllerLogic::compute_extend_action(
      5, NOW, true, NOW + 10*MIN, Speed::High, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::UpdateTimer, NOW + 15*MIN, Speed::Off});
}

TEST("extend: fan ON + timer + small negative delta reduces remaining") {
  // 10 minutes remaining, subtract 5 -> 5 minutes from now.
  auto a = FanControllerLogic::compute_extend_action(
      -5, NOW, true, NOW + 10*MIN, Speed::High, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::UpdateTimer, NOW + 5*MIN, Speed::Off});
}

TEST("extend: fan ON + timer + delta reduces exactly to zero -> TurnOff") {
  auto a = FanControllerLogic::compute_extend_action(
      -10, NOW, true, NOW + 10*MIN, Speed::High, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::TurnOff, 0, Speed::Off});
}

TEST("extend: fan ON + timer + delta reduces below zero -> TurnOff") {
  auto a = FanControllerLogic::compute_extend_action(
      -15, NOW, true, NOW + 10*MIN, Speed::High, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::TurnOff, 0, Speed::Off});
}

TEST("extend: huge positive delta from OFF saturates at max_run_ms cap") {
  // Asking for 9999 minutes with a 1440-minute cap.
  auto a = FanControllerLogic::compute_extend_action(
      9999, NOW, false, 0, Speed::High, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::StartTimer, NOW + MAX_RUN, Speed::High});
}

TEST("extend: huge positive delta on a running timer saturates at now + max_run_ms") {
  // Endpoint already at NOW + 1430min; +99 would push to 1529min; cap kicks in at 1440.
  auto a = FanControllerLogic::compute_extend_action(
      99, NOW, true, NOW + 1430*MIN, Speed::High, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::UpdateTimer, NOW + MAX_RUN, Speed::Off});
}

TEST("extend: lower-than-cap positive delta passes through unchanged") {
  // Sanity: 120 min on top of 60 min running -> 180 min, well under cap.
  auto a = FanControllerLogic::compute_extend_action(
      120, NOW, true, NOW + 60*MIN, Speed::High, MAX_RUN);
  REQUIRE_EQ(a, ExtendAction{ExtendActionKind::UpdateTimer, NOW + 180*MIN, Speed::Off});
}

// ============================================================================
// 7. decide_restore_action — reboot-resume logic.
//    Given persisted state + wall-clock + RESTORE mode, decide what to do.
//
//    Semantics:
//       mode = AlwaysOff                              -> StayOff (feature off)
//       restored_speed = Off                          -> StayOff (nothing to restore)
//       restored_endpoint_unix_s = 0                  -> ResumeIndefinite (no timer was running)
//       !time_synced AND endpoint > 0                 -> StayOff (cannot compute remaining safely)
//       time_synced AND endpoint <= now               -> StayOff (timer would have expired)
//       time_synced AND endpoint > now                -> ResumeWithTimer { remaining_ms }
// ============================================================================

using qc::RestoreAction;
using qc::RestoreActionKind;
using qc::RestoreMode;

namespace qc {
inline const char* restore_kind_name(RestoreActionKind k) {
  switch (k) {
    case RestoreActionKind::StayOff:          return "StayOff";
    case RestoreActionKind::ResumeIndefinite: return "ResumeIndefinite";
    case RestoreActionKind::ResumeWithTimer:  return "ResumeWithTimer";
  }
  return "??";
}
inline bool operator==(const RestoreAction& a, const RestoreAction& b) {
  return a.kind == b.kind && a.resume_speed == b.resume_speed &&
         a.timer_ms == b.timer_ms;
}
inline std::ostream& operator<<(std::ostream& os, const RestoreAction& a) {
  return os << "{kind=" << restore_kind_name(a.kind)
            << " speed=" << speed_name(a.resume_speed)
            << " timer_ms=" << a.timer_ms << "}";
}
}  // namespace qc

// Shorthand for restore-action tests.
static constexpr uint32_t NOW_UNIX = 1'700'000'000u;  // arbitrary epoch second

TEST("restore: mode=AlwaysOff always StayOff regardless of persisted state") {
  // Even with a fully valid restored-running state, AlwaysOff means do nothing.
  auto a = FanControllerLogic::decide_restore_action(
      RestoreMode::AlwaysOff, Speed::High,
      NOW_UNIX + 60, NOW_UNIX, /*time_synced=*/true);
  REQUIRE_EQ(a, RestoreAction{RestoreActionKind::StayOff, Speed::Off, 0});
}

TEST("restore: restored_speed=Off -> StayOff (fan was off, nothing to do)") {
  auto a = FanControllerLogic::decide_restore_action(
      RestoreMode::RestoreLastState, Speed::Off,
      0, NOW_UNIX, true);
  REQUIRE_EQ(a, RestoreAction{RestoreActionKind::StayOff, Speed::Off, 0});
}

TEST("restore: no timer was running (endpoint=0) -> ResumeIndefinite at last speed") {
  auto a = FanControllerLogic::decide_restore_action(
      RestoreMode::RestoreLastState, Speed::High,
      0, NOW_UNIX, true);
  REQUIRE_EQ(a, RestoreAction{RestoreActionKind::ResumeIndefinite, Speed::High, 0});
}

TEST("restore: no timer was running + Low speed -> resume Low indefinitely") {
  auto a = FanControllerLogic::decide_restore_action(
      RestoreMode::RestoreLastState, Speed::Low,
      0, NOW_UNIX, true);
  REQUIRE_EQ(a, RestoreAction{RestoreActionKind::ResumeIndefinite, Speed::Low, 0});
}

TEST("restore: timer was running, time synced, endpoint in future -> ResumeWithTimer remaining") {
  // 90 seconds remain on the timer.
  auto a = FanControllerLogic::decide_restore_action(
      RestoreMode::RestoreLastState, Speed::High,
      NOW_UNIX + 90, NOW_UNIX, true);
  REQUIRE_EQ(a, RestoreAction{RestoreActionKind::ResumeWithTimer, Speed::High, 90 * 1000});
}

TEST("restore: timer would have expired during outage -> StayOff") {
  // Endpoint was 30s ago.
  auto a = FanControllerLogic::decide_restore_action(
      RestoreMode::RestoreLastState, Speed::High,
      NOW_UNIX - 30, NOW_UNIX, true);
  REQUIRE_EQ(a, RestoreAction{RestoreActionKind::StayOff, Speed::Off, 0});
}

TEST("restore: timer endpoint == now exactly -> StayOff") {
  auto a = FanControllerLogic::decide_restore_action(
      RestoreMode::RestoreLastState, Speed::High,
      NOW_UNIX, NOW_UNIX, true);
  REQUIRE_EQ(a, RestoreAction{RestoreActionKind::StayOff, Speed::Off, 0});
}

TEST("restore: timer was running but time NOT synced -> StayOff (refuse without clock)") {
  // We can't tell if the timer expired — be safe.
  auto a = FanControllerLogic::decide_restore_action(
      RestoreMode::RestoreLastState, Speed::High,
      NOW_UNIX + 600, NOW_UNIX, /*time_synced=*/false);
  REQUIRE_EQ(a, RestoreAction{RestoreActionKind::StayOff, Speed::Off, 0});
}

TEST("restore: no-timer case does NOT require time sync (indefinite resume is safe)") {
  // Endpoint=0 means no timer was running — we don't need a clock for this.
  auto a = FanControllerLogic::decide_restore_action(
      RestoreMode::RestoreLastState, Speed::High,
      0, /*now=*/0, /*time_synced=*/false);
  REQUIRE_EQ(a, RestoreAction{RestoreActionKind::ResumeIndefinite, Speed::High, 0});
}

TEST("restore: tiny remaining (1 second) -> ResumeWithTimer 1000ms") {
  auto a = FanControllerLogic::decide_restore_action(
      RestoreMode::RestoreLastState, Speed::Low,
      NOW_UNIX + 1, NOW_UNIX, true);
  REQUIRE_EQ(a, RestoreAction{RestoreActionKind::ResumeWithTimer, Speed::Low, 1000});
}

// ============================================================================
// 8. compute_set_runtime_action — absolute "run at SPEED for MINUTES" semantics.
//    Distinct from extend_runtime (which is delta-based and resumes last_speed
//    when starting from off). set_runtime is deterministic for HA automations:
//    "at 8pm: run High for 30 min" regardless of prior state.
//
//    Semantics:
//      speed = Off, any minutes      -> TurnOff (explicit off always wins)
//      speed != Off, minutes < 0     -> NoOp (negative absolute is nonsensical)
//      speed != Off, minutes == 0    -> SetSpeedIndefinite at target_speed
//      speed != Off, minutes > 0     -> SetSpeedWithTimer at NOW + minutes
//      Above max_run_ms cap          -> saturates at NOW + max_run_ms
// ============================================================================

using qc::SetRuntimeAction;
using qc::SetRuntimeActionKind;

namespace qc {
inline const char* set_kind_name(SetRuntimeActionKind k) {
  switch (k) {
    case SetRuntimeActionKind::NoOp:                return "NoOp";
    case SetRuntimeActionKind::TurnOff:             return "TurnOff";
    case SetRuntimeActionKind::SetSpeedIndefinite:  return "SetSpeedIndefinite";
    case SetRuntimeActionKind::SetSpeedWithTimer:   return "SetSpeedWithTimer";
  }
  return "??";
}
inline bool operator==(const SetRuntimeAction& a, const SetRuntimeAction& b) {
  return a.kind == b.kind && a.target_speed == b.target_speed &&
         a.new_endpoint_ms == b.new_endpoint_ms;
}
inline std::ostream& operator<<(std::ostream& os, const SetRuntimeAction& a) {
  return os << "{kind=" << set_kind_name(a.kind)
            << " speed=" << speed_name(a.target_speed)
            << " endpoint=" << a.new_endpoint_ms << "}";
}
}  // namespace qc

TEST("set_runtime: speed=Off + minutes=0 -> TurnOff") {
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::Off, 0, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::TurnOff, Speed::Off, 0});
}

TEST("set_runtime: speed=Off + minutes>0 -> TurnOff (explicit off wins)") {
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::Off, 60, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::TurnOff, Speed::Off, 0});
}

TEST("set_runtime: speed=Off + minutes<0 -> TurnOff") {
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::Off, -30, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::TurnOff, Speed::Off, 0});
}

TEST("set_runtime: speed=High + minutes=0 -> SetSpeedIndefinite High") {
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::High, 0, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::SetSpeedIndefinite, Speed::High, 0});
}

TEST("set_runtime: speed=Low + minutes=0 -> SetSpeedIndefinite Low (manual-mode pinning)") {
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::Low, 0, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::SetSpeedIndefinite, Speed::Low, 0});
}

TEST("set_runtime: speed=High + minutes=60 -> SetSpeedWithTimer at NOW+60min") {
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::High, 60, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::SetSpeedWithTimer,
                                  Speed::High, NOW + 60*MIN});
}

TEST("set_runtime: speed=Med + minutes=1 -> SetSpeedWithTimer at NOW+1min (smallest positive)") {
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::Med, 1, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::SetSpeedWithTimer,
                                  Speed::Med, NOW + 1*MIN});
}

TEST("set_runtime: speed=Low + minutes=1440 (exactly cap) -> SetSpeedWithTimer at NOW+MAX_RUN") {
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::Low, 1440, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::SetSpeedWithTimer,
                                  Speed::Low, NOW + MAX_RUN});
}

TEST("set_runtime: speed=High + minutes>cap -> saturates at NOW+MAX_RUN") {
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::High, 9999, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::SetSpeedWithTimer,
                                  Speed::High, NOW + MAX_RUN});
}

TEST("set_runtime: speed=High + minutes=INT_MAX -> saturates safely (no overflow)") {
  // Defensive — a runaway HA template shouldn't underflow the uint32 endpoint.
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::High, INT32_MAX, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::SetSpeedWithTimer,
                                  Speed::High, NOW + MAX_RUN});
}

TEST("set_runtime: speed=High + minutes=-30 -> NoOp (negative absolute minutes invalid)") {
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::High, -30, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::NoOp, Speed::Off, 0});
}

TEST("set_runtime: speed=Low + minutes=INT_MIN -> NoOp (still negative)") {
  auto a = FanControllerLogic::compute_set_runtime_action(
      Speed::Low, INT32_MIN, NOW, MAX_RUN);
  REQUIRE_EQ(a, SetRuntimeAction{SetRuntimeActionKind::NoOp, Speed::Off, 0});
}

// ============================================================================
// resolve_target_speed — translates an HA fan.turn_on/turn_off call into the
// concrete target Speed. Pure function so the bug-prone "what speed do we
// resume to" decision is testable in isolation.
// ============================================================================

using qc::FanControllerLogic;

// --- Off / preset paths ---

TEST("resolve_target: explicit state=false -> Off (regardless of last_speed)") {
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = false;
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 3, Speed::High), Speed::Off);
}

TEST("resolve_target: preset 'Low' -> Low") {
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = true; in.preset_mode = "Low";
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 3, Speed::High), Speed::Low);
}

TEST("resolve_target: preset 'Med' -> Med") {
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = true; in.preset_mode = "Med";
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 3, Speed::High), Speed::Med);
}

TEST("resolve_target: preset 'High' -> High") {
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = true; in.preset_mode = "High";
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 3, Speed::Low), Speed::High);
}

// --- Numeric speed_idx paths (1-based, depends on configured_speed_count) ---

TEST("resolve_target: speed_idx=1 on TwoSpeed -> Low") {
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = true; in.has_speed = true; in.speed_idx = 1;
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 2, Speed::High), Speed::Low);
}

TEST("resolve_target: speed_idx=2 on TwoSpeed -> High") {
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = true; in.has_speed = true; in.speed_idx = 2;
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 2, Speed::Low), Speed::High);
}

TEST("resolve_target: speed_idx=2 on ThreeSpeed -> Med") {
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = true; in.has_speed = true; in.speed_idx = 2;
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 3, Speed::Low), Speed::Med);
}

TEST("resolve_target: speed_idx=1 on OneSpeed -> High (only valid step)") {
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = true; in.has_speed = true; in.speed_idx = 1;
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 1, Speed::Off), Speed::High);
}

// --- Bare turn_on (no preset, no speed) — the bug-fix territory ---

TEST("resolve_target: bare turn_on resumes last_speed=Low (was bug: jumped to High)") {
  // The reported bug: set Low -> off -> on -> jumped to High instead of Low.
  // Bare turn_on should resume the last non-Off speed.
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = true;  // no preset, no speed_idx
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 2, Speed::Low), Speed::Low);
}

TEST("resolve_target: bare turn_on resumes last_speed=Med") {
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = true;
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 3, Speed::Med), Speed::Med);
}

TEST("resolve_target: bare turn_on with last_speed=Off falls back to High (first-boot UX)") {
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = true;
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 3, Speed::Off), Speed::High);
}

TEST("resolve_target: bare turn_on with last_speed=High stays High") {
  FanControllerLogic::CallInputs in{};
  in.has_state = true; in.state_value = true;
  REQUIRE_EQ(FanControllerLogic::resolve_target_speed(in, 3, Speed::High), Speed::High);
}

// ============================================================================
// Smart Mode: compute_smart_speed — OEM decision tree port
// ============================================================================

// Helper to build a default SmartConfig (OEM defaults in °C).
static qc::SmartConfig default_smart() {
  return qc::SmartConfig{};
}

// --- Dip::None always returns Off ---
TEST("smart: Dip::None always Off") {
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(default_smart(), Dip::None, 50.0f, 50.0f), Speed::Off);
}

// --- Condensation protection (all wirings): humidity > hum_high → Off ---
TEST("smart: condensation cutoff stops fan (TwoSpeed)") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 50.0f, 91.0f), Speed::Off);
}

TEST("smart: condensation cutoff is strict > not >= (TwoSpeed)") {
  auto cfg = default_smart();
  // humidity == hum_high (90) should NOT trigger condensation stop
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 50.0f, 90.0f), Speed::High);
}

// --- TwoSpeed decision tree ---
TEST("smart: TwoSpeed temp >= high → High") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 38.0f, 50.0f), Speed::High);
}

TEST("smart: TwoSpeed temp >= low but < high → Low") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 30.0f, 50.0f), Speed::Low);
}

TEST("smart: TwoSpeed temp below all thresholds, humidity below low → Off") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 20.0f, 50.0f), Speed::Off);
}

TEST("smart: TwoSpeed humidity trigger with hum_response=Low → Low") {
  auto cfg = default_smart();
  cfg.hum_response = Speed::Low;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 20.0f, 75.0f), Speed::Low);
}

TEST("smart: TwoSpeed humidity trigger with hum_response=High → High") {
  auto cfg = default_smart();
  cfg.hum_response = Speed::High;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 20.0f, 75.0f), Speed::High);
}

TEST("smart: TwoSpeed humidity trigger with hum_response=Off → Off") {
  auto cfg = default_smart();
  cfg.hum_response = Speed::Off;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 20.0f, 75.0f), Speed::Off);
}

TEST("smart: TwoSpeed humidity trigger with hum_response=Med → Off (Med invalid on 2-speed)") {
  auto cfg = default_smart();
  cfg.hum_response = Speed::Med;
  // OEM decision tree checks for High(3) and Low(1) only on TwoSpeed; Med falls through to Off
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 20.0f, 75.0f), Speed::Off);
}

// --- ThreeSpeed decision tree ---
TEST("smart: ThreeSpeed temp >= high → High") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::ThreeSpeed, 40.0f, 50.0f), Speed::High);
}

TEST("smart: ThreeSpeed temp >= med but < high → Med") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::ThreeSpeed, 33.0f, 50.0f), Speed::Med);
}

TEST("smart: ThreeSpeed temp >= low but < med → Low") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::ThreeSpeed, 28.0f, 50.0f), Speed::Low);
}

TEST("smart: ThreeSpeed temp below all, humidity trigger hum_response=Med → Med") {
  auto cfg = default_smart();
  cfg.hum_response = Speed::Med;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::ThreeSpeed, 20.0f, 75.0f), Speed::Med);
}

TEST("smart: ThreeSpeed temp below all, humidity trigger hum_response=High → High") {
  auto cfg = default_smart();
  cfg.hum_response = Speed::High;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::ThreeSpeed, 20.0f, 75.0f), Speed::High);
}

TEST("smart: ThreeSpeed temp below all, humidity below low → Off") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::ThreeSpeed, 20.0f, 50.0f), Speed::Off);
}

// --- OneSpeed decision tree ---
TEST("smart: OneSpeed temp >= high → High") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::OneSpeed, 40.0f, 50.0f), Speed::High);
}

TEST("smart: OneSpeed temp below high, humidity trigger hum_response=High → High") {
  auto cfg = default_smart();
  cfg.hum_response = Speed::High;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::OneSpeed, 20.0f, 75.0f), Speed::High);
}

TEST("smart: OneSpeed temp below high, humidity trigger hum_response=Low → Off (only High valid)") {
  auto cfg = default_smart();
  cfg.hum_response = Speed::Low;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::OneSpeed, 20.0f, 75.0f), Speed::Off);
}

TEST("smart: OneSpeed temp below high, humidity below low → Off") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::OneSpeed, 20.0f, 50.0f), Speed::Off);
}

// --- Temperature threshold priority: temp wins over humidity ---
TEST("smart: ThreeSpeed temp at high overrides humidity trigger") {
  auto cfg = default_smart();
  cfg.hum_response = Speed::Low;
  // Both temp and humidity thresholds met — temp cascade runs first
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::ThreeSpeed, 40.0f, 75.0f), Speed::High);
}

// --- Boundary tests (exact threshold values) ---
TEST("smart: TwoSpeed temp exactly at temp_high → High") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, cfg.temp_high_c, 50.0f), Speed::High);
}

TEST("smart: TwoSpeed temp just below temp_low → Off (no humidity trigger)") {
  auto cfg = default_smart();
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, cfg.temp_low_c - 0.01f, 50.0f), Speed::Off);
}

TEST("smart: humidity exactly at hum_low triggers humidity response") {
  auto cfg = default_smart();
  cfg.hum_response = Speed::Low;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 20.0f, cfg.hum_low_pct), Speed::Low);
}

TEST("smart: humidity just below hum_low does not trigger") {
  auto cfg = default_smart();
  cfg.hum_response = Speed::Low;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 20.0f, cfg.hum_low_pct - 0.01f), Speed::Off);
}

// --- Disabled threshold tests ---

TEST("smart: temp_high disabled, ThreeSpeed: high temp falls to Med") {
  auto cfg = default_smart();
  cfg.temp_high_enabled = false;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::ThreeSpeed, 40.0f, 50.0f), Speed::Med);
}

TEST("smart: temp_high+med disabled (Winter preset), ThreeSpeed: falls to Low") {
  auto cfg = default_smart();
  cfg.temp_high_enabled = false;
  cfg.temp_med_enabled = false;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::ThreeSpeed, 40.0f, 50.0f), Speed::Low);
}

TEST("smart: temp_low disabled, TwoSpeed: temp between low and high → Off") {
  auto cfg = default_smart();
  cfg.temp_low_enabled = false;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 28.0f, 50.0f), Speed::Off);
}

TEST("smart: hum_high disabled, condensation not triggered") {
  auto cfg = default_smart();
  cfg.hum_high_enabled = false;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 40.0f, 95.0f), Speed::High);
}

TEST("smart: hum_low disabled, humidity trigger skipped") {
  auto cfg = default_smart();
  cfg.hum_low_enabled = false;
  cfg.hum_response = Speed::Low;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 20.0f, 75.0f), Speed::Off);
}

TEST("smart: all thresholds disabled → Off") {
  auto cfg = default_smart();
  cfg.temp_high_enabled = false;
  cfg.temp_med_enabled = false;
  cfg.temp_low_enabled = false;
  cfg.hum_high_enabled = false;
  cfg.hum_low_enabled = false;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::ThreeSpeed, 40.0f, 95.0f), Speed::Off);
}

TEST("smart: Summer preset (hum_low disabled), temp triggers still work") {
  auto cfg = default_smart();
  cfg.hum_low_enabled = false;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 40.0f, 50.0f), Speed::High);
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::TwoSpeed, 28.0f, 50.0f), Speed::Low);
}

TEST("smart: temp_high disabled, OneSpeed: high temp → Off (only High available but disabled)") {
  auto cfg = default_smart();
  cfg.temp_high_enabled = false;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(cfg, Dip::OneSpeed, 40.0f, 50.0f), Speed::Off);
}

// ============================================================================
// Smart Mode hysteresis — the turn-off deadband applied while the fan is
// running. When fan_running is true, temperature thresholds and the humidity
// run-trigger (hum_low) are relaxed by the configured hysteresis so the fan
// keeps running until the reading drops a band below the turn-on point. The
// condensation cutoff (hum_high) is a hard safety stop and is NOT relaxed.
// fan_running defaults to false, so every test above exercises the turn-on path.
// ============================================================================

// --- fan_running=false: hysteresis has no effect on the turn-on decision ---
TEST("smart hysteresis: fan off ignores hysteresis (turn-on unchanged)") {
  auto cfg = default_smart();
  cfg.temp_hyst_c = 2.0f;
  // Just below the high threshold, fan off: must NOT turn on (needs >= high).
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(
                 cfg, Dip::OneSpeed, cfg.temp_high_c - 1.0f, 50.0f, /*fan_running=*/false),
             Speed::Off);
}

// --- fan_running=true: fan stays on within the hysteresis band ---
TEST("smart hysteresis: OneSpeed stays on within band while running") {
  auto cfg = default_smart();
  cfg.temp_hyst_c = 2.0f;
  // temp_high - 1 is inside [high-2, high): still running.
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(
                 cfg, Dip::OneSpeed, cfg.temp_high_c - 1.0f, 50.0f, /*fan_running=*/true),
             Speed::High);
}

// --- fan_running=true: fan turns off once below the band ---
TEST("smart hysteresis: OneSpeed turns off below band while running") {
  auto cfg = default_smart();
  cfg.temp_hyst_c = 2.0f;
  // temp_high - 3 is below high-2: turn off.
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(
                 cfg, Dip::OneSpeed, cfg.temp_high_c - 3.0f, 50.0f, /*fan_running=*/true),
             Speed::Off);
}

// --- Regression for GitHub issue #3: a 1-speed fan must turn off after the
//     attic cools well below the high threshold instead of latching on. ---
TEST("smart hysteresis: issue-3 regression — 1-speed fan turns off after cooling") {
  auto cfg = default_smart();
  cfg.temp_high_c = 31.11f;  // 88 F turn-on
  cfg.temp_hyst_c = 1.11f;   // 2 F deadband (default)
  // Evening cooldown to 78 F (25.56 C) while running: well below 88-2=86 F.
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(
                 cfg, Dip::OneSpeed, 25.56f, 50.0f, /*fan_running=*/true),
             Speed::Off);
}

// --- Zero hysteresis reproduces exact OEM behavior (turn-on == turn-off) ---
TEST("smart hysteresis: zero hysteresis turns off at the threshold while running") {
  auto cfg = default_smart();  // temp_hyst_c defaults to 0
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(
                 cfg, Dip::OneSpeed, cfg.temp_high_c - 0.01f, 50.0f, /*fan_running=*/true),
             Speed::Off);
}

// --- Humidity hysteresis relaxes hum_low while running ---
TEST("smart hysteresis: humidity band keeps fan on while running") {
  auto cfg = default_smart();
  cfg.hum_hyst_pct = 5.0f;
  cfg.hum_response = Speed::Low;
  // Temp below all thresholds; humidity inside [hum_low-5, hum_low): still running.
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(
                 cfg, Dip::TwoSpeed, 20.0f, cfg.hum_low_pct - 3.0f, /*fan_running=*/true),
             Speed::Low);
}

TEST("smart hysteresis: humidity below band turns fan off while running") {
  auto cfg = default_smart();
  cfg.hum_hyst_pct = 5.0f;
  cfg.hum_response = Speed::Low;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(
                 cfg, Dip::TwoSpeed, 20.0f, cfg.hum_low_pct - 7.0f, /*fan_running=*/true),
             Speed::Off);
}

// --- Condensation cutoff (hum_high) is a hard stop, never relaxed ---
TEST("smart hysteresis: condensation cutoff not relaxed while running") {
  auto cfg = default_smart();
  cfg.hum_hyst_pct = 5.0f;
  // Temp would drive High, but humidity just over hum_high must still STOP,
  // even though the fan is running and a humidity hysteresis is configured.
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(
                 cfg, Dip::TwoSpeed, 40.0f, cfg.hum_high_pct + 0.5f, /*fan_running=*/true),
             Speed::Off);
}

// --- Cascade: relaxed thresholds still cascade correctly while running ---
TEST("smart hysteresis: ThreeSpeed stays High within relaxed high band") {
  auto cfg = default_smart();
  cfg.temp_hyst_c = 2.0f;
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(
                 cfg, Dip::ThreeSpeed, cfg.temp_high_c - 1.0f, 50.0f, /*fan_running=*/true),
             Speed::High);
}

TEST("smart hysteresis: ThreeSpeed drops High->Med below relaxed high band") {
  auto cfg = default_smart();
  cfg.temp_hyst_c = 2.0f;
  // Below high-2 but above med-2: drops one tier to Med rather than off.
  REQUIRE_EQ(FanControllerLogic::compute_smart_speed(
                 cfg, Dip::ThreeSpeed, cfg.temp_high_c - 3.0f, 50.0f, /*fan_running=*/true),
             Speed::Med);
}

// --- overtemp_watchdog_tripped ---
TEST("overtemp watchdog: honors enable state and cutoff") {
  REQUIRE_EQ(FanControllerLogic::overtemp_watchdog_tripped(true, 80.0f), false);
  REQUIRE_EQ(FanControllerLogic::overtemp_watchdog_tripped(
                 true, qc::OVERTEMP_CUTOFF_C),
             true);
  REQUIRE_EQ(FanControllerLogic::overtemp_watchdog_tripped(true, 90.0f), true);
  REQUIRE_EQ(FanControllerLogic::overtemp_watchdog_tripped(false, 90.0f), false);
}

// --- Watchdog policy ---
TEST("watchdog start: enabled running fan starts once; disabled or off clears") {
  REQUIRE_EQ(FanControllerLogic::next_watchdog_start_ms(true, true, 0, 123u), 123u);
  REQUIRE_EQ(FanControllerLogic::next_watchdog_start_ms(true, true, 123u, 456u), 123u);
  REQUIRE_EQ(FanControllerLogic::next_watchdog_start_ms(false, true, 123u, 456u), 0u);
  REQUIRE_EQ(FanControllerLogic::next_watchdog_start_ms(true, false, 123u, 456u), 0u);
}

TEST("runtime watchdog: honors enable state and exact 24h boundary") {
  constexpr uint32_t START = 123u;
  REQUIRE_EQ(FanControllerLogic::runtime_watchdog_expired(
                 true, START, START + qc::WATCHDOG_MAX_RUNTIME_MS - 1u),
             false);
  REQUIRE_EQ(FanControllerLogic::runtime_watchdog_expired(
                 true, START, START + qc::WATCHDOG_MAX_RUNTIME_MS),
             true);
  REQUIRE_EQ(FanControllerLogic::runtime_watchdog_expired(
                 false, START, START + qc::WATCHDOG_MAX_RUNTIME_MS),
             false);
  REQUIRE_EQ(FanControllerLogic::runtime_watchdog_expired(true, 0, 0xFFFFFFFFu), false);
}

TEST("runtime watchdog: elapsed comparison survives millis wrap") {
  constexpr uint32_t START = 0xFFFFFF00u;
  const uint32_t now = START + qc::WATCHDOG_MAX_RUNTIME_MS;
  REQUIRE_EQ(FanControllerLogic::runtime_watchdog_expired(true, START, now), true);
}

TEST("sensor watchdog: honors enable state, missing sample, and boundary") {
  constexpr uint32_t LAST_VALID = 123u;
  REQUIRE_EQ(FanControllerLogic::sensor_watchdog_expired(true, 0, 456u), true);
  REQUIRE_EQ(FanControllerLogic::sensor_watchdog_expired(false, 0, 456u), false);
  REQUIRE_EQ(FanControllerLogic::sensor_watchdog_expired(
                 true, LAST_VALID, LAST_VALID + qc::SENSOR_STALE_MS - 1u),
             false);
  REQUIRE_EQ(FanControllerLogic::sensor_watchdog_expired(
                 true, LAST_VALID, LAST_VALID + qc::SENSOR_STALE_MS),
             true);
  REQUIRE_EQ(FanControllerLogic::sensor_watchdog_expired(
                 false, LAST_VALID, LAST_VALID + qc::SENSOR_STALE_MS),
             false);
}

TEST("sensor watchdog: elapsed comparison survives millis wrap") {
  constexpr uint32_t LAST_VALID = 0xFFFFFF00u;
  const uint32_t now = LAST_VALID + qc::SENSOR_STALE_MS;
  REQUIRE_EQ(FanControllerLogic::sensor_watchdog_expired(true, LAST_VALID, now), true);
}

// ============================================================================
// Speed <-> HA index, label, parse helpers
// ============================================================================

TEST("speed_to_ha_index: ThreeSpeed maps Low/Med/High to 1/2/3") {
  REQUIRE_EQ(FanControllerLogic::speed_to_ha_index(Speed::Low, 3), 1);
  REQUIRE_EQ(FanControllerLogic::speed_to_ha_index(Speed::Med, 3), 2);
  REQUIRE_EQ(FanControllerLogic::speed_to_ha_index(Speed::High, 3), 3);
}

TEST("speed_to_ha_index: TwoSpeed maps Low/High to 1/2 (no Med)") {
  REQUIRE_EQ(FanControllerLogic::speed_to_ha_index(Speed::Low, 2), 1);
  REQUIRE_EQ(FanControllerLogic::speed_to_ha_index(Speed::High, 2), 2);
}

TEST("speed_to_ha_index: OneSpeed maps High to 1") {
  REQUIRE_EQ(FanControllerLogic::speed_to_ha_index(Speed::High, 1), 1);
}

TEST("speed_to_ha_index: Off or count=0 returns 0") {
  REQUIRE_EQ(FanControllerLogic::speed_to_ha_index(Speed::Off, 3), 0);
  REQUIRE_EQ(FanControllerLogic::speed_to_ha_index(Speed::High, 0), 0);
}

TEST("ha_index_to_speed: ThreeSpeed maps 1/2/3 to Low/Med/High") {
  REQUIRE_EQ(FanControllerLogic::ha_index_to_speed(1, 3), Speed::Low);
  REQUIRE_EQ(FanControllerLogic::ha_index_to_speed(2, 3), Speed::Med);
  REQUIRE_EQ(FanControllerLogic::ha_index_to_speed(3, 3), Speed::High);
}

TEST("ha_index_to_speed: TwoSpeed maps 1/2 to Low/High") {
  REQUIRE_EQ(FanControllerLogic::ha_index_to_speed(1, 2), Speed::Low);
  REQUIRE_EQ(FanControllerLogic::ha_index_to_speed(2, 2), Speed::High);
}

TEST("ha_index_to_speed: OneSpeed any non-zero index is High") {
  REQUIRE_EQ(FanControllerLogic::ha_index_to_speed(1, 1), Speed::High);
}

TEST("ha_index_to_speed: idx 0 or count 0 returns Off") {
  REQUIRE_EQ(FanControllerLogic::ha_index_to_speed(0, 3), Speed::Off);
  REQUIRE_EQ(FanControllerLogic::ha_index_to_speed(1, 0), Speed::Off);
}

TEST("speed_label: ThreeSpeed labels are Low/Medium/High") {
  REQUIRE(std::string(FanControllerLogic::speed_label(Speed::Low, 3)) == "Low");
  REQUIRE(std::string(FanControllerLogic::speed_label(Speed::Med, 3)) == "Medium");
  REQUIRE(std::string(FanControllerLogic::speed_label(Speed::High, 3)) == "High");
}

TEST("speed_label: TwoSpeed labels are Low/High (Med collapses to High)") {
  REQUIRE(std::string(FanControllerLogic::speed_label(Speed::Low, 2)) == "Low");
  REQUIRE(std::string(FanControllerLogic::speed_label(Speed::High, 2)) == "High");
  REQUIRE(std::string(FanControllerLogic::speed_label(Speed::Med, 2)) == "High");
}

TEST("parse_speed_string: canonical tokens") {
  Speed s;
  REQUIRE(FanControllerLogic::parse_speed_string("off", &s));   REQUIRE_EQ(s, Speed::Off);
  REQUIRE(FanControllerLogic::parse_speed_string("low", &s));   REQUIRE_EQ(s, Speed::Low);
  REQUIRE(FanControllerLogic::parse_speed_string("med", &s));   REQUIRE_EQ(s, Speed::Med);
  REQUIRE(FanControllerLogic::parse_speed_string("medium", &s));REQUIRE_EQ(s, Speed::Med);
  REQUIRE(FanControllerLogic::parse_speed_string("high", &s));  REQUIRE_EQ(s, Speed::High);
}

TEST("parse_speed_string: case-insensitive") {
  Speed s;
  REQUIRE(FanControllerLogic::parse_speed_string("OFF", &s));   REQUIRE_EQ(s, Speed::Off);
  REQUIRE(FanControllerLogic::parse_speed_string("Low", &s));   REQUIRE_EQ(s, Speed::Low);
  REQUIRE(FanControllerLogic::parse_speed_string("Medium", &s));REQUIRE_EQ(s, Speed::Med);
  REQUIRE(FanControllerLogic::parse_speed_string("HIGH", &s));  REQUIRE_EQ(s, Speed::High);
}

TEST("parse_speed_string: rejects unknown / over-length input") {
  Speed s = Speed::High;
  REQUIRE_EQ(FanControllerLogic::parse_speed_string("turbo", &s), false);
  REQUIRE_EQ(FanControllerLogic::parse_speed_string("", &s), false);
  REQUIRE_EQ(FanControllerLogic::parse_speed_string("aReallyLongOne", &s), false);
  REQUIRE_EQ(FanControllerLogic::parse_speed_string(nullptr, &s), false);
  // *out untouched on failure.
  REQUIRE_EQ(s, Speed::High);
}

int main() { return tu::run_all(); }
