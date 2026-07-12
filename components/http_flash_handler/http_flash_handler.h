// HttpFlashHandler — registers a custom POST /api/flash_url endpoint on
// the device's web_server. Lets a browser (or curl) hand the device an
// arbitrary firmware URL + optional MD5 to flash via the existing
// ota.platform: http_request component.
//
// Wiring contract (set in YAML via this component's Python schema):
//   - `web_server_base` must be enabled (top-level web_server: { ... }).
//   - `ota.platform: http_request` entry must exist with an `id:`, passed
//     to us via `ota_id:` so we can call its set_url/set_md5/flash methods.
//
// The validator that gates the inputs lives in http_flash_logic.h and is
// host-side unit-tested.

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/http_request/ota/ota_http_request.h"

namespace esphome {
namespace quietcool {

class HttpFlashHandler : public Component {
 public:
  void set_ota_component(http_request::OtaHttpRequestComponent *ota) { ota_ = ota; }

  void setup() override;
  void dump_config() override;
  // Run AFTER_WIFI so the web_server has had a chance to start (web_server
  // setup_priority is AFTER_WIFI too; the global_web_server_base pointer is
  // set during web_server_base's setup which fires earlier). Either way we
  // just call add_handler — no harm if it lands before web_server's first
  // request loop.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Called from the AsyncWebHandler subclass when a valid request lands.
  // Schedules the flash for ~500ms in the future so the HTTP 200 response
  // has time to flush before the OTA reboots the device.
  void schedule_flash(const std::string &url, const std::string &md5);

 protected:
  http_request::OtaHttpRequestComponent *ota_ = nullptr;
};

}  // namespace quietcool
}  // namespace esphome
