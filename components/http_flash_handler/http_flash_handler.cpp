#include "http_flash_handler.h"
#include "http_flash_logic.h"
#include "stock_upload_logic.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/ota/ota_backend_factory.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <esp_ota_ops.h>
#include <nvs.h>

#include <algorithm>
#include <cstring>

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

    const bool stock_restore = qc::HttpFlashLogic::is_known_stock_restore(url_t, md5_t);
    ESP_LOGW(TAG, "HTTP flash accepted: url='%s' md5='%s' stock_restore=%s — fan will reboot",
             url_t.c_str(), md5_t.empty() ? "<url>.md5" : md5_t.c_str(),
             stock_restore ? "yes" : "no");

    // 200, not 202: the IDF backend renders unmapped codes as 500, which would
    // make a successful flash look like a server error to the caller.
    r->send(200, "text/plain", "Accepted - downloading firmware, device will reboot");

    parent_->schedule_flash(url_t, md5_t, stock_restore);
  }

 private:
  HttpFlashHandler *parent_;
};

// Dedicated local-file restore path. It intentionally does not replace or
// intercept ESPHome's built-in /update route: normal replacement-firmware
// uploads must retain app rollback. This endpoint is an explicit assertion by
// the user that the file is OEM firmware and may therefore be made permanent.
class StockFileUploadHandler : public AsyncWebHandler {
 public:
  explicit StockFileUploadHandler(HttpFlashHandler *parent) : parent_(parent) {}

  bool canHandle(AsyncWebServerRequest *r) const override {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    const auto url = r->url_to(url_buf);
    return (r->method() == HTTP_GET && url == "/restore-stock") ||
           (r->method() == HTTP_POST && url == "/api/restore_stock_file");
  }

  // NOLINTNEXTLINE(readability-identifier-naming)
  bool isRequestHandlerTrivial() const override { return false; }

