// DualGestureTracker — tracks press/release events for KEY1 + KEY2 and
// computes hold durations for both single-button gestures (existing) AND
// a both-buttons-held-simultaneously "dual" gesture (used for the stock
// firmware restore action).
//
// Pure C++17 logic — no ESPHome includes, no millis() calls. Caller injects
// `now_ms` on every event so this is unit-testable host-side.
//
// Semantics:
//   - Single press+release pair → on_release returns held_ms.
//   - If BOTH keys are pressed at some point during the press overlap, the
//     "dual hold" arms. The dual-hold duration is measured from the moment
//     the second key was pressed to the first release.
//   - The FIRST release of a dual hold:
//        * returns 0 from on_release (suppresses the per-button single
//          action that the YAML lambda would otherwise dispatch)
//        * records pending_dual_held_ms, consumable once via
//          take_pending_dual_held_ms() by the same YAML lambda.
//   - The SECOND release after a dual hold also returns 0 (no double-fire
//     of the single-button action for the trailing key release).
//
// LED feedback consumers read three accessors at 10 Hz:
//   - key1_held_ms_now / key2_held_ms_now: live single-button hold time
//   - dual_held_ms_now: live "both currently held" time (0 if not in dual)

#pragma once

#include <cstdint>

namespace qc {

class DualGestureTracker {
 public:
  // Threshold for the dual-button STOCK RESTORE commit. The YAML release
  // lambda compares take_pending_dual_held_ms() against this.
  // 10s is deliberately longer than the single-button COMMIT_MS (5s) so the
  // user can release at any point during the 5–10s window to safely abort.
  static constexpr uint32_t COMMIT_MS = 10000u;

  // Optional secondary threshold for LED feedback (intermediate "you're past
  // single-commit, keep holding for dual-commit"). Currently unused inside
  // this class — exposed as a constant for the LED state machine to share.
  static constexpr uint32_t WARNING_MS = 5000u;

  void on_press(uint8_t key, uint32_t now_ms) {
    if (key == 1) {
      key1_pressed_ = true;
      key1_press_start_ms_ = now_ms;
      if (key2_pressed_) start_dual_(now_ms);
    } else if (key == 2) {
      key2_pressed_ = true;
      key2_press_start_ms_ = now_ms;
      if (key1_pressed_) start_dual_(now_ms);
    }
  }

  // Returns single-button held_ms, OR 0 if the release should be suppressed
  // (either because the key wasn't pressed, OR because this release is part
  // of a dual gesture). Pair with take_pending_dual_held_ms() to see if a
  // dual gesture just completed.
  uint32_t on_release(uint8_t key, uint32_t now_ms) {
    if (key == 1) {
      if (!key1_pressed_) {
        // Either the second release after a dual (suppress_next_release_ set)
        // or a spurious release of an un-pressed key — either way, 0.
        suppress_next_release_ = false;
        return 0;
      }
      key1_pressed_ = false;
      if (dual_active_) {
        pending_dual_held_ms_ = now_ms - dual_press_start_ms_;
        dual_active_ = false;
        suppress_next_release_ = key2_pressed_;
        return 0;
      }
      if (suppress_next_release_) {
        suppress_next_release_ = false;
        return 0;
      }
      return now_ms - key1_press_start_ms_;
    } else if (key == 2) {
      if (!key2_pressed_) {
        suppress_next_release_ = false;
        return 0;
      }
      key2_pressed_ = false;
      if (dual_active_) {
        pending_dual_held_ms_ = now_ms - dual_press_start_ms_;
        dual_active_ = false;
        suppress_next_release_ = key1_pressed_;
        return 0;
      }
      if (suppress_next_release_) {
        suppress_next_release_ = false;
        return 0;
      }
      return now_ms - key2_press_start_ms_;
    }
    return 0;
  }

  // One-shot consumer for the dual-hold duration. Returns the pending value
  // (set when on_release ended a dual hold) and clears it. Subsequent calls
  // return 0 until the next dual gesture completes.
  uint32_t take_pending_dual_held_ms() {
    const uint32_t v = pending_dual_held_ms_;
    pending_dual_held_ms_ = 0;
    return v;
  }

  // For LED feedback at 10 Hz — live hold durations.
  uint32_t key1_held_ms_now(uint32_t now_ms) const {
    return key1_pressed_ ? (now_ms - key1_press_start_ms_) : 0u;
  }
  uint32_t key2_held_ms_now(uint32_t now_ms) const {
    return key2_pressed_ ? (now_ms - key2_press_start_ms_) : 0u;
  }
  uint32_t dual_held_ms_now(uint32_t now_ms) const {
    return dual_active_ ? (now_ms - dual_press_start_ms_) : 0u;
  }

  bool key1_pressed() const { return key1_pressed_; }
  bool key2_pressed() const { return key2_pressed_; }

 private:
  void start_dual_(uint32_t now_ms) {
    dual_active_ = true;
    dual_press_start_ms_ = now_ms;
  }

  bool key1_pressed_ = false;
  bool key2_pressed_ = false;
  uint32_t key1_press_start_ms_ = 0;
  uint32_t key2_press_start_ms_ = 0;

  bool dual_active_ = false;
  uint32_t dual_press_start_ms_ = 0;

  // Set on the FIRST release of a dual hold when the other key is still
  // pressed — consumed (and cleared) by that other key's release so the
  // single-button action doesn't double-fire.
  bool suppress_next_release_ = false;

  // Set on the first release of a dual hold; cleared by take_pending_dual_held_ms.
  uint32_t pending_dual_held_ms_ = 0;
};

}  // namespace qc
