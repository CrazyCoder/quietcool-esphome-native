#include "fan_controller.h"
#include "../oem_ble_compat/oem_ble_compat_logic.h"
#include "../oem_nvs_reader/oem_nvs_reader.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace quietcool {

static const char *const TAG = "fan_controller";

void FanController::setup() {
  // Read DIP wiring NOW from the binary_sensors (whose own setup() already ran
  // due to our lower setup_priority). Cache the decoded value — HA will see
  // the resulting traits on first API connection.
  if (dip_a_ != nullptr && dip_b_ != nullptr && dip_c_ != nullptr) {
    const bool a = dip_a_->state;
    const bool b = dip_b_->state;
    const bool c = dip_c_->state;
    // OEM truth table (reverse-engineered from OEM firmware).
    // OEM names MOTOR_TWO/THREE/ONE/NO are mapped to our speed-tap-count names.
    //   GPIO13(a) GPIO21(b) GPIO14(c) ->
    //   1 0 1 = TwoSpeed   (Off, Low, High)         [OEM MOTOR_TWO]
    //   1 1 0 = ThreeSpeed (Off, Low, Med, High)    [OEM MOTOR_THREE]
    //   0 1 1 = OneSpeed   (Off, High)              [OEM MOTOR_ONE]
    //   *     = None       (invalid, all refused)   [OEM MOTOR_NO]
    if      ( a && !b &&  c) boot_dip_ = ::qc::Dip::TwoSpeed;
    else if ( a &&  b && !c) boot_dip_ = ::qc::Dip::ThreeSpeed;
    else if (!a &&  b &&  c) boot_dip_ = ::qc::Dip::OneSpeed;
    else                     boot_dip_ = ::qc::Dip::None;
  }

  // ESPHome 2026.4+ canonical API: preset modes are stored on the Fan entity
  // base (not on FanTraits anymore — the old path is deprecated 2026.11 and
  // its wire protocol doesn't fully route preset_mode commands through).
  // See developers.esphome.io/blog/2026/04/09/...
  switch (boot_dip_) {
    case ::qc::Dip::OneSpeed:
      this->set_supported_preset_modes({"High"});
      configured_speed_count_ = 1;
      break;
    case ::qc::Dip::TwoSpeed:
      this->set_supported_preset_modes({"Low", "High"});
      configured_speed_count_ = 2;
      break;
    case ::qc::Dip::ThreeSpeed:
      this->set_supported_preset_modes({"Low", "Med", "High"});
      configured_speed_count_ = 3;
      break;
    case ::qc::Dip::None:
    default:
      this->set_supported_preset_modes({});
      configured_speed_count_ = 0;
      break;
  }

  if (speeds_available_sensor_)
    speeds_available_sensor_->publish_state(configured_speed_count_);

  if (mode_select_) {
    mode_select_->add_on_state_callback([this](size_t index) {
      if (syncing_mode_ || restore_pending_) return;
      const char *mode = mode_select_->option_at(index);
      if (!mode) return;
      if (strcmp(mode, "Smart") == 0) {
        activate_smart_mode();
      } else if (strcmp(mode, "Run") == 0) {
        if (smart_mode_active_) deactivate_smart_mode();
        cancel_runtime_timer_();
        publish_mode_("Run");
      } else {
        if (smart_mode_active_) deactivate_smart_mode();
        if (current_speed_ != ::qc::Speed::Off && runtime_endpoint_ms_ == 0) {
          uint32_t minutes = effective_default_run_minutes_();
          if (minutes > 0) extend_runtime(static_cast<int32_t>(minutes));
        }
      }
    });
  }

  // Restore last speed from the base class's FanRestoreState (saved
  // automatically by publish_state() → save_state_() on every speed change).
  // With restore_mode ALWAYS_OFF, restore_state_() returns {state=false,
  // speed=<saved>} — we extract just the speed.
  auto restored = this->restore_state_();
  if (restored.has_value() && restored->speed > 0) {
    last_speed_ = ::qc::FanControllerLogic::ha_index_to_speed(
        restored->speed, configured_speed_count_);
  }

  // Boot policy: relays start OFF regardless of saved HA state. Matches OEM
  // cold-boot behavior where relays are always cleared on power-on.
  if (low_relay_)  low_relay_->set_state(false);
  if (med_relay_)  med_relay_->set_state(false);
  if (high_relay_) high_relay_->set_state(false);
  current_speed_ = ::qc::Speed::Off;
  this->state = false;
  // Seed the speed index from last_speed_ so HA remembers the correct
  // speed to resume to on the next turn_on toggle.
  this->speed = ::qc::FanControllerLogic::speed_to_ha_index(
      last_speed_, configured_speed_count_);
  this->publish_state();

  // Cold-boot the countdown timer to "no timer" and publish 0 so
  // HA's sensor shows a sane initial value instead of "unknown".
  runtime_endpoint_ms_ = 0;
  publish_runtime_remaining_();
  publish_smart_status_("Off");
  if (watchdog_sensor_) watchdog_sensor_->publish_state(false);
  if (fan_speed_sensor_) {
    fan_speed_sensor_->publish_state(
        ::qc::FanControllerLogic::speed_label(last_speed_, configured_speed_count_));
  }

  // Initialize NVS handle. Hash key is a fixed component-scoped
  // 32-bit value — keep it stable across firmware versions so old saved state
  // remains readable after an OTA. Bump only if the on-flash schema changes.
  this->pref_ = global_preferences->make_preference<PersistedState>(0x71630601 /* 'qc' + 06 + 01 */, true);

  // ALWAYS schedule the deferred restore check, even when the switch reads OFF
  // right now. Template switches with RESTORE_DEFAULT_OFF can have their state
  // populated AFTER our setup() runs (the priority ordering is "good enough"
  // but not strictly guaranteed by the scheduler), so reading the mode here
  // and short-circuiting risks wiping NVS the user actually wanted preserved.
  // The deferred callback at +1s gives the switch's state machine time to
  // settle, then reads effective_restore_mode_() once for the real decision.
  restore_pending_ = true;
  restore_deadline_ms_ = millis() + 30u * 1000u;
  schedule_restore_check_();

  // Smart Mode NVS preference (separate from the speed/timer pref so adding
  // Smart Mode doesn't break the existing 5-byte schema).
  this->smart_pref_ = global_preferences->make_preference<uint8_t>(0x71630602, true);

  // One-shot OEM Smart Mode threshold import.
  import_oem_smart_thresholds_();
}

