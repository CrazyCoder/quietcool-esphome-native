// StockUploadInspector — streaming, platform-independent validation for the
// local OEM firmware upload endpoint.
//
// This deliberately does not whitelist a particular OEM version or digest.
// A stock image that still exists ten years from now should remain usable.
// The inspector verifies the stable ESP-IDF OTA application envelope, rejects
// this project's own replacement image (which must retain normal rollback),
// enforces the target partition size, and records a QuietCool product marker as
// an informational confidence signal. ESP-IDF's OTA backend performs the final
// image checksum, appended SHA-256, chip-revision, and segment validation.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace qc {

enum class StockUploadStatus : uint8_t {
  Ok = 0,
  TooShort,
  TooLarge,
  InvalidImageMagic,
  InvalidSegmentCount,
  WrongChip,
  MissingAppDescriptor,
  ReplacementFirmware,
};

class StockUploadInspector {
 public:
  static constexpr size_t PREFIX_SIZE = 0x70;
  static constexpr size_t PROJECT_NAME_OFFSET = 0x50;
  static constexpr size_t PROJECT_NAME_SIZE = 32;
  static constexpr std::string_view QUIETCOOL_MARKER = "IT-BLT-ATTICFAN";
  static constexpr std::string_view REPLACEMENT_PROJECT = "quietcool-atticfan";

  void reset() {
    prefix_.fill(0);
    prefix_size_ = 0;
    total_size_ = 0;
    marker_match_ = 0;
    quietcool_marker_seen_ = false;
  }

  void consume(const uint8_t *data, size_t len) {
    if (data == nullptr || len == 0) return;

    for (size_t i = 0; i < len; ++i) {
      const uint8_t byte = data[i];
      if (prefix_size_ < prefix_.size()) prefix_[prefix_size_++] = byte;

      if (!quietcool_marker_seen_) {
        const uint8_t wanted = static_cast<uint8_t>(QUIETCOOL_MARKER[marker_match_]);
        if (byte == wanted) {
          ++marker_match_;
          if (marker_match_ == QUIETCOOL_MARKER.size()) {
            quietcool_marker_seen_ = true;
            marker_match_ = 0;
          }
        } else {
          marker_match_ = byte == static_cast<uint8_t>(QUIETCOOL_MARKER[0]) ? 1u : 0u;
        }
      }
    }
    total_size_ += len;
  }

  StockUploadStatus validate(size_t partition_size) const {
    if (prefix_size_ < PREFIX_SIZE) return StockUploadStatus::TooShort;
    if (partition_size == 0 || total_size_ > partition_size) return StockUploadStatus::TooLarge;

    // esp_image_header_t: magic, segment_count, ... chip_id at bytes 12-13.
    if (prefix_[0] != 0xE9) return StockUploadStatus::InvalidImageMagic;
    if (prefix_[1] == 0 || prefix_[1] > 16) return StockUploadStatus::InvalidSegmentCount;
    if (prefix_[12] != 0 || prefix_[13] != 0) return StockUploadStatus::WrongChip;

    // esp_app_desc_t follows the 24-byte image header and first 8-byte segment
    // header. Its magic word is 0xABCD5432 in little-endian byte order. This
    // rejects bootloader images and full-flash dumps while accepting ordinary
    // ESP-IDF application OTA images across framework versions.
    if (prefix_[0x20] != 0x32 || prefix_[0x21] != 0x54 ||
        prefix_[0x22] != 0xCD || prefix_[0x23] != 0xAB) {
      return StockUploadStatus::MissingAppDescriptor;
    }

    if (project_name() == REPLACEMENT_PROJECT) return StockUploadStatus::ReplacementFirmware;
    return StockUploadStatus::Ok;
  }

  std::string project_name() const {
    if (prefix_size_ < PROJECT_NAME_OFFSET + PROJECT_NAME_SIZE) return {};
    size_t len = 0;
    while (len < PROJECT_NAME_SIZE && prefix_[PROJECT_NAME_OFFSET + len] != 0) ++len;
    return std::string(reinterpret_cast<const char *>(prefix_.data() + PROJECT_NAME_OFFSET), len);
  }

  size_t total_size() const { return total_size_; }
  bool quietcool_marker_seen() const { return quietcool_marker_seen_; }

  static const char *status_message(StockUploadStatus status) {
    switch (status) {
      case StockUploadStatus::Ok:
        return "OK";
      case StockUploadStatus::TooShort:
        return "File is too short to be an ESP32 application image";
      case StockUploadStatus::TooLarge:
        return "File is larger than the inactive OTA partition";
      case StockUploadStatus::InvalidImageMagic:
        return "File is not an ESP32 application image (missing E9 header)";
      case StockUploadStatus::InvalidSegmentCount:
        return "ESP32 image has an invalid segment count";
      case StockUploadStatus::WrongChip:
        return "Firmware targets a chip other than the original ESP32";
      case StockUploadStatus::MissingAppDescriptor:
        return "File is not an OTA application image (app descriptor missing)";
      case StockUploadStatus::ReplacementFirmware:
        return "Replacement firmware must use the normal OTA upload, not OEM restore";
    }
    return "Invalid OEM firmware upload";
  }

 private:
  std::array<uint8_t, PREFIX_SIZE> prefix_{};
  size_t prefix_size_{0};
  size_t total_size_{0};
  size_t marker_match_{0};
  bool quietcool_marker_seen_{false};
};

}  // namespace qc
