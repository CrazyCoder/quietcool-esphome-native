// Host-side unit tests for HttpFlashLogic — validates POST /api/flash_url
// request parameters before passing them to OtaHttpRequestComponent.flash().
//
// Compile + run:
//   g++ -std=c++17 -I.. test_http_flash_logic.cpp -o test_http_flash_logic.exe
//   ./test_http_flash_logic.exe

#include "test_utils.h"
#include "../http_flash_logic.h"

using qc::FlashRequestStatus;
using qc::HttpFlashLogic;

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

int main() { return tu::run_all(); }
