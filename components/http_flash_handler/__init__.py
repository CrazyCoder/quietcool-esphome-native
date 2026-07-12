# HttpFlashHandler external component — registers POST /api/flash_url on
# the device's web_server, calls the ota.http_request component to flash
# arbitrary URLs. Used by the Web Installer to push stock-firmware or
# custom builds onto a device already running our ESPHome firmware.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.http_request.ota import OtaHttpRequestComponent
from esphome.const import CONF_ID

CODEOWNERS = ["@CrazyCoder"]
DEPENDENCIES = ["web_server_base", "http_request"]

quietcool_ns = cg.esphome_ns.namespace("quietcool")
HttpFlashHandler = quietcool_ns.class_("HttpFlashHandler", cg.Component)

CONF_OTA_ID = "ota_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HttpFlashHandler),
        cv.Required(CONF_OTA_ID): cv.use_id(OtaHttpRequestComponent),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    ota = await cg.get_variable(config[CONF_OTA_ID])
    cg.add(var.set_ota_component(ota))
    # We register an OTA state listener on the http_request component so we can
    # mark a freshly-flashed FOREIGN-firmware slot valid before the reboot (see
    # request_stock_restore_finalize / on_ota_state). The listener API + the
    # notify_state_() calls in the http_request OTA are both gated behind this
    # define, and nothing else in this config turns it on, so declare it here.
    cg.add_define("USE_OTA_STATE_LISTENER")
