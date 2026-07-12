"""ESPHome top-level component schema for the QuietCool LED state machine.

Owns three BinaryOutputs (LED2/3/4) and pulls live world state from the
fan_controller component + the Improv-BLE component. See
led_state_machine_logic.h for the decision matrix; tests live in
components/led_state_machine/test/.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import output, switch
from esphome.const import CONF_ID

CODEOWNERS = ["@CrazyCoder"]
DEPENDENCIES = ["output"]
AUTO_LOAD = []  # fan_controller is wired explicitly via fan_controller_id

quietcool_ns = cg.esphome_ns.namespace("quietcool")
LedStateMachine = quietcool_ns.class_("LedStateMachine", cg.Component)
FanController = quietcool_ns.class_("FanController")

esp32_improv_ns = cg.esphome_ns.namespace("esp32_improv")
ESP32ImprovComponent = esp32_improv_ns.class_("ESP32ImprovComponent")

CONF_LED2 = "led2"
CONF_LED3 = "led3"
CONF_LED4 = "led4"
CONF_FAN_CONTROLLER_ID = "fan_controller_id"
CONF_IMPROV_ID = "improv_id"
CONF_PAIR_MODE_SWITCH = "pair_mode_switch"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LedStateMachine),
        # 3 LEDs (active-low GPIOs declared elsewhere in YAML as output:gpio
        # with inverted: true). Owned exclusively by this component — nothing
        # else should drive the same pins.
        cv.Required(CONF_LED2): cv.use_id(output.BinaryOutput),
        cv.Required(CONF_LED3): cv.use_id(output.BinaryOutput),
        cv.Required(CONF_LED4): cv.use_id(output.BinaryOutput),
        # Reference to the fan_controller for live state queries (fan_on,
        # timer_running, key1/key2 held_ms). Required because LED2/3 are
        # meaningless without fan state.
        cv.Required(CONF_FAN_CONTROLLER_ID): cv.use_id(FanController),
        # Optional Improv-BLE component — when wired, LED4 goes VeryFast
        # while advertising. Without it, LED4 shows only pair-mode state
        # (SlowBlink) or Off.
        cv.Optional(CONF_IMPROV_ID): cv.use_id(ESP32ImprovComponent),
        cv.Optional(CONF_PAIR_MODE_SWITCH): cv.use_id(switch.Switch),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    led2 = await cg.get_variable(config[CONF_LED2])
    led3 = await cg.get_variable(config[CONF_LED3])
    led4 = await cg.get_variable(config[CONF_LED4])
    fc   = await cg.get_variable(config[CONF_FAN_CONTROLLER_ID])

    cg.add(var.set_led2(led2))
    cg.add(var.set_led3(led3))
    cg.add(var.set_led4(led4))
    cg.add(var.set_fan_controller(fc))

    if CONF_IMPROV_ID in config:
        improv = await cg.get_variable(config[CONF_IMPROV_ID])
        cg.add(var.set_improv_component(improv))

    if CONF_PAIR_MODE_SWITCH in config:
        pair_sw = await cg.get_variable(config[CONF_PAIR_MODE_SWITCH])
        cg.add(var.set_pair_mode_switch(pair_sw))
