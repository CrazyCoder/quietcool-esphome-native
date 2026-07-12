#include "http_flash_handler.h"
#include "http_flash_logic.h"
#include "esphome/core/log.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <esp_ota_ops.h>
#include <nvs.h>

namespace esphome {
namespace quietcool {

static const char *const TAG = "http_flash_handler";

// AsyncWebHandler subclass — lightweight wrapper that defers the actual
// flash to the parent Component (so the HTTP response can flush + the
// scheduler context lives in a Component).
class FlashUrlHandler : public AsyncWebHandler {
 public:
  explicit FlashUrlHandler(HttpFlashHandler *parent) : parent_(parent) {}

  bool canHandle(AsyncWebServerRequest *r) const override {
    // POST only. ESPHome's web_server already answers the CORS/PNA preflight for
    // every path (OPTIONS + Access-Control-Request-Private-Network header ->
    // handle_pna_cors_request, a clean 200). Claiming OPTIONS here would shadow
    // that with our own reply, so we deliberately don't. url_to() writes into a
    // caller-provided stack buffer (no heap alloc; the old url() is removed in
    // ESPHome 2026.9).
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    return r->method() == HTTP_POST && r->url_to(url_buf) == "/api/flash_url";
  }

  void handleRequest(AsyncWebServerRequest *r) override {
    const std::string url_arg = r->hasParam("url") ? r->arg("url") : "";
    const std::string md5_arg = r->hasParam("md5") ? r->arg("md5") : "";

    std::string url_t, md5_t;
    auto status = qc::HttpFlashLogic::validate(url_arg, md5_arg, &url_t, &md5_t);
    if (status != qc::FlashRequestStatus::Ok) {
      const char *msg = qc::HttpFlashLogic::status_message(status);
      ESP_LOGW(TAG, "Flash URL rejected: %s (url='%s' md5='%s')",
               msg, url_arg.c_str(), md5_arg.c_str());
      // Access-Control-Allow-Origin is added globally by web_server_base, so we
      // don't repeat it (a duplicated value makes browsers reject the response).
      // NB: ESPHome's IDF backend only maps 200/404/409 and renders any other
      // code as 500, so this 400 reaches the client as 500 — the body still
      // carries the reason and clients treat non-2xx as failure.
      r->send(400, "text/plain", msg);
      return;
    }

    ESP_LOGW(TAG, "HTTP flash accepted: url='%s' md5='%s' — fan will reboot",
             url_t.c_str(), md5_t.empty() ? "<url>.md5" : md5_t.c_str());

    // 200, not 202: the IDF backend renders unmapped codes as 500, which would
    // make a successful flash look like a server error to the caller.
    r->send(200, "text/plain", "Accepted - downloading firmware, device will reboot");

    parent_->schedule_flash(url_t, md5_t);
  }

 private:
  HttpFlashHandler *parent_;
};

void HttpFlashHandler::setup() {
  if (web_server_base::global_web_server_base == nullptr) {
    ESP_LOGE(TAG, "web_server_base not initialized — HTTP flash endpoint unavailable");
    return;
  }
  if (ota_ == nullptr) {
    ESP_LOGE(TAG, "ota.http_request component not wired — HTTP flash endpoint unavailable");
    return;
  }
  // Listen for OTA completion so request_rollback_confirm() can mark a
  // foreign-firmware slot valid before the reboot (see on_ota_state).
  ota_->add_state_listener(this);
  web_server_base::global_web_server_base->add_handler(new FlashUrlHandler(this));
  ESP_LOGCONFIG(TAG, "Registered POST /api/flash_url");
}

void HttpFlashHandler::on_ota_state(ota::OTAState state, float progress, uint8_t error) {
  (void) progress;
  (void) error;
  switch (state) {
    case ota::OTA_COMPLETED:
      if (this->confirm_after_flash_) {
        this->confirm_after_flash_ = false;
        // backend->end() has already called esp_ota_set_boot_partition() on the
        // just-written slot, so it is now the ACTIVE otadata entry (state NEW).
        // esp_ota_mark_app_valid_cancel_rollback() operates on the active entry
        // (not specifically the running partition), so this flips the NEW slot
        // to VALID. The bootloader then boots it as an ordinary image with no
        // pending-verify — foreign firmware that can't self-confirm stays put
        // instead of rolling back to us. Runs just before App.safe_reboot().
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
          ESP_LOGW(TAG, "Confirmed new firmware slot VALID — app-rollback will keep it");
        } else {
          ESP_LOGE(TAG, "Could not confirm new slot (err=0x%X); it may roll back to us", err);
        }
      }
      break;
    case ota::OTA_ERROR:
    case ota::OTA_ABORT:
      // Flash failed → no reboot happens. Disarm so a later, unrelated OTA on
      // this component isn't wrongly stripped of its rollback protection.
      this->confirm_after_flash_ = false;
      this->erase_esphome_on_powerdown_ = false;
      break;
    default:
      break;
  }
}

void HttpFlashHandler::on_powerdown() {
  if (!this->erase_esphome_on_powerdown_) return;
  this->erase_esphome_on_powerdown_ = false;

  // The stock and ESPHome applications share the OEM 16 KB NVS partition.
  // Remove only ESPHome's private namespace; nvs_flash_erase() would destroy
  // every OEM namespace, including hx_list pair IDs and user settings.
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open("esphome", NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
  }

  if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "Erased ESPHome NVS namespace; preserved OEM pairings and settings");
  } else {
    // Failure here is non-fatal: leaving ESPHome keys as stock-ignored dead
    // weight is safer than broadening the erase and losing OEM state.
    ESP_LOGE(TAG, "Could not erase ESPHome NVS namespace (err=0x%X); OEM state preserved", err);
  }
}

void HttpFlashHandler::dump_config() {
  ESP_LOGCONFIG(TAG, "QuietCool HTTP flash handler:");
  ESP_LOGCONFIG(TAG, "  Endpoint: POST /api/flash_url?url=<url>&md5=<32hex>");
  ESP_LOGCONFIG(TAG, "  web_server_base:  %s",
                web_server_base::global_web_server_base ? "OK" : "MISSING");
  ESP_LOGCONFIG(TAG, "  ota.http_request: %s", ota_ ? "OK" : "MISSING");
}

void HttpFlashHandler::schedule_flash(const std::string &url, const std::string &md5) {
  // Capture url+md5 by value — the request object is gone by the time this fires.
  this->set_timeout("flash", 500, [this, url, md5]() {
    ota_->set_url(url);
    // ota.http_request mandates a checksum (no true skip). An explicit md5 wins;
    // otherwise fetch it from the companion "<url>.md5" file the host serves.
    if (!md5.empty())
      ota_->set_md5(md5);
    else
      ota_->set_md5_url(url + ".md5");
    ota_->flash();  // does not return — reboots into new firmware
  });
}

}  // namespace quietcool
}  // namespace esphome
