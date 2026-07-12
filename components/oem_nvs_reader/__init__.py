"""ESPHome top-level component schema for the QuietCool OEM-NVS Wi-Fi importer.

Reads the ESP-IDF "nvs" namespace at boot (before WIFI setup) and, if found,
injects the OEM-stored Wi-Fi creds into ESPHome's WiFiComponent. Combined with
a YAML that omits wifi.ssid / wifi.password, this enables a single bundled
firmware.ota.bin that picks up the user's existing Wi-Fi on first boot
without needing per-user rebuilds.

Pure decision logic in oem_nvs_reader_logic.h; tests in test/.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@CrazyCoder"]
DEPENDENCIES = ["wifi"]

quietcool_ns = cg.esphome_ns.namespace("quietcool")
OemNvsReader = quietcool_ns.class_("OemNvsReader", cg.Component)

CONF_FACTORY_SSID_BLOCKLIST = "factory_ssid_blocklist"

# The HUAWEI factory-test creds (yaye@2025/) ship on EVERY QuietCool hub and
# persist post-OTA. Default the blocklist to skip them so a fresh / un-paired
# device falls through to Improv-BLE instead of trying to join a Wi-Fi that
# doesn't exist. Users with a *real* HUAWEI SSID can override the blocklist.
DEFAULT_BLOCKLIST = ["HUAWEI"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(OemNvsReader),
        cv.Optional(
            CONF_FACTORY_SSID_BLOCKLIST, default=DEFAULT_BLOCKLIST
        ): cv.ensure_list(cv.string_strict),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for ssid in config[CONF_FACTORY_SSID_BLOCKLIST]:
        cg.add(var.add_blocked_ssid(ssid))
