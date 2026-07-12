// ESPHome wrapper around FanControllerLogic.
// Owns: 3 relay outputs (low/med/high), DIP text_sensor reference, the
// HA-facing fan::Fan entity, and the plan-application sequencer.
// All decision logic lives in fan_controller_logic.h — this file is glue.

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/fan/fan.h"
#include "esphome/components/number/number.h"
#include "esphome/components/output/binary_output.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"

#include "dual_gesture_tracker.h"
#include "fan_controller_logic.h"

#include <array>

namespace esphome {
namespace quietcool {

class FanController : public Component, public fan::Fan {
 public:
  void set_low_relay(output::BinaryOutput *p)  { low_relay_  = p; }
  void set_med_relay(output::BinaryOutput *p)  { med_relay_  = p; }
  void set_high_relay(output::BinaryOutput *p) { high_relay_ = p; }
  // DIP-bit binary sensors (a, b, c per the OEM truth table —
  // a=GPIO13, b=GPIO21, c=GPIO14). Read in setup() to determine
  // which preset_modes / speed_count to expose to HA.
  void set_dip_a(binary_sensor::BinarySensor *s) { dip_a_ = s; }
  void set_dip_b(binary_sensor::BinarySensor *s) { dip_b_ = s; }
  void set_dip_c(binary_sensor::BinarySensor *s) { dip_c_ = s; }
  void set_louver_pop_open_ms(uint32_t ms) { louver_pop_open_ms_ = ms; }
  // Optional HA-toggleable switch that gates the pop-open behavior. When the
  // switch is OFF (or unset), Off->Low|Med transitions go directly to the
  // target speed with no HIGH prepend — matching OEM stock behavior.
  void set_louver_pop_open_switch(switch_::Switch *s) { louver_pop_open_switch_ = s; }
  // Optional HA-toggleable switch that, when ON, suppresses actual GPIO
  // writes on the relay outputs. All other decisions (DIP guards, plans,
  // louver-pop-open timeouts, HA state publishing) still execute. Useful
  // for testing/debugging firmware iterations while the fan is physically
  // wired in the attic. Off at boot (RESTORE_DEFAULT_OFF) so safety is the
  // default — user has to opt IN to dry-run.
  void set_dry_run_switch(switch_::Switch *s) { dry_run_switch_ = s; }
  // Countdown timer support.
  // `runtime_remaining_sensor` is published once per second while a timer
  // is active and bounced to 0 when the timer ends or the fan turns off.
  void set_runtime_remaining_sensor(sensor::Sensor *s) { runtime_remaining_sensor_ = s; }
  void set_max_run_ms(uint32_t ms) { max_run_ms_ = ms; }
  // Optional — HA-editable number entity holding the default countdown
  // duration in MINUTES that auto-arms on `fan.turn_on` from off (OEM-app
  // parity behavior). User can set to 0 in HA to disable (full-manual mode;
  // matches OEM physical-button behavior). The number entity should ship
  // `restore_value: true` so the user's choice survives reboots.
  // When unset (no number wired), default-arm is disabled.
  void set_default_run_number(number::Number *n) { default_run_number_ = n; }
  // Reboot-resume support.
  // The mode is sourced from an HA-toggleable switch (so the user can flip
  // restore on/off from the dashboard) instead of being hardcoded in YAML.
  // When the switch is OFF or unset, behavior is AlwaysOff (OEM-parity).
  // When ON, behavior is RestoreLastState. Toggling the switch at runtime
  // takes effect on the very next save (via on_restore_switch_changed).
  void set_restore_mode_switch(switch_::Switch *s) { restore_mode_switch_ = s; }
  // Called from YAML on the restore switch's on_turn_on/on_turn_off triggers.
  // Forces an immediate save so the NVS reflects the new mode without waiting
  // for the next speed/timer change (avoids a stale-resume on the next boot).
  void on_restore_switch_changed() { save_persisted_state_(); }
  // Optional time source — required for timer-restore to actually resume a
  // running timer. Without it, RestoreLastState falls back to stay-off whenever
  // a timer was running at the moment of reboot.
  void set_time(time::RealTimeClock *t) { time_ = t; }
  // HA-facing action — extend_runtime(delta_minutes).
  // Positive extends/starts; negative reduces; reducing to <= 0 turns off.
  // See compute_extend_action in fan_controller_logic.h for the full semantics.
  void extend_runtime(int32_t delta_minutes);
  // HA-facing action — set_runtime(speed, minutes). Absolute, deterministic.
  // `speed_str` accepts case-insensitive "off"/"low"/"med"/"high" (and "medium"
  // as an alias for "med"). `minutes`: 0 = indefinite, >0 = arm timer, <0 = NoOp.
  // Speed=Off cancels any running timer and turns the fan off regardless of minutes.
  // Caps at max_run_minutes, refuses speeds disallowed by DIP wiring.
  // See compute_set_runtime_action in fan_controller_logic.h for the full semantics.
  void set_runtime(const std::string &speed_str, int32_t minutes);
  // Cancel any running countdown timer, leaving the fan at its current speed.
  // Used by OEM BLE compat for SetMode("Run") = indefinite operation.
  // The 24h continuous-runtime watchdog still applies.
  void run_indefinitely();