void FanController::on_shutdown() {
  restore_pending_ = false;
  save_persisted_state_();
  save_smart_mode_();
  global_preferences->sync();
}

void FanController::extend_runtime(int32_t delta_minutes) {
  if (smart_mode_active_) deactivate_smart_mode();
  const uint32_t now_ms = millis();
  const bool fan_on = (current_speed_ != ::qc::Speed::Off);
  auto action = ::qc::FanControllerLogic::compute_extend_action(
      delta_minutes, now_ms, fan_on, runtime_endpoint_ms_, last_speed_, max_run_ms_);
  ESP_LOGD(TAG, "extend_runtime(%d min): kind=%d new_endpoint=%u resume=%d "
                "(now=%u fan_on=%d current_endpoint=%u last=%d)",
           delta_minutes, (int)action.kind, action.new_endpoint_ms,
           (int)action.resume_speed,
           now_ms, (int)fan_on, runtime_endpoint_ms_, (int)last_speed_);
  apply_extend_action_(action);
}

void FanController::apply_extend_action_(const ::qc::ExtendAction &a) {
  using ::qc::ExtendActionKind;
  switch (a.kind) {
    case ExtendActionKind::NoOp:
      return;

    case ExtendActionKind::TurnOff:
      force_off_();
      return;

    case ExtendActionKind::StartTimer:
      // Drive the fan at resume_speed (fan was off) and arm the timer.
      arm_runtime_timer_(a.resume_speed, a.new_endpoint_ms);
      return;

    case ExtendActionKind::UpdateTimer:
      // Fan stays on at current speed; just rearm the timer.
      arm_runtime_timer_(current_speed_, a.new_endpoint_ms);
      return;
  }
}

void FanController::set_runtime(const std::string &speed_str, int32_t minutes) {
  if (smart_mode_active_) deactivate_smart_mode();
  ::qc::Speed target;
  if (!::qc::FanControllerLogic::parse_speed_string(speed_str.c_str(), &target)) {
    ESP_LOGW(TAG, "set_runtime: unknown speed '%s' (expected off/low/med/high) — ignoring",
             speed_str.c_str());
    return;
  }

  const uint32_t now_ms = millis();
  auto action = ::qc::FanControllerLogic::compute_set_runtime_action(
      target, minutes, now_ms, max_run_ms_);
  ESP_LOGD(TAG, "set_runtime(%s, %d min): kind=%d target=%d endpoint=%u (now=%u)",
           speed_str.c_str(), minutes, (int)action.kind,
           (int)action.target_speed, action.new_endpoint_ms, now_ms);
  apply_set_runtime_action_(action);
}

void FanController::apply_set_runtime_action_(const ::qc::SetRuntimeAction &a) {
  using ::qc::SetRuntimeActionKind;
  switch (a.kind) {
    case SetRuntimeActionKind::NoOp:
      return;

    case SetRuntimeActionKind::TurnOff:
      force_off_();
      return;

    case SetRuntimeActionKind::SetSpeedIndefinite: {
      // Drive the target speed and cancel any running timer — pinning the fan
      // in manual mode regardless of prior state. Cancel BEFORE plan so the
      // publisher tick stops before the new speed publishes.
      cancel_runtime_timer_();
      auto plan = ::qc::FanControllerLogic::plan_speed_transition(
          current_speed_, a.target_speed, read_dip_(), effective_louver_pop_open_ms_());
      if (plan.empty()) {
        ESP_LOGW(TAG, "set_runtime: speed %d refused by DIP — leaving fan unchanged",
                 (int)a.target_speed);
        return;
      }
      start_plan_(std::move(plan));
      publish_mode_("Run");
      return;
    }

    case SetRuntimeActionKind::SetSpeedWithTimer:
      arm_runtime_timer_(a.target_speed, a.new_endpoint_ms);
      return;
  }
}

