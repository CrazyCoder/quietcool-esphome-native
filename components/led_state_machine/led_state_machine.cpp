#include "led_state_machine.h"
#include "../fan_controller/fan_controller.h"
#include "esphome/components/esp32_improv/esp32_improv_component.h"
#include "esphome/core/log.h"

namespace esphome {
namespace quietcool {

static const char *const TAG = "led_state_machine";

void LedStateMachine::setup() {
  // Start all LEDs off so a missing tick doesn't leave them in an
  // indeterminate state.
  if (led2_) led2_->set_state(false);
  if (led3_) led3_->set_state(false);
  if (led4_) led4_->set_state(false);
  // 10 Hz tick — fast enough to render the 50ms VeryFast pattern smoothly,
  // slow enough to be negligible CPU.
  this->set_interval("led_tick", 100, [this]() { this->tick_(); });
}

void LedStateMachine::dump_config() {
  ESP_LOGCONFIG(TAG, "LED state machine:");
  ESP_LOGCONFIG(TAG, "  LED2 (mode):     %s", led2_ ? "OK" : "MISSING");
  ESP_LOGCONFIG(TAG, "  LED3 (speed):    %s", led3_ ? "OK" : "MISSING");
  ESP_LOGCONFIG(TAG, "  LED4 (BLE):      %s", led4_ ? "OK" : "MISSING");
  ESP_LOGCONFIG(TAG, "  Fan controller:  %s", fan_  ? "OK" : "MISSING");
  ESP_LOGCONFIG(TAG, "  Improv-BLE:      %s",
                improv_ ? "OK (LED4 VeryFast while advertising)"
                        : "unset (LED4 ignores Improv state)");
}

void LedStateMachine::tick_() {
  ::qc::LedInput in{};
  if (fan_) {
    in.fan_on        = fan_->fan_is_on();
    in.timer_running = fan_->timer_is_running();
    in.key1_held_ms  = fan_->key1_held_ms();
    in.key2_held_ms  = fan_->key2_held_ms();
    in.dual_held_ms  = fan_->dual_held_ms();
    in.dip_invalid   = fan_->dip_is_invalid();
  }
  in.improv_advertising = (improv_ != nullptr && improv_->is_active());
  in.pair_mode_active = (pair_mode_switch_ != nullptr && pair_mode_switch_->state);

  auto state = ::qc::LedStateMachineLogic::compute_led_state(in);
  apply_(state, millis());
}

void LedStateMachine::apply_(const ::qc::LedState &state, uint32_t now_ms) {
  using ::qc::LedStateMachineLogic;
  if (led2_) led2_->set_state(LedStateMachineLogic::pattern_is_on(state.led2, now_ms));
  if (led3_) led3_->set_state(LedStateMachineLogic::pattern_is_on(state.led3, now_ms));
  if (led4_) led4_->set_state(LedStateMachineLogic::pattern_is_on(state.led4, now_ms));
}

}  // namespace quietcool
}  // namespace esphome