  void handleUpload(AsyncWebServerRequest *request, const std::string &filename,
                    size_t index, uint8_t *data, size_t len, bool final) override {
    if (index == 0 && len > 0) this->begin_upload_(request, filename);
    if (!upload_active_) {
      if (final && file_part_started_) file_part_finished_ = true;
      return;
    }

    if (len > 0) {
      if (inspector_.total_size() + len > partition_size_) {
        this->fail_(qc::StockUploadInspector::status_message(qc::StockUploadStatus::TooLarge));
        return;
      }

      size_t offset = 0;
      if (!backend_) {
        const size_t prefix_len = std::min(len, prefix_buffer_.size() - prefix_buffer_size_);
        memcpy(prefix_buffer_.data() + prefix_buffer_size_, data, prefix_len);
        prefix_buffer_size_ += prefix_len;
        inspector_.consume(data, prefix_len);
        offset = prefix_len;

        if (prefix_buffer_size_ == prefix_buffer_.size()) {
          const auto prefix_status = inspector_.validate_prefix();
          if (prefix_status != qc::StockUploadStatus::Ok) {
            this->fail_(qc::StockUploadInspector::status_message(prefix_status));
            return;
          }
          if (!this->start_backend_()) return;
        }
      }

      if (backend_ && offset < len) {
        inspector_.consume(data + offset, len - offset);
        const auto error = backend_->write(data + offset, len - offset);
        if (error != ota::OTA_RESPONSE_OK) {
          this->fail_("ESP-IDF rejected data while writing the inactive OTA slot");
          return;
        }
      }
      this->arm_timeout_();
    }

    if (!final || !upload_active_) return;
    file_part_finished_ = true;

    const auto status = inspector_.validate(partition_size_);
    if (status != qc::StockUploadStatus::Ok) {
      this->fail_(qc::StockUploadInspector::status_message(status));
      return;
    }

    const std::string project = inspector_.project_name();
    const bool marker_seen = inspector_.quietcool_marker_seen();
    const size_t image_size = inspector_.total_size();
    if (!backend_) {
      this->fail_("File is too short to initialize the OTA backend");
      return;
    }
    const auto error = backend_->end();
    backend_.reset();
    upload_active_ = false;
    if (error != ota::OTA_RESPONSE_OK) {
      result_message_ = "ESP-IDF image verification failed; the running firmware was not changed";
      ESP_LOGE(TAG, "Local OEM upload rejected by esp_ota_end (error=%u)", static_cast<unsigned>(error));
      return;
    }

    image_ready_ = true;
    uploaded_size_ = image_size;
    uploaded_project_ = project;
    uploaded_marker_seen_ = marker_seen;
    result_message_ = "OEM image verified; waiting for the complete upload request";
    this->arm_timeout_();
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    if (request->method() == HTTP_GET) {
      static const char PAGE[] = R"html(<!doctype html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>QuietCool OEM restore</title><style>
body{font:16px system-ui;max-width:680px;margin:30px auto;padding:0 16px;line-height:1.45}
input,button{font:inherit;margin:8px 0;padding:10px;max-width:100%}button{display:block}
.warning{border-left:4px solid #c60;padding:10px;background:#fff4df}
</style></head><body><h1>Restore QuietCool OEM firmware</h1>
<p>Upload an OEM IT-AF-SMT <strong>application .bin</strong> saved on this device.
This local path does not depend on QuietCool keeping any download URL online.</p>
<div class="warning"><strong>Permanent foreign-firmware restore:</strong> the uploaded
slot is marked valid and will not automatically roll back. Use only OEM firmware for
the QuietCool IT-AF-SMT—not a full-flash dump, bootloader, or ESPHome image.</div>
<form method="post" enctype="multipart/form-data"
 action="/api/restore_stock_file?confirm=RESTORE_OEM_FIRMWARE">
<p><input type="file" name="firmware" accept=".bin,application/octet-stream" required></p>
<label><input type="checkbox" required> I confirm this is OEM firmware for the
QuietCool IT-AF-SMT.</label>
<button type="submit">Upload and restore OEM firmware</button></form>
<p>The hub validates the ESP32 application structure, target chip, OTA-slot size,
and complete ESP-IDF image integrity before changing the boot partition. OEM pairings
and settings are preserved.</p></body></html>)html";
      request->send(200, "text/html", PAGE);
      return;
    }

    // The multipart parser has completed the entire request. Retire the
    // transfer timeout before confirming the new slot or sending the response.
    ++upload_generation_;
    bool should_reboot = false;
    if (image_ready_ && !multiple_files_) {
      if (parent_->finalize_uploaded_stock()) {
        upload_success_ = true;
        should_reboot = true;
        result_message_ = "OEM restore accepted; image verified and device will reboot";
        ESP_LOGW(TAG,
                 "Local OEM restore accepted: bytes=%u project='%s' quietcool_marker=%s",
                 static_cast<unsigned>(uploaded_size_), uploaded_project_.c_str(), YESNO(uploaded_marker_seen_));
      } else {
        result_message_ = "Image was written but could not be marked valid; staying on current firmware";
      }
    } else if (image_ready_) {
      parent_->cancel_uploaded_stock();
      image_ready_ = false;
    }

    if (backend_) backend_->abort();
    backend_.reset();
    upload_active_ = false;

    const int status = upload_success_ ? 200 : 409;
    const std::string message = result_message_.empty() ? "No firmware file received" : result_message_;
    request->send(status, "text/plain", message.c_str());
    if (should_reboot) parent_->schedule_uploaded_stock_reboot();
    this->clear_request_state_();
  }