void FanController::arm_runtime_timer_(::qc::Speed target, uint32_t endpoint_ms) {
  // Set endpoint BEFORE the plan so apply_step_'s implicit save captures the
  // correct endpoint in one NVS write instead of two.
  runtime_endpoint_ms_ = endpoint_ms;
  // Only run the plan when the target differs from the current speed. For the
  // extend_runtime UpdateTimer path (fan stays at current speed) this avoids
  // an unnecessary relay-write cycle and HA publish_state churn. set_runtime
  // sends the explicit target, so a real change still goes through the plan.
  if (target != current_speed_) {
    auto plan = ::qc::FanControllerLogic::plan_speed_transition(
        current_speed_, target, read_dip_(), effective_louver_pop_open_ms_());
    if (plan.empty()) {
      ESP_LOGW(TAG, "arm_runtime_timer: speed %d refused by DIP — leaving fan off",
               (int)target);
      runtime_endpoint_ms_ = 0;
      return;
    }
    start_plan_(std::move(plan));
  } else {
    // No plan to run — apply_step_ won't fire, so save the endpoint ourselves.
    save_persisted_state_();
  }
  uint32_t remaining = (endpoint_ms > millis()) ? (endpoint_ms - millis()) : 0;
  this->set_timeout("runtime_stop", remaining, [this]() {
    ESP_LOGI(TAG, "Runtime timer expired — turning fan off");
    force_off_();
  });
  start_runtime_publisher_();
  publish_mode_("Timer");
}

void FanController::run_indefinitely() {
  cancel_runtime_timer_();
  publish_mode_("Run");
}

void FanController::publish_mode_(const char *mode) {
  if (!mode_select_ || syncing_mode_) return;
  if (mode_select_->current_option() == mode) return;
  syncing_mode_ = true;
  auto call = mode_select_->make_call();
  call.set_option(mode);
  call.perform();
  syncing_mode_ = false;
}

void FanController::cancel_runtime_timer_() {
  runtime_endpoint_ms_ = 0;
  this->cancel_timeout("runtime_stop");
  this->cancel_interval("runtime_tick");
  publish_runtime_remaining_();  // publishes 0
}

void FanController::force_off_() {
  // Canonical "cancel any timer and drive fan off" path. Used by runtime
  // expiry, restore-timer expiry, the watchdog, and the over-temp trip — all
  // safety/timer-driven shutdowns share this teardown.
  cancel_runtime_timer_();
  auto plan = ::qc::FanControllerLogic::plan_speed_transition(
      current_speed_, ::qc::Speed::Off, read_dip_(), 0);
  if (!plan.empty()) start_plan_(std::move(plan));
}

void FanController::start_runtime_publisher_() {
  // Idempotent — re-arming with the same name replaces the prior callback.
  this->set_interval("runtime_tick", 1000, [this]() { publish_runtime_remaining_(); });
}

void FanController::publish_runtime_remaining_() {
  if (runtime_remaining_sensor_ == nullptr) return;
  uint32_t remaining_s = 0;
  if (runtime_endpoint_ms_ != 0) {
    const uint32_t now_ms = millis();
    if (runtime_endpoint_ms_ > now_ms) {
      remaining_s = (runtime_endpoint_ms_ - now_ms + 999) / 1000;  // round up
    }
  }
  runtime_remaining_sensor_->publish_state(static_cast<float>(remaining_s / 60));
}

// NVS persistence helpers.
void FanController::save_persisted_state_() {
  if (restore_pending_) return;  // setup-phase guard — don't clobber pre-restore state
  if (effective_restore_mode_() == ::qc::RestoreMode::AlwaysOff) {
    // Feature off — keep NVS zeroed so a later flip-to-ON doesn't pick up
    // stale state from a previous session.
    PersistedState zero{0, 0};
    this->pref_.save(&zero);
    return;
  }
  PersistedState s{};
  s.speed = static_cast<uint8_t>(current_speed_);
  // Convert monotonic endpoint_ms -> wall-clock endpoint_unix_s if we have time.
  if (runtime_endpoint_ms_ != 0 && time_ != nullptr && time_->now().is_valid()) {
    const uint32_t now_ms = millis();
    if (runtime_endpoint_ms_ > now_ms) {
      const uint32_t remaining_s = (runtime_endpoint_ms_ - now_ms + 999) / 1000;
      s.endpoint_unix_s = static_cast<uint32_t>(time_->now().timestamp) + remaining_s;
    }
  }
  this->pref_.save(&s);
}

bool FanController::load_persisted_state_(PersistedState *out) {
  return this->pref_.load(out);
}

void FanController::schedule_restore_check_() {
  // Re-fire every 1s until we either succeed or hit the deadline.
  // Cheap polling — the deferred sequence runs for at most 30s after boot.
  this->set_timeout("restore_check", 1000, [this]() { attempt_restore_now_(); });
}

