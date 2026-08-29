# OEM BLE compatibility component — reimplements the stock QuietCool V2 BLE
# protocol so the OEM Smart Control app can discover, pair with, and control our
# ESPHome firmware. Runtime-toggleable via an HA switch.
#
# Pure decision logic in oem_ble_compat_logic.h; tests in test/.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import (
    esp32_ble,
    number,
    select,
    sensor,
    switch,
    text,
    text_sensor,
)
from esphome.const import CONF_ID
from esphome.components.http_request.ota import OtaHttpRequestComponent

CODEOWNERS = ["@CrazyCoder"]
DEPENDENCIES = ["esp32_ble_server", "text"]

quietcool_ns = cg.esphome_ns.namespace("quietcool")
OemBleCompat = quietcool_ns.class_("OemBleCompat", cg.Component)
FanController = quietcool_ns.class_("FanController")

esp32_improv_ns = cg.esphome_ns.namespace("esp32_improv")
ESP32ImprovComponent = esp32_improv_ns.class_("ESP32ImprovComponent")

CONF_FAN_CONTROLLER_ID = "fan_controller_id"
CONF_ENABLE_SWITCH = "enable_switch"
CONF_IMPROV_ID = "improv_id"
CONF_PAIR_MODE_TIMEOUT = "pair_mode_timeout"
CONF_MAX_PAIR_IDS = "max_pair_ids"
CONF_TEMP_SENSOR = "temp_sensor"
CONF_HUMIDITY_SENSOR = "humidity_sensor"
CONF_SMART_TEMP_HIGH = "smart_temp_high"
CONF_SMART_TEMP_MED = "smart_temp_med"
CONF_SMART_TEMP_LOW = "smart_temp_low"
CONF_SMART_HUM_HIGH = "smart_hum_high"
CONF_SMART_HUM_LOW = "smart_hum_low"
CONF_SMART_HUM_RESPONSE = "smart_hum_response"
CONF_RUNTIME_REMAINING = "runtime_remaining_sensor"
CONF_DEFAULT_RUN_NUMBER = "default_run_minutes_number"
CONF_SMART_MODE_STATUS = "smart_mode_status"
CONF_PAIR_COUNT_SENSOR = "pair_count_sensor"
CONF_BLE_ACTIVE_CLIENTS_SENSOR = "ble_active_clients_sensor"
CONF_BLE_STACK_RESETS_SENSOR = "ble_stack_resets_sensor"
CONF_BLE_ADVERTISING_STATUS = "ble_advertising_status"
CONF_BLE_LAST_RESET_REASON = "ble_last_reset_reason"
CONF_FAN_NAME_TEXT = "fan_name_text"
CONF_FAN_MODEL_SELECT = "fan_model_select"
CONF_FAN_SERIAL_TEXT = "fan_serial_text"
CONF_BLE_MAC_SENSOR = "ble_mac_sensor"
CONF_PAIR_MODE_SWITCH = "pair_mode_switch"
CONF_PRESET_SELECT = "preset_select"
CONF_OTA_ID = "ota_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(OemBleCompat),
        cv.GenerateID(esp32_ble.CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
        cv.Required(CONF_FAN_CONTROLLER_ID): cv.use_id(FanController),
        cv.Optional(CONF_ENABLE_SWITCH): cv.use_id(switch.Switch),
        # Optional Improv-BLE component. When wired, the OEM BLE service
        # suspends while Improv advertises (advertising-packet contention)
        # and resumes when Improv goes idle.
        cv.Optional(CONF_IMPROV_ID): cv.use_id(ESP32ImprovComponent),
        cv.Optional(CONF_PAIR_MODE_TIMEOUT, default=120): cv.positive_int,
        # 50 is a hard ceiling, not a preference: the OEM hx_list namespace only
        # has Phone1..Phone50 slots and MAX_PAIR_SLOTS in the .cpp bounds every
        # scan/append against it. A larger value here would silently do nothing.
        cv.Optional(CONF_MAX_PAIR_IDS, default=50): cv.int_range(min=1, max=50),
        cv.Optional(CONF_TEMP_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_HUMIDITY_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_SMART_TEMP_HIGH): cv.use_id(number.Number),
        cv.Optional(CONF_SMART_TEMP_MED): cv.use_id(number.Number),
        cv.Optional(CONF_SMART_TEMP_LOW): cv.use_id(number.Number),
        cv.Optional(CONF_SMART_HUM_HIGH): cv.use_id(number.Number),
        cv.Optional(CONF_SMART_HUM_LOW): cv.use_id(number.Number),
        cv.Optional(CONF_SMART_HUM_RESPONSE): cv.use_id(select.Select),
        cv.Optional(CONF_RUNTIME_REMAINING): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_DEFAULT_RUN_NUMBER): cv.use_id(number.Number),
        cv.Optional(CONF_SMART_MODE_STATUS): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_PAIR_COUNT_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_BLE_ACTIVE_CLIENTS_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_BLE_STACK_RESETS_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_BLE_ADVERTISING_STATUS): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_BLE_LAST_RESET_REASON): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_FAN_NAME_TEXT): cv.use_id(text.Text),
        cv.Optional(CONF_FAN_MODEL_SELECT): cv.use_id(select.Select),
        cv.Optional(CONF_FAN_SERIAL_TEXT): cv.use_id(text.Text),
        cv.Optional(CONF_BLE_MAC_SENSOR): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_PAIR_MODE_SWITCH): cv.use_id(switch.Switch),
        cv.Optional(CONF_PRESET_SELECT): cv.use_id(select.Select),
        # OTA engine the A=10 Upgrade handler drives to flash custom firmware
        # from a BLE-supplied URL (non-OEM domains only). Optional.
        cv.Optional(CONF_OTA_ID): cv.use_id(OtaHttpRequestComponent),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_OTA_STATE_LISTENER")

    # Re-assert our raw OEM advertising payload after every ESPHome advertising
    # restart (see OemBleCompat::gap_event_handler).
    ble_parent = await cg.get_variable(config[esp32_ble.CONF_BLE_ID])
    esp32_ble.register_gap_event_handler(ble_parent, var)
    # Retain conn_id -> peer-address mappings so stale ESPHome client entries
    # can be checked against Bluedroid without closing healthy idle links.
    esp32_ble.register_gatts_event_handler(ble_parent, var)

    fc = await cg.get_variable(config[CONF_FAN_CONTROLLER_ID])
    cg.add(var.set_fan_controller(fc))

    cg.add(var.set_pair_mode_timeout_s(config[CONF_PAIR_MODE_TIMEOUT]))
    cg.add(var.set_max_pair_ids(config[CONF_MAX_PAIR_IDS]))

    if CONF_IMPROV_ID in config:
        improv = await cg.get_variable(config[CONF_IMPROV_ID])
        cg.add(var.set_improv(improv))

    for conf_key, setter in [
        (CONF_ENABLE_SWITCH, "set_enable_switch"),
        (CONF_TEMP_SENSOR, "set_temp_sensor"),
        (CONF_HUMIDITY_SENSOR, "set_humidity_sensor"),
        (CONF_SMART_TEMP_HIGH, "set_smart_temp_high"),
        (CONF_SMART_TEMP_MED, "set_smart_temp_med"),
        (CONF_SMART_TEMP_LOW, "set_smart_temp_low"),
        (CONF_SMART_HUM_HIGH, "set_smart_hum_high"),
        (CONF_SMART_HUM_LOW, "set_smart_hum_low"),
        (CONF_SMART_HUM_RESPONSE, "set_smart_hum_response"),
        (CONF_RUNTIME_REMAINING, "set_runtime_remaining_sensor"),
        (CONF_DEFAULT_RUN_NUMBER, "set_default_run_number"),
        (CONF_SMART_MODE_STATUS, "set_smart_mode_status"),
        (CONF_PAIR_COUNT_SENSOR, "set_pair_count_sensor"),
        (CONF_BLE_ACTIVE_CLIENTS_SENSOR, "set_ble_active_clients_sensor"),
        (CONF_BLE_STACK_RESETS_SENSOR, "set_ble_stack_resets_sensor"),
        (CONF_BLE_ADVERTISING_STATUS, "set_ble_advertising_status"),
        (CONF_BLE_LAST_RESET_REASON, "set_ble_last_reset_reason"),
        (CONF_FAN_NAME_TEXT, "set_fan_name_text"),
        (CONF_FAN_MODEL_SELECT, "set_fan_model_select"),
        (CONF_FAN_SERIAL_TEXT, "set_fan_serial_text"),
        (CONF_BLE_MAC_SENSOR, "set_ble_mac_sensor"),
        (CONF_PAIR_MODE_SWITCH, "set_pair_mode_switch"),
        (CONF_PRESET_SELECT, "set_preset_select"),
        (CONF_OTA_ID, "set_ota_component"),
    ]:
        if conf_key in config:
            ref = await cg.get_variable(config[conf_key])
            cg.add(getattr(var, setter)(ref))
