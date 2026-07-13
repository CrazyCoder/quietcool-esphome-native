// Host-side unit tests for HttpFlashLogic — validates POST /api/flash_url
// request parameters before passing them to OtaHttpRequestComponent.flash().
//
// Compile + run:
//   g++ -std=c++17 -I.. test_http_flash_logic.cpp -o test_http_flash_logic.exe
//   ./test_http_flash_logic.exe

#include "test_utils.h"
#include "../http_flash_logic.h"
#include "../stock_upload_logic.h"

using qc::FlashRequestStatus;
using qc::HttpFlashLogic;
using qc::StockUploadInspector;
using qc::StockUploadStatus;

namespace qc {
inline const char* status_name(FlashRequestStatus s) {
  switch (s) {
    case FlashRequestStatus::Ok:                  return "Ok";
    case FlashRequestStatus::MissingUrl:          return "MissingUrl";
    case FlashRequestStatus::InvalidScheme:       return "InvalidScheme";
    case FlashRequestStatus::InvalidMd5Length:    return "InvalidMd5Length";
    case FlashRequestStatus::InvalidMd5Characters:return "InvalidMd5Characters";
  }
  return "??";
}
inline std::ostream& operator<<(std::ostream& os, FlashRequestStatus s) {
  return os << status_name(s);
}
inline std::ostream& operator<<(std::ostream& os, StockUploadStatus s) {
  return os << StockUploadInspector::status_message(s);
}
}  // namespace qc

// ============================================================================
// URL validation.
// ============================================================================

TEST("empty URL -> MissingUrl") {
  REQUIRE_EQ(HttpFlashLogic::validate("", ""), FlashRequestStatus::MissingUrl);
}

TEST("whitespace-only URL -> MissingUrl (treated as empty after trim)") {
  REQUIRE_EQ(HttpFlashLogic::validate("   ", ""), FlashRequestStatus::MissingUrl);
}

TEST("URL with no scheme -> InvalidScheme") {
  REQUIRE_EQ(HttpFlashLogic::validate("example.com/firmware.bin", ""),
             FlashRequestStatus::InvalidScheme);
}

TEST("ftp:// URL -> InvalidScheme (only http/https allowed)") {
  REQUIRE_EQ(HttpFlashLogic::validate("ftp://example.com/fw.bin", ""),
             FlashRequestStatus::InvalidScheme);
}

TEST("file:// URL -> InvalidScheme (no local-file access)") {
  REQUIRE_EQ(HttpFlashLogic::validate("file:///etc/passwd", ""),
             FlashRequestStatus::InvalidScheme);
}

TEST("http:// URL -> Ok") {
  REQUIRE_EQ(HttpFlashLogic::validate("http://example.com/firmware.bin", ""),
             FlashRequestStatus::Ok);
}

TEST("https:// URL -> Ok") {
  REQUIRE_EQ(HttpFlashLogic::validate("https://example.com/firmware.bin", ""),
             FlashRequestStatus::Ok);
}

TEST("HTTP:// (uppercase scheme) -> Ok") {
  REQUIRE_EQ(HttpFlashLogic::validate("HTTP://example.com/firmware.bin", ""),
             FlashRequestStatus::Ok);
}

TEST("Mixed-case https -> Ok") {
  REQUIRE_EQ(HttpFlashLogic::validate("HtTpS://example.com/fw.bin", ""),
             FlashRequestStatus::Ok);
}

TEST("URL with port + query string -> Ok") {
  REQUIRE_EQ(HttpFlashLogic::validate("http://10.0.0.1:8000/fw.bin?v=2", ""),
             FlashRequestStatus::Ok);
}

TEST("the QuietCool OEM CDN URL -> Ok") {
  REQUIRE_EQ(HttpFlashLogic::validate(
                 "http://myquietcool.com/profile/upload/2025/11/18/IT-BLT-ATTICFAN_V4.1_20251118010357A008.bin",
                 ""),
             FlashRequestStatus::Ok);
}

// ============================================================================
// MD5 validation — optional (empty Ok), else 32 hex chars exactly.
// ============================================================================

TEST("empty MD5 -> Ok (handler fetches <url>.md5)") {
  REQUIRE_EQ(HttpFlashLogic::validate("http://example.com/fw.bin", ""),
             FlashRequestStatus::Ok);
}