void FanController::attempt_restore_now_() {
  if (!restore_pending_) return;  // already done
  const bool deadline_hit = (int32_t)(millis() - restore_deadline_ms_) >= 0;
  const bool synced = (time_ != nullptr && time_->now().is_valid());

  PersistedState saved{};
  const bool have_saved = load_persisted_state_(&saved);
  if (!have_saved) {
    // First boot ever (no NVS entry yet). Nothing to restore.
    ESP_LOGI(TAG, "Restore: no saved state in NVS — cold-boot stays off");
    restore_pending_ = false;
    publish_mode_("Timer");
    return;
  }

  const ::qc::Speed saved_speed = static_cast<::qc::Speed>(saved.speed);
  const bool timer_was_running = (saved.endpoint_unix_s != 0);

  // If a timer was running but we still don't have time and the deadline
  // hasn't fired, keep waiting. Otherwise decide now.
  if (timer_was_running && !synced && !deadline_hit) {
    schedule_restore_check_();
    return;
  }

  // Compute the action via the pure-logic helper.
  const uint32_t now_unix = synced ? static_cast<uint32_t>(time_->now().timestamp) : 0;
  auto action = ::qc::FanControllerLogic::decide_restore_action(
      effective_restore_mode_(), saved_speed, saved.endpoint_unix_s, now_unix, synced);
  ESP_LOGI(TAG, "Restore decision: kind=%d speed=%d timer_ms=%u "
                "(saved.speed=%d saved.endpoint=%u now_unix=%u synced=%d deadline_hit=%d)",
           (int)action.kind, (int)action.resume_speed, action.timer_ms,
           (int)saved_speed, saved.endpoint_unix_s, now_unix,
           (int)synced, (int)deadline_hit);

  restore_pending_ = false;  // unblock save_persisted_state_ from now on

  // Smart Mode restore supersedes speed/timer restore — if Smart Mode was
  // active at shutdown, it will re-evaluate conditions and drive relays itself.
  uint8_t smart_saved = 0;
  if (this->smart_pref_.load(&smart_saved) && smart_saved == 1 &&
      effective_restore_mode_() == ::qc::RestoreMode::RestoreLastState) {
    ESP_LOGI(TAG, "Restoring Smart Mode from NVS (will engage after sensor stabilization)");
    smart_mode_active_ = true;
    // Mirror activate_smart_mode(): the fan entity reports ON whenever Smart
    // Mode is armed, so the restored state must publish ON too.
    this->state = true;
    this->publish_state();
    publish_mode_("Smart");
    publish_smart_status_(sensor_stabilized_ ? "Monitoring" : "Stabilizing");
    return;
  }

  switch (action.kind) {
    case ::qc::RestoreActionKind::StayOff:
      publish_mode_("Timer");
      return;
    case ::qc::RestoreActionKind::ResumeIndefinite: {
      auto plan = ::qc::FanControllerLogic::plan_speed_transition(
          current_speed_, action.resume_speed, read_dip_(),
          effective_louver_pop_open_ms_());
      if (plan.empty()) {
        ESP_LOGW(TAG, "Restore: resume speed %d refused by DIP — staying off",
                 (int)action.resume_speed);
        return;
      }
      start_plan_(std::move(plan));
      publish_mode_("Run");
      return;
    }
    case ::qc::RestoreActionKind::ResumeWithTimer: {
      // Use extend_runtime semantics with the persisted last_speed_ set so the
      // StartTimer branch resumes at the right speed. Bypass the api action
      // path because we want the EXACT timer_ms, not minute-rounded.
      auto plan = ::qc::FanControllerLogic::plan_speed_transition(
          current_speed_, action.resume_speed, read_dip_(),
          effective_louver_pop_open_ms_());
      if (plan.empty()) {
        ESP_LOGW(TAG, "Restore: resume speed %d refused by DIP — staying off",
                 (int)action.resume_speed);
        return;
      }
      start_plan_(std::move(plan));
      runtime_endpoint_ms_ = millis() + action.timer_ms;
      this->set_timeout("runtime_stop", action.timer_ms, [this]() {
        ESP_LOGI(TAG, "Runtime timer expired (restored) — turning fan off");
        force_off_();
      });
      start_runtime_publisher_();
      publish_mode_("Timer");
      return;
    }
  }
}

void FanController::dump_config() {
  ESP_LOGCONFIG(TAG, "QuietCool fan_controller:");
  ESP_LOGCONFIG(TAG, "  LOW relay:  %s", low_relay_  ? "OK" : "MISSING");
  ESP_LOGCONFIG(TAG, "  MED relay:  %s", med_relay_  ? "OK" : "MISSING");
  ESP_LOGCONFIG(TAG, "  HIGH relay: %s", high_relay_ ? "OK" : "MISSING");
  const char *dip_name = "None";
  switch (boot_dip_) {
    case ::qc::Dip::OneSpeed:   dip_name = "OneSpeed";   break;
    case ::qc::Dip::TwoSpeed:   dip_name = "TwoSpeed";   break;
    case ::qc::Dip::ThreeSpeed: dip_name = "ThreeSpeed"; break;
    case ::qc::Dip::None:       dip_name = "None";       break;
  }
  ESP_LOGCONFIG(TAG, "  Boot DIP: %s -> speed_count=%u", dip_name, configured_speed_count_);
  if (louver_pop_open_ms_ > 0) {
    ESP_LOGCONFIG(TAG, "  Louver pop-open: %u ms when enabled (gated by HA switch %s)",
                  louver_pop_open_ms_,
                  louver_pop_open_switch_ ? "OK" : "MISSING — always disabled");
  } else {
    ESP_LOGCONFIG(TAG, "  Louver pop-open: disabled at YAML level");
  }
  ESP_LOGCONFIG(TAG, "  Dry-run switch: %s",
                dry_run_switch_ ? "OK (relay writes gated by HA toggle)" : "MISSING — always live");
  ESP_LOGCONFIG(TAG, "  Max run: %u minutes (countdown timer cap)",
                max_run_ms_ / 60000u);
  ESP_LOGCONFIG(TAG, "  Default run number: %s (currently %u min)",
                default_run_number_ ? "OK" : "unset (auto-arm disabled)",
                effective_default_run_minutes_());
  ESP_LOGCONFIG(TAG, "  Runtime remaining sensor: %s",
                runtime_remaining_sensor_ ? "OK" : "MISSING — countdown is invisible to HA");
  ESP_LOGCONFIG(TAG, "  Restore mode switch: %s (current: %s)",
                restore_mode_switch_ ? "OK" : "MISSING — always behaves as ALWAYS_OFF",
                effective_restore_mode_() == ::qc::RestoreMode::RestoreLastState
                    ? "RESTORE_LAST_STATE" : "ALWAYS_OFF");
  ESP_LOGCONFIG(TAG, "  Time source: %s",
                time_ ? "OK (required for timer-restore)"
                      : "MISSING — timer-restore disabled (indefinite-resume still works)");
}

