// OEM BLE protocol logic — pure C++17, no ESPHome includes.
// Covers: pair-state and idle-client recovery machines, command gate checks,
// OEM field-format conversions, BLE frame assembly, and response builders for
// each of the 22 JSON commands. Tested host-side via
// test/test_oem_ble_compat_logic.cpp.
//
// The ESPHome wrapper (oem_ble_compat.h/.cpp) owns the GATT service, JSON
// parse/serialize, and fan_controller references.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace qc {

// ── Pair-state machine ──────────────────────────────────────────────

enum class PairState : uint8_t {
  Init     = 0,  // boot / not authenticated
  Auth     = 1,  // logged in with a known pair-id
  PairMode = 2,  // accepting new pair registrations
};

struct PairMachine {
  PairState state = PairState::Init;
  uint32_t pair_mode_deadline_ms = 0;  // millis() after which pair-mode auto-reverts
  std::string current_pair_id;

  // Enter pair mode. Any state → PairMode.
  void enter_pair_mode(uint32_t now_ms, uint32_t timeout_ms = 120000) {
    state = PairState::PairMode;
    pair_mode_deadline_ms = now_ms + timeout_ms;
  }

  // Call periodically; reverts pair-mode when the deadline passes.
  void check_timeout(uint32_t now_ms) {
    if (state != PairState::PairMode) return;
    if (now_ms >= pair_mode_deadline_ms) {
      state = current_pair_id.empty() ? PairState::Init : PairState::Auth;
    }
  }
};

// ── Command gate ────────────────────────────────────────────────────

enum class GateResult : uint8_t {
  Allowed,        // proceed
  NeedAuth,       // pair_state must be Auth
  OtaBlocked,     // OTA in progress, only A=5 allowed
  PreGateOnly,    // Login/Pair — reachable before auth
};

// Pre-gate commands (reachable before auth):
//   A=13 Login (always), A=14 Pair (handler self-gates on PairState::PairMode).
// A=15 PairMode requires PairState::Auth — matches stock ble_v2_dispatcher_main,
// where PairMode sits inside the `pair_state==1` (authenticated) branch. An
// unpaired device can only enter pair mode via the physical KEY2 long-hold;
// allowing remote unauthenticated PairMode would let any in-range client put
// the hub into pair mode and pair itself. GetUpgradeState (A=5) is allowed
// during OTA; everything else needs pair_state==Auth AND no OTA in progress.
inline GateResult check_gate(int a_code, PairState ps, bool ota_in_progress) {
  // Pre-gate: Login always; Pair reachable (handle_pair_ enforces PairMode).
  if (a_code == 13 || a_code == 14)
    return GateResult::PreGateOnly;

  // Auth required for everything else — including A=15 PairMode.
  if (ps != PairState::Auth) return GateResult::NeedAuth;

  // A=5 always allowed when authed (even during OTA)
  if (a_code == 5) return GateResult::Allowed;

  // OTA blocks the rest
  if (ota_in_progress) return GateResult::OtaBlocked;

  return GateResult::Allowed;
}

// ── SetRouter switch decision ───────────────────────────────────────

// SetRouter (A=11) applies new Wi-Fi creds at runtime (save_wifi_sta switches
// the STA live; the .cpp also cycles disable()/enable() to reconnect away from
// the current AP) — no reboot. Switch only on a real change: skip solely when
// we're already connected to exactly this SSID. (Disconnected -> switch even on
// the same SSID, so a recovery after the AP's password changed re-applies.)
inline bool setrouter_should_switch(bool currently_connected,
                                    const std::string &current_ssid,
                                    const std::string &new_ssid) {
  return !(currently_connected && current_ssid == new_ssid);
}

// ── Idle BLE client recovery ────────────────────────────────────────

// Stock QuietCool firmware releases an idle BLE link after roughly 25 seconds.
// Enforce a comparable bound so one abandoned client cannot monopolize the
// server, and so a dropped ESP_GATTS_DISCONNECT_EVT cannot leave ESPHome's
// client count pinned forever. Recovery is suspended during OTA because cycling
// the BLE stack while ota.http_request is writing flash would risk disrupting
// the update. The wrapper first asks Bluedroid to close the recorded connection;
// if ESPHome still reports it after the grace period, the complete BLE stack
// must be recycled to clear stale server and CCCD state.
enum class BleIdleAction : uint8_t {
  None,
  CloseClient,
  RecycleStack,
};

