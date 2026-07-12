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
#include "esphome/components/ota/ota_backend.h"

namespace esphome {
namespace quietcool {

class HttpFlashHandler : public Component, public ota::OTAStateListener {
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

  // Arm a one-shot: when the NEXT OTA on the wired http_request component
  // completes, mark the freshly-written slot VALID before the reboot. This is
  // required only when flashing FOREIGN firmware (e.g. stock QuietCool V4.1)
  // that never calls esp_ota_mark_app_valid_cancel_rollback() for itself.
  // Without it, ESP-IDF app-rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y)
  // boots the new image in pending-verify state and reverts to THIS firmware on
  // the next reset. Our own ESPHome OTAs must NOT arm this — they self-confirm
  // on boot and should keep the rollback safety net.
  void request_rollback_confirm() { confirm_after_flash_ = true; }

  // OTAStateListener. Fires synchronously from OtaHttpRequestComponent::flash()
  // (main-loop context) — on OTA_COMPLETED it runs just before App.safe_reboot(),
  // which is our window to rewrite otadata in time.
  void on_ota_state(ota::OTAState state, float progress, uint8_t error) override;

 protected:
  http_request::OtaHttpRequestComponent *ota_ = nullptr;
  // One-shot flag set by request_rollback_confirm(); consumed/cleared in
  // on_ota_state(). Default false so ordinary OTAs keep app-rollback.
  bool confirm_after_flash_ = false;
};

}  // namespace quietcool
}  // namespace esphome