uint32_t FanController::effective_louver_pop_open_ms_() const {
  if (louver_pop_open_ms_ == 0) return 0;
  // No switch wired => behave as OFF (OEM-parity default).
  if (louver_pop_open_switch_ == nullptr) return 0;
  return louver_pop_open_switch_->state ? louver_pop_open_ms_ : 0;
}

bool FanController::dry_run_active_() const {
  return dry_run_switch_ != nullptr && dry_run_switch_->state;
}

uint32_t FanController::effective_default_run_minutes_() const {
  if (default_run_number_ == nullptr) return 0;  // not wired
  const float v = default_run_number_->state;
  if (!std::isfinite(v) || v <= 0.0f) return 0;  // NaN/Inf/<=0 = disabled
  // Clamp at the configured max_run_minutes so a user-typed value larger
  // than the cap doesn't immediately get saturated by compute_extend_action.
  const uint32_t minutes = static_cast<uint32_t>(v);
  const uint32_t cap_min = max_run_ms_ / 60000u;
  return (minutes > cap_min) ? cap_min : minutes;
}

fan::FanTraits FanController::get_traits() {
  // Speed/preset visibility is dynamic per the DIP wiring read in setup().
  // TwoSpeed   -> 2 steps (Low/High); ThreeSpeed -> 3; OneSpeed -> 1;
  // None       -> 0 steps + no presets (effectively just on/off, all refused).
  const bool has_speed = configured_speed_count_ > 0;
  auto traits = fan::FanTraits(/*oscillation=*/false, /*speed=*/has_speed,
                               /*direction=*/false,
                               /*speed_count=*/configured_speed_count_);
  // Wire the preset modes set in setup() into the traits. REQUIRED in
  // 2026.4+ — without this, the wire protocol does not forward
  // preset_mode commands and HA effectively ignores preset selection.
  this->wire_preset_modes_(traits);
  return traits;
}

::qc::Speed FanController::parse_call_(const fan::FanCall &call) const {
  // Flatten the ESPHome FanCall into a POD for the pure logic function. The
  // bare-turn_on path resumes `last_speed_` (the most recent non-Off speed)
  // rather than defaulting to High — matches HA's standard fan UX where
  // toggling off and back on restores the prior speed. Fully tested in
  // test_fan_controller_logic.cpp (resolve_target_speed tests).
  ::qc::FanControllerLogic::CallInputs in;
  in.has_state = call.get_state().has_value();
  in.state_value = in.has_state && call.get_state().value();
  in.preset_mode = call.get_preset_mode();
  in.has_speed = call.get_speed().has_value();
  in.speed_idx = in.has_speed ? call.get_speed().value() : 0;
  return ::qc::FanControllerLogic::resolve_target_speed(
      in, configured_speed_count_, last_speed_);
}

qc::Dip FanController::read_dip_() const {
  return boot_dip_;
}

void FanController::control(const fan::FanCall &call) {
  // Any HA call cancels Smart Mode (user is taking manual control).
  if (smart_mode_active_) {
    deactivate_smart_mode();
  }

  ::qc::Speed target = parse_call_(call);
  ::qc::Dip dip = read_dip_();
  const bool was_off = (current_speed_ == ::qc::Speed::Off);
  ::qc::Plan plan = ::qc::FanControllerLogic::plan_speed_transition(
      current_speed_, target, dip, effective_louver_pop_open_ms_());

  if (plan.empty()) {
    ESP_LOGW(TAG, "Refused: target=%d not allowed on DIP=%d (current=%d). "
                  "Ignoring HA call.", (int)target, (int)dip, (int)current_speed_);
    this->publish_state();
    return;
  }

  key1_cycle_counter_ = 0;
  start_plan_(std::move(plan));

  bool timer_mode = !mode_select_ || mode_select_->current_option() == "Timer";
  if (was_off && target != ::qc::Speed::Off && runtime_endpoint_ms_ == 0 && timer_mode) {
    const uint32_t default_minutes = effective_default_run_minutes_();
    if (default_minutes > 0) {
      ESP_LOGI(TAG, "fan.turn_on from off: auto-arming default %u-min timer",
               default_minutes);
      extend_runtime(static_cast<int32_t>(default_minutes));
    }
  }
}

void FanController::on_key1_press() {
  if (smart_mode_active_) deactivate_smart_mode();
  ::qc::Dip dip = read_dip_();
  uint8_t next = key1_cycle_counter_ + 1;
  auto r = ::qc::FanControllerLogic::cycle_next(next, dip);
  key1_cycle_counter_ = r.resets_counter ? 0 : next;

  ::qc::Plan plan = ::qc::FanControllerLogic::plan_speed_transition(
      current_speed_, r.target, dip, effective_louver_pop_open_ms_());
  if (plan.empty()) {
    // Should only happen on Dip::None where every press => Off (allowed) so
    // this branch is essentially unreachable; log if we hit it anyway.
    ESP_LOGW(TAG, "Button cycle produced refused transition (target=%d DIP=%d)",
             (int)r.target, (int)dip);
    return;
  }
  ESP_LOGD(TAG, "KEY1 cycle: counter=%u -> %s%s", next,
           r.target == ::qc::Speed::Off ? "Off" :
           r.target == ::qc::Speed::Low ? "Low" :
           r.target == ::qc::Speed::Med ? "Med" : "High",
           r.resets_counter ? " (cycle complete, counter reset)" : "");
  start_plan_(std::move(plan));
}