TEST("MD5 32 hex chars lowercase -> Ok") {
  REQUIRE_EQ(HttpFlashLogic::validate("http://example.com/fw.bin",
                                       "36d2e90dcfdd553272fc4eebdc3c4444"),
             FlashRequestStatus::Ok);
}

TEST("MD5 32 hex chars uppercase -> Ok") {
  REQUIRE_EQ(HttpFlashLogic::validate("http://example.com/fw.bin",
                                       "36D2E90DCFDD553272FC4EEBDC3C4444"),
             FlashRequestStatus::Ok);
}

TEST("MD5 mixed case -> Ok") {
  REQUIRE_EQ(HttpFlashLogic::validate("http://example.com/fw.bin",
                                       "36d2E90DcFDD553272FC4eeBDC3c4444"),
             FlashRequestStatus::Ok);
}

TEST("MD5 31 chars -> InvalidMd5Length") {
  REQUIRE_EQ(HttpFlashLogic::validate("http://example.com/fw.bin",
                                       "36d2e90dcfdd553272fc4eebdc3c444"),
             FlashRequestStatus::InvalidMd5Length);
}

TEST("MD5 33 chars -> InvalidMd5Length") {
  REQUIRE_EQ(HttpFlashLogic::validate("http://example.com/fw.bin",
                                       "36d2e90dcfdd553272fc4eebdc3c4444X"),
             FlashRequestStatus::InvalidMd5Length);
}

TEST("MD5 with non-hex character -> InvalidMd5Characters") {
  REQUIRE_EQ(HttpFlashLogic::validate("http://example.com/fw.bin",
                                       "36d2e90dcfdd553272fc4eebdc3c444g"),
             FlashRequestStatus::InvalidMd5Characters);
}

TEST("MD5 with internal whitespace (32 chars total) -> InvalidMd5Characters") {
  // Length is exactly 32 — including a space — so length check passes and
  // character check is what flags the bad input.
  REQUIRE_EQ(HttpFlashLogic::validate("http://example.com/fw.bin",
                                       "36d2e90dcfdd553272fc4eebdc3c4 44"),
             FlashRequestStatus::InvalidMd5Characters);
}

// ============================================================================
// Url+md5 trimming — accept leading/trailing whitespace (common with copy-paste).
// ============================================================================

TEST("URL with leading whitespace -> Ok (trimmed)") {
  REQUIRE_EQ(HttpFlashLogic::validate("   http://example.com/fw.bin", ""),
             FlashRequestStatus::Ok);
}

TEST("URL with trailing whitespace -> Ok (trimmed)") {
  REQUIRE_EQ(HttpFlashLogic::validate("http://example.com/fw.bin\t\n", ""),
             FlashRequestStatus::Ok);
}

TEST("MD5 with leading/trailing whitespace -> Ok (trimmed)") {
  REQUIRE_EQ(HttpFlashLogic::validate("http://example.com/fw.bin",
                                       "  36d2e90dcfdd553272fc4eebdc3c4444\n"),
             FlashRequestStatus::Ok);
}

// ============================================================================
// validate() out-params — hand the trimmed url + md5 back to the caller so it
// doesn't re-trim (the source of truth is the single trim inside validate).
// ============================================================================

TEST("validate: out-params receive trimmed url + md5 on success") {
  std::string url_t, md5_t;
  REQUIRE_EQ(HttpFlashLogic::validate("  http://example.com/fw.bin \n",
                                       "\t36d2e90dcfdd553272fc4eebdc3c4444 ",
                                       &url_t, &md5_t),
             FlashRequestStatus::Ok);
  REQUIRE_EQ(url_t, std::string("http://example.com/fw.bin"));
  REQUIRE_EQ(md5_t, std::string("36d2e90dcfdd553272fc4eebdc3c4444"));
}

// ============================================================================
// Known stock restore classification — only the verified OEM URL+MD5 pair
// may opt out of app rollback and request namespace-scoped cleanup.
// ============================================================================

TEST("known stock restore: exact OEM V4.1 URL + MD5 -> true") {
  REQUIRE(HttpFlashLogic::is_known_stock_restore(
      HttpFlashLogic::OEM_V41_URL, HttpFlashLogic::OEM_V41_MD5));
}