  // Smart Mode sensor + threshold wiring (populated by codegen from YAML).
  void set_temp_sensor(sensor::Sensor *s) { temp_sensor_ = s; }
  void set_humidity_sensor(sensor::Sensor *s) { humidity_sensor_ = s; }
  void set_smart_temp_high(number::Number *n) { smart_temp_high_ = n; }
  void set_smart_temp_med(number::Number *n) { smart_temp_med_ = n; }
  void set_smart_temp_low(number::Number *n) { smart_temp_low_ = n; }
  void set_smart_hum_high(number::Number *n) { smart_hum_high_ = n; }
  void set_smart_hum_low(number::Number *n) { smart_hum_low_ = n; }
  void set_smart_temp_high_switch(switch_::Switch *s) { set_threshold_switch_(::qc::SmartThreshold::TempHigh, s); }
  void set_smart_temp_med_switch(switch_::Switch *s)  { set_threshold_switch_(::qc::SmartThreshold::TempMed,  s); }
  void set_smart_temp_low_switch(switch_::Switch *s)  { set_threshold_switch_(::qc::SmartThreshold::TempLow,  s); }
  void set_smart_hum_high_switch(switch_::Switch *s)  { set_threshold_switch_(::qc::SmartThreshold::HumHigh,  s); }
  void set_smart_hum_low_switch(switch_::Switch *s)   { set_threshold_switch_(::qc::SmartThreshold::HumLow,   s); }
  void set_mode_select(select::Select *s) { mode_select_ = s; }
  void set_smart_hum_response(select::Select *s) { smart_hum_response_ = s; }
  void set_smart_mode_status(text_sensor::TextSensor *s) { smart_mode_status_ = s; }
  void set_fan_speed_sensor(text_sensor::TextSensor *s) { fan_speed_sensor_ = s; }

  // Called from the YAML 10s interval. Checks over-temp + 24h watchdog
  // (all modes), then runs the Smart Mode decision tree if active.
  void smart_tick();
  void activate_smart_mode();
  void deactivate_smart_mode();
  bool is_smart_mode_active() const { return smart_mode_active_; }
  bool is_watchdog_tripped() const { return watchdog_tripped_; }
  void reset_watchdog();
  void set_watchdog_sensor(binary_sensor::BinarySensor *s) { watchdog_sensor_ = s; }
  // Optional one-shot diagnostic — receives configured_speed_count_ after
  // the DIP truth table runs in setup(). Lets YAML surface the speed-tap
  // count to HA without re-decoding the DIP bits.
  void set_speeds_available_sensor(sensor::Sensor *s) { speeds_available_sensor_ = s; }