class BleIdleWatchdog {
 public:
  static constexpr uint32_t IDLE_TIMEOUT_MS = 30000;
  static constexpr uint32_t CLOSE_GRACE_MS = 5000;

  void note_activity(uint32_t now_ms) {
    tracking_client_ = true;
    close_requested_ = false;
    last_activity_ms_ = now_ms;
  }

  BleIdleAction update(bool oem_active, bool ota_in_progress,
                       uint8_t client_count, uint32_t now_ms) {
    if (!oem_active || ota_in_progress || client_count == 0) {
      reset();
      return BleIdleAction::None;
    }

    if (!tracking_client_) {
      tracking_client_ = true;
      last_activity_ms_ = now_ms;
      return BleIdleAction::None;
    }

    if (!close_requested_) {
      if (now_ms - last_activity_ms_ < IDLE_TIMEOUT_MS)
        return BleIdleAction::None;
      close_requested_ = true;
      close_requested_ms_ = now_ms;
      return BleIdleAction::CloseClient;
    }

    if (now_ms - close_requested_ms_ < CLOSE_GRACE_MS)
      return BleIdleAction::None;

    reset();
    return BleIdleAction::RecycleStack;
  }

  void reset() {
    tracking_client_ = false;
    close_requested_ = false;
    last_activity_ms_ = 0;
    close_requested_ms_ = 0;
  }

 private:
  bool tracking_client_ = false;
  bool close_requested_ = false;
  uint32_t last_activity_ms_ = 0;
  uint32_t close_requested_ms_ = 0;
};

// ── OEM field format helpers ────────────────────────────────────────

// Temperature: our sensors are °C; OEM wire format is °F × 10 (integer).
inline int temp_c_to_f_x10(float c) {
  return static_cast<int>(std::round((c * 9.0f / 5.0f + 32.0f) * 10.0f));
}

// Smart Mode thresholds are stored in °C internally; OEM wire is °F (integer, not ×10).
inline int threshold_c_to_f(float c) {
  return static_cast<int>(std::round(c * 9.0f / 5.0f + 32.0f));
}

// Reverse: OEM °F integer → our °C float, rounded to 0.1° step grid.
inline float threshold_f_to_c(int f) {
  return std::round(((static_cast<float>(f) - 32.0f) * 5.0f / 9.0f) * 10.0f) / 10.0f;
}

// DIP wiring → OEM string.
inline const char *dip_to_oem(uint8_t dip) {
  switch (dip) {
    case 1: return "TWO";
    case 2: return "THREE";
    case 3: return "ONE";
    default: return "NO";
  }
}

// Our Speed enum (0=Off,1=Low,2=Med,3=High) → OEM speed string.
inline const char *speed_to_oem(uint8_t speed) {
  switch (speed) {
    case 1: return "LOW";
    case 2: return "MEDIUM";
    case 3: return "HIGH";
    default: return "OFF";
  }
}

// OEM speed string → our Speed enum value.
inline uint8_t oem_speed_to_internal(const char *s) {
  if (!s) return 0;
  if (strcmp(s, "HIGH") == 0 || strcmp(s, "high") == 0)     return 3;
  if (strcmp(s, "MEDIUM") == 0 || strcmp(s, "medium") == 0 ||
      strcmp(s, "MED") == 0 || strcmp(s, "med") == 0)       return 2;
  if (strcmp(s, "LOW") == 0 || strcmp(s, "low") == 0)       return 1;
  return 0;  // OFF / CLOSE
}

// Our fan mode → OEM mode string for GetWorkState.
// fan_on=false → "Idle", smart_active → "TH", timer_running → "Run", else → "Timer"
// OEM app mode strings (from Constants.java):
//   "Idle"  = fan off
//   "Timer" = countdown timer running (FAN_MODE_TIMER)
//   "Run"   = running without timer (FAN_MODE_RUN)
//   "TH"    = Smart Mode active (FAN_MODE_SMART)
inline const char *mode_to_oem(bool fan_on, bool smart_active, bool timer_running) {
  if (smart_active) return "TH";
  if (!fan_on) return "Idle";
  if (timer_running) return "Timer";
  return "Run";
}

// Humidity response select value → OEM range string.
inline const char *hum_response_to_oem(const char *select_val) {
  if (!select_val) return "CLOSE";
  if (strcmp(select_val, "Off") == 0) return "CLOSE";
  if (strcmp(select_val, "Low") == 0) return "LOW";
  if (strcmp(select_val, "Medium") == 0) return "MEDIUM";
  if (strcmp(select_val, "High") == 0) return "HIGH";
  return "CLOSE";
}

