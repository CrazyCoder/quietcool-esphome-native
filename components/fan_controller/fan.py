"""ESPHome external-component schema for the QuietCool fan_controller.

Owns the three speed relays (LOW/MED/HIGH BinaryOutputs), reads DIP wiring
from a text_sensor, exposes the HA fan entity, and (optionally) prepends a
brief HIGH pulse on Off->Low|Med transitions to assist mechanical louvers.

Decision logic lives in fan_controller_logic.h and is unit-tested on the
host — see components/fan_controller/test/.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, fan, number, output, select, sensor, switch, text_sensor, time as time_

CODEOWNERS = ["@CrazyCoder"]
DEPENDENCIES = ["binary_sensor", "button", "fan", "number", "output", "select", "sensor", "switch", "text_sensor"]

quietcool_ns = cg.esphome_ns.namespace("quietcool")
FanController = quietcool_ns.class_("FanController", cg.Component, fan.Fan)

CONF_LOW_RELAY = "low_relay"
CONF_MED_RELAY = "med_relay"
CONF_HIGH_RELAY = "high_relay"
CONF_DIP_A = "dip_a"
CONF_DIP_B = "dip_b"
CONF_DIP_C = "dip_c"
CONF_LOUVER_POP_OPEN_SECONDS = "louver_pop_open_seconds"
CONF_LOUVER_POP_OPEN_SWITCH = "louver_pop_open_switch"
CONF_DRY_RUN_SWITCH = "dry_run_switch"
CONF_DISABLE_WATCHDOGS_SWITCH = "disable_watchdogs_switch"
CONF_RUNTIME_REMAINING_SENSOR = "runtime_remaining_sensor"
CONF_MAX_RUN_MINUTES = "max_run_minutes"
CONF_RESTORE_MODE_SWITCH = "restore_mode_switch"
CONF_TIME_ID = "time_id"
CONF_DEFAULT_RUN_MINUTES_NUMBER = "default_run_minutes_number"
CONF_TEMP_SENSOR = "temp_sensor"
CONF_HUMIDITY_SENSOR = "humidity_sensor"
CONF_SMART_TEMP_HIGH = "smart_temp_high"
CONF_SMART_TEMP_MED = "smart_temp_med"
CONF_SMART_TEMP_LOW = "smart_temp_low"
CONF_SMART_HUM_HIGH = "smart_hum_high"
CONF_SMART_HUM_LOW = "smart_hum_low"
CONF_SMART_TEMP_HYST = "smart_temp_hyst"
CONF_SMART_HUM_HYST = "smart_hum_hyst"
CONF_SMART_HUM_RESPONSE = "smart_hum_response"
CONF_SMART_MODE_STATUS = "smart_mode_status"
CONF_FAN_SPEED_SENSOR = "fan_speed_sensor"
CONF_MODE_SELECT = "mode_select"
CONF_SMART_TEMP_HIGH_SWITCH = "smart_temp_high_switch"
CONF_SMART_TEMP_MED_SWITCH = "smart_temp_med_switch"
CONF_SMART_TEMP_LOW_SWITCH = "smart_temp_low_switch"
CONF_SMART_HUM_HIGH_SWITCH = "smart_hum_high_switch"
CONF_SMART_HUM_LOW_SWITCH = "smart_hum_low_switch"
CONF_WATCHDOG_SENSOR = "watchdog_sensor"
CONF_SPEEDS_AVAILABLE_SENSOR = "speeds_available_sensor"
CONF_STOCK_RESTORE_BUTTON = "stock_restore_button"

CONFIG_SCHEMA = (
    fan.fan_schema(FanController)
    .extend(
        {
            cv.Required(CONF_LOW_RELAY): cv.use_id(output.BinaryOutput),
            cv.Required(CONF_MED_RELAY): cv.use_id(output.BinaryOutput),
            cv.Required(CONF_HIGH_RELAY): cv.use_id(output.BinaryOutput),
            # The 3 DIP-bit binary sensors. Read in setup() to decide
            # how many speed steps / which preset_modes to expose to HA.
            # See the OEM DIP encoding truth table in the YAML comments.
            cv.Required(CONF_DIP_A): cv.use_id(binary_sensor.BinarySensor),
            cv.Required(CONF_DIP_B): cv.use_id(binary_sensor.BinarySensor),
            cv.Required(CONF_DIP_C): cv.use_id(binary_sensor.BinarySensor),
            # Duration of the HIGH pulse on Off -> Low|Med transitions WHEN the
            # runtime switch is enabled. 0 disables the feature entirely
            # (overrides the switch). See fan_controller_logic.h
            # plan_speed_transition() for the full skip rules.
            cv.Optional(CONF_LOUVER_POP_OPEN_SECONDS, default=5): cv.positive_int,
            # Optional runtime gate. The OEM stock controller has no louver
            # pop-open, so for parity the switch should ship OFF
            # (RESTORE_DEFAULT_OFF on the YAML side). Users who want the
            # feature toggle it on in HA. Omit this option entirely to
            # hard-disable louver pop-open even when seconds > 0.
            cv.Optional(CONF_LOUVER_POP_OPEN_SWITCH): cv.use_id(switch.Switch),
            # Optional dry-run gate. When the referenced switch is ON, the
            # component still computes plans and publishes HA state but
            # suppresses actual GPIO writes to the relay outputs. Useful for
            # safely iterating firmware while the fan is physically wired.
            # Switch should ship RESTORE_DEFAULT_OFF (relays live by default).
            cv.Optional(CONF_DRY_RUN_SWITCH): cv.use_id(switch.Switch),
            # Optional safety override. When ON, all watchdogs are bypassed.
            # The referenced switch must default OFF so protection is enabled
            # when no restored preference exists.
            cv.Optional(CONF_DISABLE_WATCHDOGS_SWITCH): cv.use_id(switch.Switch),
            # Countdown timer.
            # Optional sensor that gets the remaining-seconds value once per
            # second while a timer is running, and 0 when idle. Omit if you
            # don't want the live countdown surfaced to HA.
            cv.Optional(CONF_RUNTIME_REMAINING_SENSOR): cv.use_id(sensor.Sensor),
            # Hard cap on how long a countdown timer can extend the runtime.
            # Default 1440 (24h) matches the OEM firmware's internal max.
            # extend_runtime() will saturate the endpoint at now+cap; this
            # prevents a buggy HA automation from leaving the fan on for days.
            cv.Optional(CONF_MAX_RUN_MINUTES, default=1440): cv.positive_int,
            # Reboot-resume policy, gated by an HA-toggleable switch.
            # Point at a `switch.template` entity; when ON, the fan persists its
            # last speed + wall-clock timer endpoint to NVS and resumes after a
            # controller reboot. When OFF (or unset), behavior matches OEM
            # cold-boot (everything starts off).
            # The switch should ship `restore_mode: RESTORE_DEFAULT_OFF` on the
            # YAML side so the safer default survives flashes. Timer-restore
            # additionally requires `time_id`; without it, only the speed is
            # resumed (any running timer is cancelled).
            cv.Optional(CONF_RESTORE_MODE_SWITCH): cv.use_id(switch.Switch),
            # Optional time source — used to compute remaining timer after
            # reboot via wall-clock arithmetic. Point this at a
            # `time.homeassistant` component if you want timer-restore to work.
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            # Optional HA-editable number entity holding the default run
            # duration in MINUTES, auto-armed on fan.turn_on from off.
            # User-settable from HA's Device Configuration page; persists
            # across reboots (the number itself should ship `restore_value:
            # true` + `entity_category: config`). Set to 0 in HA to disable
            # auto-arm and have full-manual control. When this option is
            # omitted entirely from YAML, auto-arm is disabled.
            cv.Optional(CONF_DEFAULT_RUN_MINUTES_NUMBER): cv.use_id(number.Number),
            # Smart Mode: sensor + threshold wiring
            cv.Optional(CONF_TEMP_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_HUMIDITY_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_SMART_TEMP_HIGH): cv.use_id(number.Number),
            cv.Optional(CONF_SMART_TEMP_MED): cv.use_id(number.Number),
            cv.Optional(CONF_SMART_TEMP_LOW): cv.use_id(number.Number),
            cv.Optional(CONF_SMART_HUM_HIGH): cv.use_id(number.Number),
            cv.Optional(CONF_SMART_HUM_LOW): cv.use_id(number.Number),
            cv.Optional(CONF_SMART_TEMP_HYST): cv.use_id(number.Number),
            cv.Optional(CONF_SMART_HUM_HYST): cv.use_id(number.Number),
            cv.Optional(CONF_MODE_SELECT): cv.use_id(select.Select),
            cv.Optional(CONF_SMART_TEMP_HIGH_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_SMART_TEMP_MED_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_SMART_TEMP_LOW_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_SMART_HUM_HIGH_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_SMART_HUM_LOW_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_SMART_HUM_RESPONSE): cv.use_id(select.Select),
            cv.Optional(CONF_SMART_MODE_STATUS): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_FAN_SPEED_SENSOR): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_WATCHDOG_SENSOR): cv.use_id(binary_sensor.BinarySensor),
            # Optional diagnostic — receives the DIP-decoded speed-tap count
            # (1/2/3, or 0 for an invalid wiring) once at boot.
            cv.Optional(CONF_SPEEDS_AVAILABLE_SENSOR): cv.use_id(sensor.Sensor),
            # Button pressed by dispatch_pending_dual_gesture() when the
            # KEY1+KEY2 dual hold crosses COMMIT_MS (10s) — typically the
            # "Restore Stock Firmware" template button. Without this slot,
            # the commit log fires but no button is pressed (defensive
            # no-op rather than a crash).
            cv.Optional(CONF_STOCK_RESTORE_BUTTON): cv.use_id(button.Button),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await fan.new_fan(config)
    await cg.register_component(var, config)

    low  = await cg.get_variable(config[CONF_LOW_RELAY])
    med  = await cg.get_variable(config[CONF_MED_RELAY])
    high = await cg.get_variable(config[CONF_HIGH_RELAY])
    da   = await cg.get_variable(config[CONF_DIP_A])
    db   = await cg.get_variable(config[CONF_DIP_B])
    dc   = await cg.get_variable(config[CONF_DIP_C])

    cg.add(var.set_low_relay(low))
    cg.add(var.set_med_relay(med))
    cg.add(var.set_high_relay(high))
    cg.add(var.set_dip_a(da))
    cg.add(var.set_dip_b(db))
    cg.add(var.set_dip_c(dc))
    cg.add(var.set_louver_pop_open_ms(config[CONF_LOUVER_POP_OPEN_SECONDS] * 1000))

    if CONF_LOUVER_POP_OPEN_SWITCH in config:
        sw = await cg.get_variable(config[CONF_LOUVER_POP_OPEN_SWITCH])
        cg.add(var.set_louver_pop_open_switch(sw))

    if CONF_DRY_RUN_SWITCH in config:
        sw = await cg.get_variable(config[CONF_DRY_RUN_SWITCH])
        cg.add(var.set_dry_run_switch(sw))

    if CONF_DISABLE_WATCHDOGS_SWITCH in config:
        sw = await cg.get_variable(config[CONF_DISABLE_WATCHDOGS_SWITCH])
        cg.add(var.set_disable_watchdogs_switch(sw))

    if CONF_RUNTIME_REMAINING_SENSOR in config:
        s = await cg.get_variable(config[CONF_RUNTIME_REMAINING_SENSOR])
        cg.add(var.set_runtime_remaining_sensor(s))

    cg.add(var.set_max_run_ms(config[CONF_MAX_RUN_MINUTES] * 60 * 1000))

    if CONF_RESTORE_MODE_SWITCH in config:
        sw = await cg.get_variable(config[CONF_RESTORE_MODE_SWITCH])
        cg.add(var.set_restore_mode_switch(sw))

    if CONF_TIME_ID in config:
        t = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(t))

    if CONF_DEFAULT_RUN_MINUTES_NUMBER in config:
        n = await cg.get_variable(config[CONF_DEFAULT_RUN_MINUTES_NUMBER])
        cg.add(var.set_default_run_number(n))

    for conf_key, setter in [
        (CONF_TEMP_SENSOR, "set_temp_sensor"),
        (CONF_HUMIDITY_SENSOR, "set_humidity_sensor"),
        (CONF_SMART_TEMP_HIGH, "set_smart_temp_high"),
        (CONF_SMART_TEMP_MED, "set_smart_temp_med"),
        (CONF_SMART_TEMP_LOW, "set_smart_temp_low"),
        (CONF_SMART_HUM_HIGH, "set_smart_hum_high"),
        (CONF_SMART_HUM_LOW, "set_smart_hum_low"),
        (CONF_SMART_TEMP_HYST, "set_smart_temp_hyst"),
        (CONF_SMART_HUM_HYST, "set_smart_hum_hyst"),
        (CONF_MODE_SELECT, "set_mode_select"),
        (CONF_SMART_TEMP_HIGH_SWITCH, "set_smart_temp_high_switch"),
        (CONF_SMART_TEMP_MED_SWITCH, "set_smart_temp_med_switch"),
        (CONF_SMART_TEMP_LOW_SWITCH, "set_smart_temp_low_switch"),
        (CONF_SMART_HUM_HIGH_SWITCH, "set_smart_hum_high_switch"),
        (CONF_SMART_HUM_LOW_SWITCH, "set_smart_hum_low_switch"),
        (CONF_SMART_HUM_RESPONSE, "set_smart_hum_response"),
        (CONF_SMART_MODE_STATUS, "set_smart_mode_status"),
        (CONF_FAN_SPEED_SENSOR, "set_fan_speed_sensor"),
        (CONF_WATCHDOG_SENSOR, "set_watchdog_sensor"),
        (CONF_SPEEDS_AVAILABLE_SENSOR, "set_speeds_available_sensor"),
        (CONF_STOCK_RESTORE_BUTTON, "set_stock_restore_button"),
    ]:
        if conf_key in config:
            v = await cg.get_variable(config[conf_key])
            cg.add(getattr(var, setter)(v))
