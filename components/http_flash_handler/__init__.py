# HttpFlashHandler external component — registers URL-download and local-file
# firmware endpoints on the device web server. The latter is a version-neutral
# OEM restore path that validates a streamed OTA application before deliberately
# confirming the foreign slot and preserving OEM NVS state.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.components.http_request.ota import OtaHttpRequestComponent
from esphome.const import CONF_ID

CODEOWNERS = ["@CrazyCoder"]
DEPENDENCIES = ["web_server_base", "http_request"]

quietcool_ns = cg.esphome_ns.namespace("quietcool")
HttpFlashHandler = quietcool_ns.class_("HttpFlashHandler", cg.Component)

CONF_OTA_ID = "ota_id"
CONF_WEB_SERVER_ID = "web_server_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HttpFlashHandler),
        cv.GenerateID(CONF_WEB_SERVER_ID): cv.use_id(web_server_base.WebServerBase),
        cv.Required(CONF_OTA_ID): cv.use_id(OtaHttpRequestComponent),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    ota = await cg.get_variable(config[CONF_OTA_ID])
    cg.add(var.set_ota_component(ota))
    # The flash/restore endpoints are registered on the web server directly, so
    # they must run through its credential check: on a hub with web_server auth
    # configured they must not remain an open reflash path on the LAN. Passing the
    # web server to the handler lets it require the same credentials as the rest
    # of the UI.
    web_server = await cg.get_variable(config[CONF_WEB_SERVER_ID])
    cg.add(var.set_web_server(web_server))
    # We register an OTA state listener on the http_request component so we can
    # mark a freshly-flashed FOREIGN-firmware slot valid before the reboot (see
    # request_stock_restore_finalize / on_ota_state). The listener API + the
    # notify_state_() calls in the http_request OTA are both gated behind this
    # define, and nothing else in this config turns it on, so declare it here.
    cg.add_define("USE_OTA_STATE_LISTENER")