TEST("known stock restore: uppercase MD5 -> true") {
  REQUIRE(HttpFlashLogic::is_known_stock_restore(
      HttpFlashLogic::OEM_V41_URL, "36D2E90DCFDD553272FC4EEBDC3C4444"));
}

TEST("known stock restore: OEM URL without explicit MD5 -> false") {
  REQUIRE(!HttpFlashLogic::is_known_stock_restore(HttpFlashLogic::OEM_V41_URL, ""));
}

TEST("known stock restore: OEM URL with wrong MD5 -> false") {
  REQUIRE(!HttpFlashLogic::is_known_stock_restore(
      HttpFlashLogic::OEM_V41_URL, "00000000000000000000000000000000"));
}

TEST("known stock restore: stock MD5 at another URL -> false") {
  REQUIRE(!HttpFlashLogic::is_known_stock_restore(
      "https://example.com/oem-v41.bin", HttpFlashLogic::OEM_V41_MD5));
}

// ============================================================================
// status_message() — user-facing text for each result, kept next to the enum.
// ============================================================================

TEST("status_message: every status maps to a non-empty message") {
  for (auto s : {FlashRequestStatus::Ok, FlashRequestStatus::MissingUrl,
                 FlashRequestStatus::InvalidScheme, FlashRequestStatus::InvalidMd5Length,
                 FlashRequestStatus::InvalidMd5Characters}) {
    REQUIRE(HttpFlashLogic::status_message(s)[0] != '\0');
  }
}

TEST("status_message: rejection text matches the status") {
  REQUIRE_EQ(std::string(HttpFlashLogic::status_message(FlashRequestStatus::MissingUrl)),
             std::string("Missing or empty 'url' parameter"));
}

// ============================================================================
// trim() helper — the shared primitive validate() uses internally and hands
// back via its out-params. Unit-tested directly here.
// ============================================================================

TEST("trim: leading + trailing whitespace stripped") {
  REQUIRE_EQ(HttpFlashLogic::trim("  hello  "), std::string("hello"));
}

TEST("trim: tabs and newlines treated as whitespace") {
  REQUIRE_EQ(HttpFlashLogic::trim("\t\nhello\r\n"), std::string("hello"));
}

TEST("trim: empty string stays empty") {
  REQUIRE_EQ(HttpFlashLogic::trim(""), std::string(""));
}

TEST("trim: whitespace-only stays empty") {
  REQUIRE_EQ(HttpFlashLogic::trim("   \t\n"), std::string(""));
}

TEST("trim: no whitespace stays unchanged") {
  REQUIRE_EQ(HttpFlashLogic::trim("hello"), std::string("hello"));
}

// ============================================================================
// Local stock-upload inspection. The OTA backend performs the cryptographic
// and segment validation; this streaming preflight rejects clearly wrong file
// types and prevents replacement firmware from bypassing normal rollback.
// ============================================================================

static std::vector<uint8_t> make_app_image(std::string_view project = "sec_gatts_demo",
                                           size_t size = 512) {
  std::vector<uint8_t> image(size, 0);
  image[0] = 0xE9;
  image[1] = 6;
  image[0x20] = 0x32;
  image[0x21] = 0x54;
  image[0x22] = 0xCD;
  image[0x23] = 0xAB;
  for (size_t i = 0; i < project.size() && i < StockUploadInspector::PROJECT_NAME_SIZE; ++i)
    image[StockUploadInspector::PROJECT_NAME_OFFSET + i] = static_cast<uint8_t>(project[i]);
  return image;
}

TEST("stock upload: version-independent ESP32 app image -> Ok") {
  auto image = make_app_image("some_future_oem_project");
  StockUploadInspector inspector;
  inspector.consume(image.data(), image.size());
  REQUIRE_EQ(inspector.validate(0x1E0000), StockUploadStatus::Ok);
  REQUIRE_EQ(inspector.project_name(), std::string("some_future_oem_project"));
}

TEST("stock upload: stock V4.1 generic project name -> Ok") {
  auto image = make_app_image("sec_gatts_demo");
  StockUploadInspector inspector;
  inspector.consume(image.data(), image.size());
  REQUIRE_EQ(inspector.validate(0x1E0000), StockUploadStatus::Ok);
}

