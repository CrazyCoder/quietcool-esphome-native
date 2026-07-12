// ESPHome wrapper around LedStateMachineLogic.
// Owns 3 BinaryOutput refs (LED2/3/4), pulls live world state from
// FanController + the Improv-BLE component, and drives the LEDs at 10 Hz.
//
// All decision logic lives in led_state_machine_logic.h — this file is glue.

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/output/binary_output.h"
#include "esphome/components/switch/switch.h"

#include "led_state_machine_logic.h"

namespace esphome {
namespace esp32_improv {
class ESP32ImprovComponent;  // forward decl; full include in .cpp
}  // namespace esp32_improv

namespace quietcool {

class FanController;  // forward decl; full include happens in .cpp

class LedStateMachine : public Component {
 public:
  void set_led2(output::BinaryOutput *p) { led2_ = p; }
  void set_led3(output::BinaryOutput *p) { led3_ = p; }
  void set_led4(output::BinaryOutput *p) { led4_ = p; }
  void set_fan_controller(FanController *fc) { fan_ = fc; }
  // Optional — when wired, LED4 goes VeryFast (10 Hz) while Improv-BLE is
  // currently advertising. When unset, the slot stays false and LED4 falls
  // back to pair-mode SlowBlink (or Off when idle).
  void set_improv_component(esp32_improv::ESP32ImprovComponent *c) { improv_ = c; }
  void set_pair_mode_switch(switch_::Switch *s) { pair_mode_switch_ = s; }

  void setup() override;
  void dump_config() override;
  // Run AFTER fan_controller (HARDWARE - 1.0f = 199). We're at HARDWARE - 2.0f
  // so the fan_controller pointer's state is guaranteed populated when we
  // first tick. Improv-BLE comes up later — we just poll it every tick and
  // tolerate it being absent.
  float get_setup_priority() const override { return setup_priority::HARDWARE - 2.0f; }

 protected:
  void tick_();
  void apply_(const ::qc::LedState &state, uint32_t now_ms);

  output::BinaryOutput *led2_ = nullptr;
  output::BinaryOutput *led3_ = nullptr;
  output::BinaryOutput *led4_ = nullptr;
  FanController *fan_ = nullptr;
  esp32_improv::ESP32ImprovComponent *improv_ = nullptr;
  switch_::Switch *pair_mode_switch_ = nullptr;
};

}  // namespace quietcool
}  // namespace esphome