 protected:
  void begin_upload_(AsyncWebServerRequest *request, const std::string &filename) {
    // A completed file part followed by another file part is still the same
    // multipart request: reject it instead of letting a later part replace the
    // already-verified boot selection. If the previous transfer never reached
    // a part boundary, it was interrupted and this is a clean retry.
    if (file_part_started_ && file_part_finished_) {
      if (image_ready_) parent_->cancel_uploaded_stock();
      if (backend_) backend_->abort();
      backend_.reset();
      image_ready_ = false;
      upload_active_ = false;
      upload_success_ = false;
      multiple_files_ = true;
      file_part_finished_ = false;
      result_message_ = "Exactly one firmware file must be uploaded";
      ++upload_generation_;
      ESP_LOGW(TAG, "Rejected local OEM request containing multiple file parts");
      return;
    }

    if (backend_) backend_->abort();
    if (image_ready_) parent_->cancel_uploaded_stock();
    backend_.reset();
    inspector_.reset();
    prefix_buffer_.fill(0);
    prefix_buffer_size_ = 0;
    partition_size_ = 0;
    upload_active_ = false;
    upload_success_ = false;
    image_ready_ = false;
    multiple_files_ = false;
    file_part_started_ = true;
    file_part_finished_ = false;
    uploaded_size_ = 0;
    uploaded_project_.clear();
    uploaded_marker_seen_ = false;
    result_message_.clear();
    ++upload_generation_;

    const std::string confirmation = request->hasParam("confirm") ? request->arg("confirm") : "";
    if (confirmation != "RESTORE_OEM_FIRMWARE") {
      result_message_ = "Explicit OEM restore confirmation is required";
      ESP_LOGW(TAG, "Rejected local OEM upload without confirmation");
      return;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
      result_message_ = "No inactive OTA partition is available";
      return;
    }
    partition_size_ = target->size;

    upload_active_ = true;
    this->arm_timeout_();
    ESP_LOGW(TAG, "Local OEM restore upload started (preflight): filename='%s' target=%s@0x%06" PRIx32 " size=%u",
             filename.c_str(), target->label, target->address, static_cast<unsigned>(target->size));
  }

  bool start_backend_() {
    backend_ = ota::make_ota_backend();
    if (!backend_) {
      this->fail_("Could not initialize the ESP-IDF OTA backend");
      return false;
    }
    auto error = backend_->begin(0);
    if (error == ota::OTA_RESPONSE_OK)
      error = backend_->write(prefix_buffer_.data(), prefix_buffer_size_);
    if (error != ota::OTA_RESPONSE_OK) {
      this->fail_("Could not begin writing the inactive OTA partition");
      return false;
    }
    ESP_LOGW(TAG, "Local OEM upload prefix accepted; writing inactive OTA slot");
    return true;
  }

  void arm_timeout_() {
    const uint32_t generation = upload_generation_;
    parent_->schedule_stock_upload_timeout([this, generation]() {
      if (generation != upload_generation_ || (!upload_active_ && !image_ready_ && !backend_)) return;
      if (backend_) backend_->abort();
      backend_.reset();
      if (image_ready_) parent_->cancel_uploaded_stock();
      upload_active_ = false;
      upload_success_ = false;
      image_ready_ = false;
      file_part_started_ = false;
      file_part_finished_ = false;
      result_message_ = "Upload timed out before the request completed";
      ++upload_generation_;
      ESP_LOGE(TAG, "Local OEM upload timed out; OTA session aborted");
    });
  }

  void clear_request_state_() {
    ++upload_generation_;
    backend_.reset();
    inspector_.reset();
    prefix_buffer_.fill(0);
    prefix_buffer_size_ = 0;
    partition_size_ = 0;
    upload_active_ = false;
    upload_success_ = false;
    image_ready_ = false;
    multiple_files_ = false;
    file_part_started_ = false;
    file_part_finished_ = false;
    uploaded_size_ = 0;
    uploaded_project_.clear();
    uploaded_marker_seen_ = false;
    result_message_.clear();
  }

  void fail_(const char *message) {
    if (backend_) backend_->abort();
    backend_.reset();
    upload_active_ = false;
    upload_success_ = false;
    result_message_ = message;
    ESP_LOGE(TAG, "Local OEM upload rejected: %s", message);
  }

  HttpFlashHandler *parent_;
  ota::OTABackendPtr backend_{nullptr};
  qc::StockUploadInspector inspector_;
  std::array<uint8_t, qc::StockUploadInspector::PREFIX_SIZE> prefix_buffer_{};
  size_t prefix_buffer_size_{0};
  size_t partition_size_{0};
  bool upload_active_{false};
  bool upload_success_{false};
  bool image_ready_{false};
  bool multiple_files_{false};
  bool file_part_started_{false};
  bool file_part_finished_{false};
  size_t uploaded_size_{0};
  std::string uploaded_project_;
  bool uploaded_marker_seen_{false};
  uint32_t upload_generation_{0};
  std::string result_message_;
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
  // Listen for OTA completion so request_stock_restore_finalize() can mark a
  // known foreign-firmware slot valid before the reboot (see on_ota_state).
  ota_->add_state_listener(this);
  web_server_base::global_web_server_base->add_handler(new FlashUrlHandler(this));
  web_server_base::global_web_server_base->add_handler(new StockFileUploadHandler(this));
  ESP_LOGCONFIG(TAG, "Registered POST /api/flash_url and POST /api/restore_stock_file");
}