TEST("stock upload: replacement project is rejected") {
  auto image = make_app_image("quietcool-atticfan");
  StockUploadInspector inspector;
  inspector.consume(image.data(), image.size());
  REQUIRE_EQ(inspector.validate(0x1E0000), StockUploadStatus::ReplacementFirmware);
  REQUIRE_EQ(inspector.validate_prefix(), StockUploadStatus::ReplacementFirmware);
}

TEST("stock upload: full-flash or bootloader image without app descriptor is rejected") {
  auto image = make_app_image();
  image[0x20] = 0;
  StockUploadInspector inspector;
  inspector.consume(image.data(), image.size());
  REQUIRE_EQ(inspector.validate(0x1E0000), StockUploadStatus::MissingAppDescriptor);
}

TEST("stock upload: wrong image magic is rejected") {
  auto image = make_app_image();
  image[0] = 0;
  StockUploadInspector inspector;
  inspector.consume(image.data(), image.size());
  REQUIRE_EQ(inspector.validate(0x1E0000), StockUploadStatus::InvalidImageMagic);
}

TEST("stock upload: image for another chip is rejected") {
  auto image = make_app_image();
  image[12] = 9;
  StockUploadInspector inspector;
  inspector.consume(image.data(), image.size());
  REQUIRE_EQ(inspector.validate(0x1E0000), StockUploadStatus::WrongChip);
}

TEST("stock upload: image larger than OTA slot is rejected") {
  auto image = make_app_image();
  StockUploadInspector inspector;
  inspector.consume(image.data(), image.size());
  REQUIRE_EQ(inspector.validate(image.size() - 1), StockUploadStatus::TooLarge);
}

TEST("stock upload: truncated image is rejected") {
  auto image = make_app_image();
  StockUploadInspector inspector;
  inspector.consume(image.data(), 40);
  REQUIRE_EQ(inspector.validate(0x1E0000), StockUploadStatus::TooShort);
  REQUIRE_EQ(inspector.validate_prefix(), StockUploadStatus::TooShort);
}

TEST("stock upload: valid prefix can be accepted before the complete image arrives") {
  auto image = make_app_image("sec_gatts_demo");
  StockUploadInspector inspector;
  inspector.consume(image.data(), StockUploadInspector::PREFIX_SIZE);
  REQUIRE_EQ(inspector.validate_prefix(), StockUploadStatus::Ok);
  REQUIRE_EQ(inspector.validate(StockUploadInspector::PREFIX_SIZE - 1), StockUploadStatus::TooLarge);
}

TEST("stock upload: QuietCool marker is found across chunk boundaries") {
  auto image = make_app_image();
  const std::string marker(StockUploadInspector::QUIETCOOL_MARKER);
  image.insert(image.end(), marker.begin(), marker.end());
  StockUploadInspector inspector;
  const size_t split = image.size() - marker.size() + 5;
  inspector.consume(image.data(), split);
  REQUIRE(!inspector.quietcool_marker_seen());
  inspector.consume(image.data() + split, image.size() - split);
  REQUIRE(inspector.quietcool_marker_seen());
  REQUIRE_EQ(inspector.validate(0x1E0000), StockUploadStatus::Ok);
}

TEST("stock upload: QuietCool marker is informational, not a version whitelist") {
  auto image = make_app_image("future_vendor_build");
  StockUploadInspector inspector;
  inspector.consume(image.data(), image.size());
  REQUIRE(!inspector.quietcool_marker_seen());
  REQUIRE_EQ(inspector.validate(0x1E0000), StockUploadStatus::Ok);
}

TEST("stock upload: reset clears streaming state") {
  auto image = make_app_image();
  const std::string marker(StockUploadInspector::QUIETCOOL_MARKER);
  image.insert(image.end(), marker.begin(), marker.end());
  StockUploadInspector inspector;
  inspector.consume(image.data(), image.size());
  REQUIRE(inspector.quietcool_marker_seen());
  inspector.reset();
  REQUIRE_EQ(inspector.total_size(), 0u);
  REQUIRE(!inspector.quietcool_marker_seen());
}

int main() { return tu::run_all(); }