bool FanController::dispatch_pending_dual_gesture() {
  const uint32_t dual_held = gesture_.take_pending_dual_held_ms();
  if (dual_held == 0) return false;
  if (dual_held >= ::qc::DualGestureTracker::COMMIT_MS) {
    ESP_LOGW(TAG, "BOTH buttons %ums held: STOCK FIRMWARE RESTORE", dual_held);
    if (stock_restore_button_) stock_restore_button_->press();
  } else {
    ESP_LOGI(TAG, "BOTH buttons %ums held (released before %ums commit): aborted",
             dual_held, ::qc::DualGestureTracker::COMMIT_MS);
  }
  return true;
}

void FanController::start_plan_(::qc::Plan plan) {
  pending_plan_ = std::move(plan);
  pending_idx_ = 0;
  ++pending_seq_;  // invalidates any in-flight set_timeout callback
  ESP_LOGD(TAG, "start_plan: %u steps, seq=%u", (unsigned)pending_plan_.size(),
           pending_seq_);
  advance_plan_();
}

void FanController::advance_plan_() {
  if (pending_idx_ >= pending_plan_.size()) {
    ESP_LOGD(TAG, "advance_plan: idx=%u >= size=%u, nothing to do",
             (unsigned)pending_idx_, (unsigned)pending_plan_.size());
    return;
  }
  qc::PlanStep step = pending_plan_[pending_idx_];  // copy; pending_ may be freed below
  ESP_LOGD(TAG, "advance_plan: step %u/%u target=%d duration=%ums",
           (unsigned)pending_idx_ + 1, (unsigned)pending_plan_.size(),
           (int)step.target, step.duration_ms);
  apply_step_(step);
  pending_idx_++;

  if (step.duration_ms == 0) {
    // Terminal hold — plan complete.
    pending_plan_.clear();
    return;
  }

  // Schedule the next step. Capture the seq so a freshly-started plan
  // doesn't get clobbered by a stale timeout.
  uint32_t seq_snapshot = pending_seq_;
  ESP_LOGD(TAG, "scheduling next step in %ums (seq=%u)", step.duration_ms,
           seq_snapshot);
  this->set_timeout("plan_advance", step.duration_ms, [this, seq_snapshot]() {
    ESP_LOGD(TAG, "timeout fired: seq_snapshot=%u current_seq=%u", seq_snapshot,
             pending_seq_);
    if (seq_snapshot == pending_seq_) advance_plan_();
  });
}

void FanController::apply_step_(const ::qc::PlanStep &step) {
  auto rs = ::qc::FanControllerLogic::relays_for_speed(step.target);
  const bool dry_run = dry_run_active_();
  if (dry_run) {
    ESP_LOGD(TAG, "DRY RUN: would drive HIGH=%d MED=%d LOW=%d (target=%d) — relays NOT touched",
             (int)rs.high, (int)rs.med, (int)rs.low, (int)step.target);
  } else {
    // Matches the OEM GPIO write order: HIGH -> MED -> LOW. No inter-write delay.
    if (high_relay_) high_relay_->set_state(rs.high);
    if (med_relay_)  med_relay_->set_state(rs.med);
    if (low_relay_)  low_relay_->set_state(rs.low);
  }
  current_speed_ = step.target;
  // 24h watchdog: track continuous relay-on start time.
  if (step.target != ::qc::Speed::Off) {
    if (relay_on_start_ms_ == 0) relay_on_start_ms_ = millis();
  } else {
    relay_on_start_ms_ = 0;
  }
  if (step.target != ::qc::Speed::Off) {
    last_speed_ = step.target;
  } else {
    // Fan just went OFF — any runtime timer is now meaningless. Idempotent;
    // if the timer itself triggered this turn-off (force_off_), the cancels
    // are a no-op.
    cancel_runtime_timer_();
  }

  // Mirror into the HA fan entity state.
  // (preset_mode_ is private on fan::Fan and is only updated by incoming
  //  HA calls — we don't try to set it from the publish side. HA infers
  //  the active preset from the speed value.)
  // The 1-based `speed` index must be within [1, configured_speed_count_]
  // or HA computes percentage = speed * percentage_step out of range
  // (e.g. speed=3 with step=50 -> 150%). Mirror parse_call_'s mapping.
  //
  // On Off transition we intentionally LEAVE `this->speed` at the last
  // non-Off value (do NOT reset to 0). Reason: HA's fan card stores the
  // published percentage and re-sends it via fan.turn_on when the user
  // clicks the toggle to turn on. If we publish percentage=0 on off, HA
  // has nothing to restore and defaults to 100% — so "Low -> Off -> On"
  // would jump to High. Keeping the last percentage published lets HA
  // restore it correctly. The on/off display is driven by `state`, not
  // by `speed`, so the UI still shows "Off" while remembering the speed.
  this->state = smart_mode_active_ || (step.target != ::qc::Speed::Off);
  if (step.target != ::qc::Speed::Off) {
    this->speed = ::qc::FanControllerLogic::speed_to_ha_index(
        step.target, configured_speed_count_);
  }
  this->publish_state();

  if (fan_speed_sensor_) {
    const auto ref = (current_speed_ != ::qc::Speed::Off) ? current_speed_ : last_speed_;
    fan_speed_sensor_->publish_state(
        ::qc::FanControllerLogic::speed_label(ref, configured_speed_count_));
  }

  // Persist post-transition state so a reboot can resume correctly.
  // (No-op when restore_mode==AlwaysOff or while restore_pending_ is true.)
  save_persisted_state_();
}