// OEM range string → our select option.
inline const char *oem_range_to_select(const char *s) {
  if (!s) return "Off";
  if (strcmp(s, "LOW") == 0) return "Low";
  if (strcmp(s, "MEDIUM") == 0) return "Medium";
  if (strcmp(s, "HIGH") == 0) return "High";
  return "Off";
}

// OEM mode string → internal flags.
struct ModeFlags {
  bool turn_on;       // should fan be on?
  bool smart_mode;    // activate Smart Mode?
  bool timer_mode;    // start Run-countdown?
};

inline ModeFlags parse_oem_mode(const char *m) {
  if (!m) return {false, false, false};
  if (strcmp(m, "TH") == 0)    return {false, true, false};
  if (strcmp(m, "Timer") == 0) return {true, false, true};   // countdown timer
  if (strcmp(m, "Run") == 0)   return {true, false, false};  // running, no timer
  // "Idle" or anything else
  return {false, false, false};
}

// ── Frame assembler ─────────────────────────────────────────────────

// Accumulates BLE write chunks into complete messages. A message is
// complete when the buffer contains balanced braces (JSON) or when a
// binary command marker is detected at byte[1].
struct FrameAssembler {
  // Ceiling on a single buffered message. The largest legitimate request is
  // SetPresets with 4 fully-populated presets (~400 bytes); Upgrade with a
  // 100-char URL is ~115. Without a ceiling, a client that writes bytes which
  // never complete a frame — no auth required, framing happens before the
  // command gate — grows this vector until the heap runs out.
  static constexpr size_t MAX_BUFFERED = 1024;

  std::vector<uint8_t> buf;

  // Returns false when the incoming data would exceed MAX_BUFFERED; the buffer
  // is dropped in that case so the session resynchronises on the next message
  // instead of staying wedged.
  bool feed(const uint8_t *data, size_t len) {
    if (buf.size() + len > MAX_BUFFERED) {
      buf.clear();
      return false;
    }
    buf.insert(buf.end(), data, data + len);
    return true;
  }

  // Returns true when a complete frame is available.
  bool has_complete() const { return frame_end_() != 0; }

  // Extracts the complete message and removes it from the buffer.
  std::vector<uint8_t> take() {
    size_t end = frame_end_();
    if (end == 0) return {};
    std::vector<uint8_t> result(buf.begin(), buf.begin() + end);
    buf.erase(buf.begin(), buf.begin() + end);
    return result;
  }

  void clear() { buf.clear(); }

 private:
  // The two binary commands are fixed-length on the wire:
  //   GetRecordData   '{' 0x1C <day-index> '}'                    =  4 bytes
  //   SynchronizeTime '{' 0x1D <10 ASCII epoch digits> '}'        = 13 bytes
  // Frame them by that length rather than by scanning for '}': the day-index is
  // a raw byte that can itself be 0x7D ('}'), and a length-framed read also
  // cannot be confused by a following command already sitting in the buffer.
  // Returns 0 when the head of the buffer is not a binary command.
  size_t binary_len_() const {
    if (buf.size() < 2 || buf[0] != '{') return 0;
    if (buf[1] == 0x1C) return 4;
    if (buf[1] == 0x1D) return 13;
    return 0;
  }

  // Index one past the end of the complete frame at the head of the buffer,
  // or 0 if no complete frame is buffered yet.
  size_t frame_end_() const {
    size_t blen = binary_len_();
    if (blen != 0) return (buf.size() >= blen) ? blen : 0;

    // JSON: balanced braces, ignoring any brace inside a string literal.
    // Field values carry arbitrary user text — a Wi-Fi password or URL may
    // legitimately contain '{' or '}', and counting those would cut the frame
    // mid-message and leave the remainder to corrupt every later command.
    int depth = 0;
    bool in_string = false, escaped = false;
    for (size_t i = 0; i < buf.size(); ++i) {
      uint8_t b = buf[i];
      if (in_string) {
        if (escaped) escaped = false;
        else if (b == '\\') escaped = true;
        else if (b == '"') in_string = false;
        continue;
      }
      if (b == '"') in_string = true;
      else if (b == '{') ++depth;
      // Ignore a stray '}' with no open brace; letting depth go negative would
      // wedge the buffer permanently.
      else if (b == '}' && depth > 0 && --depth == 0) return i + 1;
    }
    return 0;
  }
};