  void setup() override;
  void on_shutdown() override;
  void dump_config() override;
  // Run AFTER binary_sensors (HARDWARE = 200) so their state is populated
  // when we read it in setup(). Stays well above the API server's connection
  // priority (-10) so traits announce correctly.
  float get_setup_priority() const override { return setup_priority::HARDWARE - 1.0f; }
  fan::FanTraits get_traits() override;

  // KEY1 short-press helper — drives the OEM speed-cycle sequence on this DIP.
  // Call from a YAML lambda hooked to the KEY1 binary_sensor on_press.
  void on_key1_press();

  // Button-hold tracking (delegated to DualGestureTracker).
  // Pressed = start the hold timer. Released returns the held duration in ms
  // and clears the tracking — the YAML on_release lambda decides what to do
  // based on the duration (short → speed-cycle / Improv, long → factory-
  // reset / safe-mode). All gesture-to-action mapping lives in YAML so
  // global ESPHome APIs (App, global_preferences, id(safe_mode_id), etc.)
  // are accessible at the call site without dragging cross-component
  // includes into this header.
  // end_keyN_press returns 0 when the release is part of a dual-button
  // gesture (suppresses single-button action); the YAML lambda should
  // immediately check take_pending_dual_gesture_ms() to detect that case.
  void on_key1_pressed() { gesture_.on_press(1, millis()); }
  void on_key2_pressed() { gesture_.on_press(2, millis()); }
  uint32_t end_key1_press() { return gesture_.on_release(1, millis()); }
  uint32_t end_key2_press() { return gesture_.on_release(2, millis()); }
  // One-shot: returns the dual-hold duration in ms after a dual gesture has
  // just ended (set on the FIRST release of a dual hold; cleared by this
  // call). 0 otherwise. The YAML release lambda compares against
  // DualGestureTracker::COMMIT_MS to decide whether to fire stock restore.
  uint32_t take_pending_dual_gesture_ms() {
    return gesture_.take_pending_dual_held_ms();
  }
  // Consumes the pending dual-gesture state from the tracker and routes it:
  // on commit (≥ COMMIT_MS) presses the configured stock-restore button; on
  // abort (released earlier) logs and drops it. Returns true if a dual
  // gesture was present (whether committed or aborted) — caller should skip
  // its single-key action branches. Pair with end_keyN_press() ordering:
  // call end_keyN_press() FIRST to populate the tracker's pending state,
  // then this method to dispatch it.
  bool dispatch_pending_dual_gesture();
  void set_stock_restore_button(button::Button *b) { stock_restore_button_ = b; }
  // Live held durations in ms — 0 if not currently held. Read by
  // LedStateMachine at its 10 Hz tick.
  uint32_t key1_held_ms() const { return gesture_.key1_held_ms_now(millis()); }
  uint32_t key2_held_ms() const { return gesture_.key2_held_ms_now(millis()); }
  uint32_t dual_held_ms() const { return gesture_.dual_held_ms_now(millis()); }
  // Accessors for LED state machine + OEM BLE compat.
  bool fan_is_on() const { return current_speed_ != ::qc::Speed::Off; }
  bool timer_is_running() const { return runtime_endpoint_ms_ != 0; }
  bool dip_is_invalid() const { return boot_dip_ == ::qc::Dip::None; }
  uint8_t current_speed_enum() const { return static_cast<uint8_t>(current_speed_); }
  uint8_t last_speed_enum() const { return static_cast<uint8_t>(last_speed_); }
  uint8_t current_dip_enum() const { return static_cast<uint8_t>(boot_dip_); }
  // Generic threshold-enable accessors. Used by Smart Mode evaluation (to
  // skip disabled bands) and by oem_ble_compat (to honor OEM 0xFF/0x7FFF
  // "disable this band" sentinels on incoming SetTempHumidity / preset apply).
  // When no switch is wired the threshold is treated as ENABLED — preserves
  // OEM-parity default for builds that omit the per-threshold toggles.
  bool is_threshold_enabled(::qc::SmartThreshold t) const;
  void set_threshold_enabled(::qc::SmartThreshold t, bool en);