bool HttpFlashHandler::confirm_new_firmware_slot_() {
  // OTA backend end() has called esp_ota_set_boot_partition() on the newly
  // written slot. Although it is not the running partition yet, it is now the
  // active otadata entry in NEW state, which this API changes to VALID.
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    ESP_LOGW(TAG, "Confirmed new firmware slot VALID — app rollback will keep it");
    return true;
  }

  ESP_LOGE(TAG, "Could not confirm new slot (err=0x%X); restoring current boot selection", err);
  this->cancel_uploaded_stock();
  return false;
}

bool HttpFlashHandler::cancel_uploaded_stock() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running == nullptr) {
    ESP_LOGE(TAG, "Could not restore running boot partition: partition unavailable");
    return false;
  }
  const esp_err_t err = esp_ota_set_boot_partition(running);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not restore running boot partition (err=0x%X)", err);
    return false;
  }
  ESP_LOGW(TAG, "Restored current firmware as the next boot partition");
  return true;
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
        if (!this->confirm_new_firmware_slot_()) this->erase_esphome_on_powerdown_ = false;
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

bool HttpFlashHandler::finalize_uploaded_stock() {
  if (!this->confirm_new_firmware_slot_()) return false;
  this->erase_esphome_on_powerdown_ = true;
  return true;
}

void HttpFlashHandler::schedule_uploaded_stock_reboot() {
  this->set_timeout("stock-file-reboot", 1000, []() {
    ESP_LOGW(TAG, "Rebooting into locally uploaded OEM firmware");
    App.safe_reboot();
  });
}

void HttpFlashHandler::on_powerdown() {
  if (!this->erase_esphome_on_powerdown_ && !this->erase_wifi_on_powerdown_) return;
  const bool erase_esphome = this->erase_esphome_on_powerdown_;
  const bool erase_wifi = this->erase_wifi_on_powerdown_;
  this->erase_esphome_on_powerdown_ = false;
  this->erase_wifi_on_powerdown_ = false;

  auto erase_namespace = [](const char *name) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(name, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
      err = nvs_erase_all(handle);
      if (err == ESP_OK) err = nvs_commit(handle);
      nvs_close(handle);
    }
    return err;
  };

  if (erase_esphome) {
    esp_err_t err = erase_namespace("esphome");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
      ESP_LOGE(TAG, "Could not erase ESPHome NVS namespace (err=0x%X)", err);
  }
  if (erase_wifi) {
    esp_err_t err = erase_namespace("nvs.net80211");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
      ESP_LOGE(TAG, "Could not erase Wi-Fi NVS namespace (err=0x%X)", err);
  }
  ESP_LOGW(TAG, "Reset NVS namespaces: esphome=%s wifi=%s; preserved OEM hx_list",
           YESNO(erase_esphome), YESNO(erase_wifi));
}

void HttpFlashHandler::dump_config() {
  ESP_LOGCONFIG(TAG, "QuietCool HTTP flash handler:");
  ESP_LOGCONFIG(TAG, "  Endpoint: POST /api/flash_url?url=<url>&md5=<32hex>");
  ESP_LOGCONFIG(TAG, "  Restore page: GET /restore-stock");
  ESP_LOGCONFIG(TAG, "  Endpoint: POST /api/restore_stock_file?confirm=RESTORE_OEM_FIRMWARE");
  ESP_LOGCONFIG(TAG, "  web_server_base:  %s",
                web_server_base::global_web_server_base ? "OK" : "MISSING");
  ESP_LOGCONFIG(TAG, "  ota.http_request: %s", ota_ ? "OK" : "MISSING");
}

void HttpFlashHandler::schedule_flash(const std::string &url, const std::string &md5,
                                      bool stock_restore) {
  // Capture all request state by value — the request object is gone by the
  // time this fires. Arm stock finalization inside the scheduled callback so a
  // newer request replacing this named timeout cannot inherit a stale flag.
  this->set_timeout("flash", 500, [this, url, md5, stock_restore]() {
    if (stock_restore) this->request_stock_restore_finalize();
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