// ============================================================================
// Smart Mode
// ============================================================================

void FanController::activate_smart_mode() {
  if (overtemp_tripped_ || watchdog_tripped_) {
    ESP_LOGW(TAG, "Smart Mode refused: %s tripped — reset first",
             overtemp_tripped_ ? "over-temp" : "watchdog");
    this->publish_state();
    return;
  }
  if (smart_mode_active_) return;
  ESP_LOGI(TAG, "Smart Mode: activating");
  cancel_runtime_timer_();
  smart_mode_active_ = true;
  this->state = true;
  this->publish_state();
  publish_mode_("Smart");
  publish_smart_status_(sensor_stabilized_ ? "Monitoring" : "Stabilizing");
  save_smart_mode_();
}

void FanController::deactivate_smart_mode() {
  if (!smart_mode_active_) return;
  ESP_LOGI(TAG, "Smart Mode: deactivating (manual override)");
  smart_mode_active_ = false;
  if (current_speed_ == ::qc::Speed::Off) {
    this->state = false;
    this->publish_state();
  }
  publish_mode_("Timer");
  publish_smart_status_("Off");
  save_smart_mode_();
}

void FanController::save_smart_mode_() {
  uint8_t val = smart_mode_active_ ? 1 : 0;
  this->smart_pref_.save(&val);
}

void FanController::trip_watchdog_(const char *reason) {
  ESP_LOGW(TAG, "WATCHDOG TRIPPED: %s — forcing OFF", reason);
  watchdog_tripped_ = true;
  smart_mode_active_ = false;
  force_off_();
  publish_mode_("Timer");
  publish_smart_status_("Watchdog tripped");
  save_smart_mode_();
  if (watchdog_sensor_) watchdog_sensor_->publish_state(true);
}

void FanController::reset_watchdog() {
  ESP_LOGI(TAG, "Watchdog reset by user");
  watchdog_tripped_ = false;
  overtemp_tripped_ = false;
  relay_on_start_ms_ = 0;
  if (watchdog_sensor_) watchdog_sensor_->publish_state(false);
  publish_smart_status_("Off");
}

bool FanController::is_threshold_enabled(::qc::SmartThreshold t) const {
  auto *sw = threshold_sw_[static_cast<size_t>(t)];
  return !sw || sw->state;
}

void FanController::set_threshold_enabled(::qc::SmartThreshold t, bool en) {
  auto *sw = threshold_sw_[static_cast<size_t>(t)];
  if (!sw) return;
  if (en) sw->turn_on(); else sw->turn_off();
}

void FanController::smart_tick() {
  if (temp_sensor_ == nullptr || humidity_sensor_ == nullptr) return;
  const float temp_c = temp_sensor_->state;
  const float hum = humidity_sensor_->state;
  const bool sensors_valid = std::isfinite(temp_c) && std::isfinite(hum);
  const uint32_t now = millis();

  if (sensors_valid) last_valid_sensor_ms_ = now;

  // --- Over-temp safety (all modes) ---
  if (sensors_valid && ::qc::FanControllerLogic::is_overtemp(temp_c)) {
    ESP_LOGW(TAG, "OVER-TEMP SAFETY: %.1f°C >= %.1f°C cutoff — forcing OFF",
             temp_c, ::qc::OVERTEMP_CUTOFF_C);
    overtemp_tripped_ = true;
    smart_mode_active_ = false;
    force_off_();
    publish_mode_("Timer");
    publish_smart_status_("Over-temp tripped");
    save_smart_mode_();
    if (watchdog_sensor_) watchdog_sensor_->publish_state(true);
    return;
  }

  // --- 24h continuous runtime watchdog (all modes) ---
  if (relay_on_start_ms_ > 0 &&
      (now - relay_on_start_ms_) >= ::qc::WATCHDOG_MAX_RUNTIME_MS) {
    trip_watchdog_("Continuous runtime exceeded 24h");
    return;
  }

  if (!smart_mode_active_) return;

  // --- 60s sensor stabilization gate (wrap-safe: flag latches once) ---
  if (!sensor_stabilized_) {
    if (now < ::qc::SENSOR_STABILIZE_MS) return;
    sensor_stabilized_ = true;
  }

  // --- Sensor stale watchdog (Smart Mode only, non-latching) ---
  if (last_valid_sensor_ms_ == 0 ||
      (now - last_valid_sensor_ms_) >= ::qc::SENSOR_STALE_MS) {
    if (current_speed_ != ::qc::Speed::Off) {
      ESP_LOGW(TAG, "Smart Mode: sensor stale >5min — suspending (relays off)");
      auto plan = ::qc::FanControllerLogic::plan_speed_transition(
          current_speed_, ::qc::Speed::Off, read_dip_(), 0);
      if (!plan.empty()) start_plan_(std::move(plan));
    }
    publish_smart_status_("Sensor stale");
    return;
  }

  // --- OEM latching: only evaluate the decision tree when the fan is off ---
  // Once Smart Mode starts the fan, it latches at that speed until an external
  // action (button, HA command, sensor stale, or over-temp) stops it. Matches
  // the OEM firmware, which only runs the decision tree while the fan is stopped.
  if (current_speed_ != ::qc::Speed::Off) {
    publish_smart_status_("Running");
    return;
  }

  publish_smart_status_("Monitoring");

  // --- Smart Mode decision tree ---
  auto cfg = build_smart_config_();
  ::qc::Speed target = ::qc::FanControllerLogic::compute_smart_speed(
      cfg, boot_dip_, temp_c, hum);

  if (target == ::qc::Speed::Off) return;

  ESP_LOGD(TAG, "Smart Mode: temp=%.1f°C hum=%.0f%% -> %s",
           temp_c, hum,
           target == ::qc::Speed::Low  ? "Low" :
           target == ::qc::Speed::Med  ? "Med" : "High");

  auto plan = ::qc::FanControllerLogic::plan_speed_transition(
      current_speed_, target, read_dip_(), effective_louver_pop_open_ms_());
  if (!plan.empty()) start_plan_(std::move(plan));
}