// ── Response chunking ───────────────────────────────────────────────

// Splits a response byte sequence into MTU-3-sized chunks for BLE notify.
inline std::vector<std::vector<uint8_t>> chunk_response(
    const std::vector<uint8_t> &data, uint16_t mtu) {
  std::vector<std::vector<uint8_t>> chunks;
  uint16_t chunk_size = (mtu > 3) ? (mtu - 3) : 20;
  for (size_t offset = 0; offset < data.size(); offset += chunk_size) {
    size_t end = std::min(offset + chunk_size, data.size());
    chunks.emplace_back(data.begin() + offset, data.begin() + end);
  }
  if (chunks.empty()) chunks.push_back({});
  return chunks;
}

// Make a user-supplied string safe to embed in a JSON response value.
//
// Two distinct hazards, both reachable from ordinary user input (a fan name or
// preset name typed in HA or the OEM app, a Wi-Fi SSID):
//   - '"' and '\' would terminate or corrupt the JSON string, so they are
//     escaped, as are control characters.
//   - '{' and '}' are NOT JSON-special inside a string, but the OEM app frames
//     an incoming notification by scanning for a brace regardless of quoting,
//     so either one truncates the message it sees. They are dropped outright;
//     no escape exists that would help.
inline std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      case '{': case '}': break;  // dropped — see above
      default:
        if (c >= 0x20) out += static_cast<char>(c);  // drop other control chars
        break;
    }
  }
  return out;
}

// Prepend "QQ" to a JSON string (V4.1+ wire format).
inline std::vector<uint8_t> qq_wrap(const std::string &json) {
  std::vector<uint8_t> out;
  out.reserve(2 + json.size());
  out.push_back('Q');
  out.push_back('Q');
  out.insert(out.end(), json.begin(), json.end());
  return out;
}

// Frame a binary (non-JSON) response the way stock does: "QQ" prefix, then the
// request's own '{' + type-byte + payload + '}' envelope.
//
// The '}' terminator is mandatory. The Smart Control app accumulates notify
// chunks and asks its receive assembler whether a message is complete; for a
// "QQ"-prefixed buffer that check is purely `endsWith("}")`. A binary response
// without the terminator is never dispatched — the client keeps buffering, the
// pending command never completes, and its request queue stalls.
inline std::vector<uint8_t> binary_frame(uint8_t type,
                                         const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> out;
  out.reserve(5 + payload.size());
  out.push_back('Q');
  out.push_back('Q');
  out.push_back('{');
  out.push_back(type);
  out.insert(out.end(), payload.begin(), payload.end());
  out.push_back('}');
  return out;
}

// A=3 GetVersion compatibility response. The Smart Control app incorrectly
// treats any mismatch with its selected cloud-channel version as an available
// update (compareTo(...) != 0), even when the device reports a newer version.
// Match the production channel exactly to suppress replacement-firmware prompts.
// This is not this project's own release version. Keep the exact response
// host-tested so a future cloud-version bump is an intentional change.
inline const char *get_version_response() {
  return R"({"A":3,"V":"IT-BLT-ATTICFAN_V4.1","P":100,"D":"2025.11.18","M":"online","H":"A"})";
}

// ── Fan model catalogue (from APK CommConstants + MyUtils.getDeviceModel) ──

struct FanModelEntry {
  const char *index;    // "0".."7" — stored in NVS "mmm" key, sent as M in V2
  const char *display;  // human-readable, sent as N in V2
};

static constexpr int FAN_MODEL_COUNT = 8;

// Order matches the OEM app's AtticSelectFanModelActivity tile layout.
inline const FanModelEntry *fan_model_table() {
  static const FanModelEntry table[FAN_MODEL_COUNT] = {
    {"0", "Generic"},
    {"1", "AFG SMT PRO-2.0"},
    {"2", "AFG SMT PRO-3.0"},
    {"3", "AFG SMT ES-3.0"},
    {"4", "AFR SMT ES-2.0(1st Generation)"},
    {"5", "AFR SMT PRO-1.3"},
    {"6", "AFR SMT PRO-2.0"},
    {"7", "AFR SMT ES-2.0(2nd Generation)"},
  };
  return table;
}

