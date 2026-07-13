// HttpFlashHandler — registers two custom endpoints on the device web server:
//   POST /api/flash_url         download a firmware URL through http_request
//   POST /api/restore_stock_file upload a user-supplied OEM app image
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

#include <functional>
#include <utility>

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
  void schedule_flash(const std::string &url, const std::string &md5,
                      bool stock_restore = false);

  // Arm a one-shot: when the NEXT OTA on the wired http_request component
  // completes, mark the freshly-written slot VALID before the reboot. This is
  // required only when flashing FOREIGN firmware (e.g. stock QuietCool V4.1)
  // that never calls esp_ota_mark_app_valid_cancel_rollback() for itself.
  // Without it, ESP-IDF app-rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y)
  // boots the new image in pending-verify state and reverts to THIS firmware on
  // the next reset. Our own ESPHome OTAs must NOT arm this — they self-confirm
  // on boot and should keep the rollback safety net.
  void request_rollback_confirm() { confirm_after_flash_ = true; }

  // Arm all finalization needed for a deliberate restore to stock firmware:
  // confirm the foreign app slot and, immediately before reboot, erase only
  // ESPHome's own NVS namespace. OEM namespaces (especially hx_list, which
  // holds pair IDs, presets, thresholds, and fan metadata) must survive.
  void request_stock_restore_finalize() {
    confirm_after_flash_ = true;
    erase_esphome_on_powerdown_ = true;
  }

  // Factory-reset replacement-firmware state and Wi-Fi while preserving OEM
  // hx_list pairings, fan metadata, thresholds, and presets.
  void request_factory_reset_finalize() {
    erase_esphome_on_powerdown_ = true;
    erase_wifi_on_powerdown_ = true;
  }

  // OTAStateListener. Fires synchronously from OtaHttpRequestComponent::flash()
  // (main-loop context) — on OTA_COMPLETED it runs just before App.safe_reboot(),
  // which is our window to rewrite otadata in time.
  void on_ota_state(ota::OTAState state, float progress, uint8_t error) override;
  void on_powerdown() override;

  // Called after a local stock upload has passed structural checks and the
  // ESP-IDF OTA backend has verified and selected it. Unlike on_ota_state(),
  // this path owns its OTA backend and therefore finalizes synchronously.
  bool finalize_uploaded_stock();
  bool cancel_uploaded_stock();
  void schedule_uploaded_stock_reboot();
  void schedule_stock_upload_timeout(std::function<void()> &&callback) {
    this->set_timeout("stock-file-upload-timeout", 60000, std::move(callback));
  }

 protected:
  bool confirm_new_firmware_slot_();
  http_request::OtaHttpRequestComponent *ota_ = nullptr;
  // One-shot flag set by request_rollback_confirm(); consumed/cleared in
  // on_ota_state(). Default false so ordinary OTAs keep app-rollback.
  bool confirm_after_flash_ = false;
  // One-shot flag used only by request_stock_restore_finalize(). Erasing in
  // on_powerdown() puts this after ESPHome's normal shutdown preference sync,
  // so no pending preference writes can recreate the namespace before reboot.
  bool erase_esphome_on_powerdown_ = false;
  bool erase_wifi_on_powerdown_ = false;
};

}  // namespace quietcool
}  // namespace esphome