 protected:
  void control(const fan::FanCall &call) override;
  void start_plan_(qc::Plan plan);
  void advance_plan_();
  void apply_step_(const qc::PlanStep &step);
  qc::Dip read_dip_() const;
  ::qc::Speed parse_call_(const fan::FanCall &call) const;
  // Returns the configured louver_pop_open_ms only when the runtime switch is
  // ON (or unset, treated as disabled). 0 means "no pop-open, dispatch directly".
  uint32_t effective_louver_pop_open_ms_() const;
  // True when dry-run is active — apply_step_ skips GPIO writes if so.
  bool dry_run_active_() const;

  output::BinaryOutput *low_relay_  = nullptr;
  output::BinaryOutput *med_relay_  = nullptr;
  output::BinaryOutput *high_relay_ = nullptr;
  binary_sensor::BinarySensor *dip_a_ = nullptr;
  binary_sensor::BinarySensor *dip_b_ = nullptr;
  binary_sensor::BinarySensor *dip_c_ = nullptr;
  switch_::Switch *louver_pop_open_switch_ = nullptr;
  switch_::Switch *dry_run_switch_ = nullptr;

  // Cached at setup() from the 3 dip_* binary_sensors. Defines what the
  // HA fan entity exposes (preset_modes, speed_count) and how parse_call_
  // maps HA's speed_level integer back to qc::Speed. Mid-run physical DIP
  // changes are not picked up — a reboot is required to refresh the entity.
  ::qc::Dip boot_dip_ = ::qc::Dip::None;
  uint8_t configured_speed_count_ = 0;

  uint32_t louver_pop_open_ms_ = 0;
  ::qc::Speed current_speed_ = ::qc::Speed::Off;
  uint8_t  key1_cycle_counter_ = 0;

  // Button-hold tracker — owns press/release timestamps for both
  // KEY1 and KEY2 and the dual-hold derivation logic. All time inputs come
  // from millis() at the public entry points above.
  ::qc::DualGestureTracker gesture_;

  // Pending plan (when a transition has >1 step, e.g. louver pop-open)
  qc::Plan pending_plan_;
  size_t   pending_idx_ = 0;
  uint32_t pending_seq_ = 0;  // bumped on every start_plan_; cancels stale timeouts

  // Persisted state across reboots. Wall-clock based (UNIX
  // seconds) so a reboot can correctly compute remaining time from the saved
  // endpoint, independent of the monotonic millis() clock that resets on boot.
  struct PersistedState {
    uint8_t  speed;             // qc::Speed encoded; 0 = Off
    uint32_t endpoint_unix_s;   // 0 = no timer was running
  } __attribute__((packed));
  static_assert(sizeof(PersistedState) == 5, "PersistedState must stay 5 bytes for NVS schema stability");

  switch_::Switch *restore_mode_switch_ = nullptr;
  time::RealTimeClock *time_ = nullptr;
  ESPPreferenceObject pref_;
  // Resolve the current restore mode from the switch's live state. Returns
  // AlwaysOff when no switch is wired or when the switch is OFF — both
  // preserve the OEM-parity default.
  ::qc::RestoreMode effective_restore_mode_() const {
    return (restore_mode_switch_ != nullptr && restore_mode_switch_->state)
               ? ::qc::RestoreMode::RestoreLastState
               : ::qc::RestoreMode::AlwaysOff;
  }
  // True between setup() and the deferred restore decision firing. While true,
  // apply_step_ doesn't write to NVS (would clobber the saved state before we've
  // had a chance to consider restoring it).
  bool restore_pending_ = false;
  uint32_t restore_deadline_ms_ = 0;  // millis() at which we give up waiting for time sync

  void save_persisted_state_();
  bool load_persisted_state_(PersistedState *out);
  void schedule_restore_check_();
  void attempt_restore_now_();