inline const char *fan_model_display(const char *index) {
  if (!index) return "Generic";
  const auto *t = fan_model_table();
  for (int i = 0; i < FAN_MODEL_COUNT; i++)
    if (strcmp(t[i].index, index) == 0) return t[i].display;
  return "Generic";
}

inline const char *fan_model_index(const char *display) {
  if (!display) return "0";
  const auto *t = fan_model_table();
  for (int i = 0; i < FAN_MODEL_COUNT; i++)
    if (strcmp(t[i].display, display) == 0) return t[i].index;
  return "0";
}

// ── Input validation ────────────────────────────────────────────────

inline bool validate_phone_id(const std::string &id) {
  return !id.empty() && id.size() <= 100;
}

inline bool validate_url(const std::string &url) {
  return !url.empty() && url.size() <= 100;
}

// ── Upgrade (A=10) URL classification ───────────────────────────────
//
// A=10 Upgrade is auth-gated (see check_gate). This decides what to do with a
// client-supplied firmware URL: reject malformed ones, silently no-op OEM-domain
// URLs (so the OEM app can't flash stock firmware over this build), and flash any
// other valid http(s) URL.

// A=5 GetUpgradeState bytes (mirror stock ble_session_state[3]).
static constexpr uint8_t UPGRADE_STATE_IDLE        = 0;  // "Connect_NO"
static constexpr uint8_t UPGRADE_STATE_DOWNLOADING = 5;  // "Downloading_Progress"
static constexpr uint8_t UPGRADE_STATE_FAIL        = 6;  // "Download_Fail"

enum class UpgradeDecision : uint8_t { Reject, BlockedOemDomain, Flash };

inline char upgrade_to_lower_ascii_(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Lowercased host of a URL: drop scheme, userinfo, path/query/fragment, port.
// Returns "" if there is no "://" scheme.
inline std::string upgrade_url_host_(const std::string &url) {
  size_t scheme = url.find("://");
  if (scheme == std::string::npos) return "";
  size_t start = scheme + 3;
  size_t end = url.find_first_of("/?#", start);
  std::string authority =
      url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  size_t at = authority.rfind('@');          // strip userinfo
  if (at != std::string::npos) authority = authority.substr(at + 1);
  size_t colon = authority.find(':');        // strip :port
  if (colon != std::string::npos) authority = authority.substr(0, colon);
  for (char &c : authority) c = upgrade_to_lower_ascii_(c);
  return authority;
}

// Case-insensitive http:// or https:// scheme check.
inline bool upgrade_has_http_scheme_(const std::string &url) {
  auto starts = [&](const char *p) {
    size_t n = std::strlen(p);
    if (url.size() < n) return false;
    for (size_t i = 0; i < n; i++)
      if (upgrade_to_lower_ascii_(url[i]) != p[i]) return false;
    return true;
  };
  return starts("http://") || starts("https://");
}

// host == d OR host ends with "." + d, for each OEM firmware-delivery domain.
inline bool upgrade_is_oem_host_(const std::string &host) {
  static const char *const kOemDomains[] = {"myquietcool.com", "quietcool.com"};
  for (const char *d : kOemDomains) {
    const std::string dom(d);
    if (host == dom) return true;
    if (host.size() > dom.size() + 1 &&
        host[host.size() - dom.size() - 1] == '.' &&
        host.compare(host.size() - dom.size(), dom.size(), dom) == 0)
      return true;
  }
  return false;
}

inline UpgradeDecision classify_upgrade_url(const std::string &url) {
  if (!validate_url(url)) return UpgradeDecision::Reject;        // empty or >100 chars
  if (!upgrade_has_http_scheme_(url)) return UpgradeDecision::Reject;
  std::string host = upgrade_url_host_(url);
  if (host.empty()) return UpgradeDecision::Reject;
  if (upgrade_is_oem_host_(host)) return UpgradeDecision::BlockedOemDomain;
  return UpgradeDecision::Flash;
}

inline const char *upgrade_state_string(uint8_t s) {
  switch (s) {
    case UPGRADE_STATE_DOWNLOADING: return "Downloading_Progress";
    case UPGRADE_STATE_FAIL:        return "Download_Fail";
    default:                        return "Connect_NO";
  }
}

inline bool validate_ssid(const std::string &ssid) {
  return !ssid.empty() && ssid.size() <= 32;
}

inline bool validate_password(const std::string &pwd) {
  return pwd.size() <= 64;
}

inline bool validate_threshold(int value) {
  return value >= 0 && value < 256;
}

}  // namespace qc