::qc::SmartConfig FanController::build_smart_config_() const {
  ::qc::SmartConfig cfg;
  if (smart_temp_high_ && std::isfinite(smart_temp_high_->state))
    cfg.temp_high_c = smart_temp_high_->state;
  if (smart_temp_med_ && std::isfinite(smart_temp_med_->state))
    cfg.temp_med_c = smart_temp_med_->state;
  if (smart_temp_low_ && std::isfinite(smart_temp_low_->state))
    cfg.temp_low_c = smart_temp_low_->state;
  if (smart_hum_high_ && std::isfinite(smart_hum_high_->state))
    cfg.hum_high_pct = smart_hum_high_->state;
  if (smart_hum_low_ && std::isfinite(smart_hum_low_->state))
    cfg.hum_low_pct = smart_hum_low_->state;
  cfg.hum_response = parse_hum_response_();
  cfg.temp_high_enabled = is_threshold_enabled(::qc::SmartThreshold::TempHigh);
  cfg.temp_med_enabled  = is_threshold_enabled(::qc::SmartThreshold::TempMed);
  cfg.temp_low_enabled  = is_threshold_enabled(::qc::SmartThreshold::TempLow);
  cfg.hum_high_enabled  = is_threshold_enabled(::qc::SmartThreshold::HumHigh);
  cfg.hum_low_enabled   = is_threshold_enabled(::qc::SmartThreshold::HumLow);
  return cfg;
}

::qc::Speed FanController::parse_hum_response_() const {
  if (smart_hum_response_ == nullptr) return ::qc::Speed::Low;
  const auto idx = smart_hum_response_->active_index();
  if (!idx.has_value()) return ::qc::Speed::Low;
  switch (idx.value()) {
    case 0: return ::qc::Speed::Off;     // "Off"
    case 1: return ::qc::Speed::Low;     // "Low"
    case 2: return ::qc::Speed::Med;     // "Medium"
    case 3: return ::qc::Speed::High;    // "High"
    default: return ::qc::Speed::Low;
  }
}

void FanController::publish_smart_status_(const char *status) {
  if (smart_mode_status_) smart_mode_status_->publish_state(status);
}

void FanController::import_oem_smart_thresholds_() {
  static constexpr uint32_t IMPORT_PREF_HASH = 0x71637468;  // 'qcth'
  static constexpr uint32_t IMPORT_VALUE_DONE = 0x54483032;  // 'TH02' (bumped: re-import with rounding fix)
  auto marker = global_preferences->make_preference<uint32_t>(IMPORT_PREF_HASH, true);
  uint32_t val = 0;
  if (marker.load(&val) && val == IMPORT_VALUE_DONE) return;

  ::qc::OemSmartThresholds oem;
  if (!OemNvsReader::read_smart_thresholds_from_nvs(&oem)) {
    ESP_LOGI(TAG, "OEM Smart thresholds: hx_list not readable — using defaults");
  } else if (!oem.any_valid()) {
    ESP_LOGI(TAG, "OEM Smart thresholds: no valid values in NVS — using defaults");
  } else {
    ESP_LOGI(TAG, "OEM Smart thresholds: importing from NVS");
    using Schema = ::qc::OemThresholdSchema;
    auto apply_temp = [this](int16_t raw, number::Number *n, ::qc::SmartThreshold which) {
      if (raw == Schema::TEMP_SENTINEL) { set_threshold_enabled(which, false); return; }
      if (n) n->make_call().set_value(::qc::threshold_f_to_c(static_cast<int>(raw))).perform();
    };
    auto apply_hum = [this](uint8_t raw, number::Number *n, ::qc::SmartThreshold which) {
      if (raw == Schema::HUM_SENTINEL) { set_threshold_enabled(which, false); return; }
      if (n) n->make_call().set_value(raw).perform();
    };
    apply_temp(oem.temp_high_f, smart_temp_high_, ::qc::SmartThreshold::TempHigh);
    apply_temp(oem.temp_med_f,  smart_temp_med_,  ::qc::SmartThreshold::TempMed);
    apply_temp(oem.temp_low_f,  smart_temp_low_,  ::qc::SmartThreshold::TempLow);
    apply_hum(oem.hum_high, smart_hum_high_, ::qc::SmartThreshold::HumHigh);
    apply_hum(oem.hum_low,  smart_hum_low_,  ::qc::SmartThreshold::HumLow);
    if (oem.hum_rank != Schema::HUM_SENTINEL && smart_hum_response_) {
      const char *opt = oem.hum_rank == 3 ? "High" :
                        oem.hum_rank == 2 ? "Medium" :
                        oem.hum_rank == 1 ? "Low" : "Off";
      smart_hum_response_->make_call().set_option(opt).perform();
    }
  }

  val = IMPORT_VALUE_DONE;
  marker.save(&val);
  global_preferences->sync();
}

}  // namespace quietcool
}  // namespace esphome