  // Countdown timer state.
  // `runtime_endpoint_ms_` == 0 means "no timer running" (fan runs indefinitely
  // when on, stays off when off). When > 0 it's the millis() at which the fan
  // should turn off; a set_timeout fires the actual turn-off, and a 1Hz
  // set_interval publishes the live remaining-seconds value.
  uint32_t runtime_endpoint_ms_ = 0;
  uint32_t max_run_ms_ = 24u * 60u * 60u * 1000u;  // 24h default cap
  sensor::Sensor *runtime_remaining_sensor_ = nullptr;
  number::Number *default_run_number_ = nullptr;
  // Returns the user's HA-set default run minutes, or 0 if the feature is
  // disabled (number unset, NaN, or user-set to 0). Read on every turn-on
  // so live HA edits take effect on the next press without a reboot.
  uint32_t effective_default_run_minutes_() const;
  // Remembered across off transitions so `extend_runtime(+N)` from OFF resumes
  // the user's most recently chosen speed. Default High = OEM "Run" default.
  // Restored on boot via the base class's FanRestoreState (saved
  // automatically by publish_state() → save_state_()).
  ::qc::Speed last_speed_ = ::qc::Speed::High;

  // Smart Mode state.
  bool smart_mode_active_ = false;
  bool sensor_stabilized_ = false;
  bool overtemp_tripped_ = false;
  bool watchdog_tripped_ = false;
  uint32_t relay_on_start_ms_ = 0;
  uint32_t last_valid_sensor_ms_ = 0;
  sensor::Sensor *temp_sensor_ = nullptr;
  sensor::Sensor *humidity_sensor_ = nullptr;
  number::Number *smart_temp_high_ = nullptr;
  number::Number *smart_temp_med_ = nullptr;
  number::Number *smart_temp_low_ = nullptr;
  number::Number *smart_hum_high_ = nullptr;
  number::Number *smart_hum_low_ = nullptr;
  // One slot per qc::SmartThreshold value; nullptr means "no switch wired"
  // (treated as ENABLED at the call sites — OEM-parity default).
  std::array<switch_::Switch*, static_cast<size_t>(::qc::SmartThreshold::Count)> threshold_sw_ = {};
  void set_threshold_switch_(::qc::SmartThreshold t, switch_::Switch *s) {
    threshold_sw_[static_cast<size_t>(t)] = s;
  }
  select::Select *mode_select_ = nullptr;
  bool syncing_mode_ = false;
  void publish_mode_(const char *mode);
  select::Select *smart_hum_response_ = nullptr;
  text_sensor::TextSensor *smart_mode_status_ = nullptr;
  text_sensor::TextSensor *fan_speed_sensor_ = nullptr;
  binary_sensor::BinarySensor *watchdog_sensor_ = nullptr;
  sensor::Sensor *speeds_available_sensor_ = nullptr;
  button::Button *stock_restore_button_ = nullptr;
  ESPPreferenceObject smart_pref_;
  ::qc::SmartConfig build_smart_config_() const;
  void publish_smart_status_(const char *status);
  ::qc::Speed parse_hum_response_() const;
  void import_oem_smart_thresholds_();
  void save_smart_mode_();
  void trip_watchdog_(const char *reason);

  void apply_extend_action_(const ::qc::ExtendAction &a);
  void apply_set_runtime_action_(const ::qc::SetRuntimeAction &a);
  // Drives the fan to `target` and (re)arms the runtime timer for `endpoint_ms`.
  // Shared between extend_runtime's StartTimer/UpdateTimer paths and
  // set_runtime's SetSpeedWithTimer path. Cancels any prior runtime_stop.
  // endpoint_ms must be > millis(); caller checks for "would expire now".
  void arm_runtime_timer_(::qc::Speed target, uint32_t endpoint_ms);
  void cancel_runtime_timer_();
  // Canonical "drive fan to Off and tear down any active runtime timer" path.
  // Shared by timer expiry, watchdog trip, and over-temp trip.
  void force_off_();
  void start_runtime_publisher_();
  void publish_runtime_remaining_();
};

}  // namespace quietcool
}  // namespace esphome
