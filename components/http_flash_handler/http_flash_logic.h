// HttpFlashLogic — pure-C++17 validator for the POST /api/flash_url request
// before it's handed to OtaHttpRequestComponent.flash() in the wrapping
// ESPHome component. Lives in its own header so it can be unit-tested on the
// host without dragging ESPHome / AsyncWebServer headers.
//
// Accept rules:
//   - URL must be non-empty (after trim) and start with http:// or https://
//     (case-insensitive). We deliberately don't accept ftp/file/data/etc —
//     the underlying http_request component only knows HTTP anyway, and
//     refusing other schemes early gives a clearer error response.
//   - MD5, if provided, must be exactly 32 hex chars (any case). Empty is
//     allowed — the firmware then fetches the checksum from "<url>.md5"
//     (ota.http_request mandates a checksum; there is no true skip-check).
//   - Whitespace around both URL and MD5 is trimmed (common with copy-paste
//     into a form field).

#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace qc {

enum class FlashRequestStatus : uint8_t {
  Ok = 0,
  MissingUrl,
  InvalidScheme,
  InvalidMd5Length,
  InvalidMd5Characters,
};

class HttpFlashLogic {
 public:
  // Strip ASCII whitespace (space, tab, CR, LF, VT, FF) from both ends.
  static std::string trim(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && is_ws_(s[start])) ++start;
    size_t end = s.size();
    while (end > start && is_ws_(s[end - 1])) --end;
    return std::string(s.substr(start, end - start));
  }

  // Validate the request. When non-null, url_out/md5_out receive the trimmed
  // URL + MD5 so the caller can hand them straight to the OTA component without
  // re-trimming — keeping the trim in one place instead of trusting two call
  // sites to stay in sync.
  static FlashRequestStatus validate(std::string_view url, std::string_view md5,
                                     std::string *url_out = nullptr,
                                     std::string *md5_out = nullptr) {
    std::string url_t = trim(url);
    std::string md5_t = trim(md5);
    if (url_out) *url_out = url_t;
    if (md5_out) *md5_out = md5_t;

    if (url_t.empty()) return FlashRequestStatus::MissingUrl;
    if (!has_http_scheme_(url_t)) return FlashRequestStatus::InvalidScheme;
    if (md5_t.empty()) return FlashRequestStatus::Ok;  // empty ok — handler fetches <url>.md5
    if (md5_t.size() != 32u) return FlashRequestStatus::InvalidMd5Length;
    for (char c : md5_t) {
      if (!is_hex_(c)) return FlashRequestStatus::InvalidMd5Characters;
    }
    return FlashRequestStatus::Ok;
  }

  // Human-readable message for a validation result. Lives next to the enum so
  // adding a status forces a matching message (-Wswitch) and the strings are
  // host-testable rather than buried in the web handler.
  static const char *status_message(FlashRequestStatus s) {
    switch (s) {
      case FlashRequestStatus::Ok:                   return "OK";
      case FlashRequestStatus::MissingUrl:           return "Missing or empty 'url' parameter";
      case FlashRequestStatus::InvalidScheme:        return "URL must start with http:// or https://";
      case FlashRequestStatus::InvalidMd5Length:     return "MD5 must be exactly 32 hex characters (or empty to fetch <url>.md5)";
      case FlashRequestStatus::InvalidMd5Characters: return "MD5 must contain only hex characters (0-9, a-f, A-F)";
    }
    return "Invalid flash request";
  }

 private:
  static bool is_ws_(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
  }
  static bool is_hex_(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  }
  static char to_lower_(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  // Case-insensitive prefix test; prefix must be lowercase and NUL-terminated.
  static bool starts_with_ci_(const std::string &s, const char *prefix) {
    for (size_t i = 0; prefix[i] != '\0'; ++i) {
      if (i >= s.size() || to_lower_(s[i]) != prefix[i]) return false;
    }
    return true;
  }
  static bool has_http_scheme_(const std::string &s) {
    return starts_with_ci_(s, "http://") || starts_with_ci_(s, "https://");
  }
};

}  // namespace qc
