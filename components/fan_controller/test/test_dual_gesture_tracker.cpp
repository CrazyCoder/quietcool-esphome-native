// Host-side unit tests for DualGestureTracker.
// Pure C++17, no ESPHome includes. Time injected via now_ms — no millis().
//
// Compile + run:
//   g++ -std=c++17 -I.. test_dual_gesture_tracker.cpp -o test_dual_gesture_tracker.exe
//   ./test_dual_gesture_tracker.exe

#include "test_utils.h"
#include "../dual_gesture_tracker.h"

using qc::DualGestureTracker;

// ============================================================================
// 1. Single-button press/release returns held_ms (no dual involvement).
// ============================================================================

TEST("single K1 press+release returns held_ms") {
  DualGestureTracker t;
  t.on_press(1, 1000);
  REQUIRE_EQ(t.on_release(1, 3500), 2500u);
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 0u);
}

TEST("single K2 press+release returns held_ms") {
  DualGestureTracker t;
  t.on_press(2, 500);
  REQUIRE_EQ(t.on_release(2, 1500), 1000u);
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 0u);
}

TEST("release of un-pressed key returns 0, no pending dual") {
  DualGestureTracker t;
  REQUIRE_EQ(t.on_release(1, 1000), 0u);
  REQUIRE_EQ(t.on_release(2, 1000), 0u);
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 0u);
}

// ============================================================================
// 2. Dual press: first release returns 0 (suppress single-button action),
//    take_pending returns dual_held_ms starting from the *second* press.
// ============================================================================

TEST("dual press: K1 first then K2, release K1 first") {
  DualGestureTracker t;
  t.on_press(1, 1000);  // K1 down
  t.on_press(2, 1500);  // K2 down — dual starts here
  // Release K1 at T=12000 — first release of the dual hold.
  REQUIRE_EQ(t.on_release(1, 12000), 0u);  // suppress single-button
  // Dual was held from T=1500 to T=12000 = 10500ms.
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 10500u);
}

TEST("dual press: K1 first then K2, release K2 first") {
  DualGestureTracker t;
  t.on_press(1, 1000);
  t.on_press(2, 1500);
  REQUIRE_EQ(t.on_release(2, 12000), 0u);
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 10500u);
}

TEST("dual press: K2 first then K1 (reverse order)") {
  DualGestureTracker t;
  t.on_press(2, 1000);
  t.on_press(1, 2000);  // K1 down second — dual starts at T=2000
  REQUIRE_EQ(t.on_release(2, 13000), 0u);
  // Dual held from T=2000 to T=13000 = 11000ms.
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 11000u);
}

// ============================================================================
// 3. Second release after dual: also returns 0 (suppress), no new pending.
// ============================================================================

TEST("dual press: second release returns 0, take stays empty") {
  DualGestureTracker t;
  t.on_press(1, 1000);
  t.on_press(2, 1500);
  REQUIRE_EQ(t.on_release(1, 12000), 0u);          // first release: dual end
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 10500u);  // consumed once
  REQUIRE_EQ(t.on_release(2, 12100), 0u);          // second release: suppress
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 0u);   // no new pending
}

TEST("take_pending is one-shot — second call returns 0") {
  DualGestureTracker t;
  t.on_press(1, 0);
  t.on_press(2, 500);
  t.on_release(1, 11000);
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 10500u);
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 0u);
}

// ============================================================================
// 4. Live dual_held_ms_now — for LED feedback at 10 Hz.
// ============================================================================

TEST("dual_held_ms_now: 0 when nothing pressed") {
  DualGestureTracker t;
  REQUIRE_EQ(t.dual_held_ms_now(5000), 0u);
}

TEST("dual_held_ms_now: 0 when only one button pressed") {
  DualGestureTracker t;
  t.on_press(1, 1000);
  REQUIRE_EQ(t.dual_held_ms_now(5000), 0u);
}

TEST("dual_held_ms_now: elapsed since 2nd press when both held") {
  DualGestureTracker t;
  t.on_press(1, 1000);
  t.on_press(2, 1500);
  REQUIRE_EQ(t.dual_held_ms_now(1500), 0u);   // exactly at 2nd press
  REQUIRE_EQ(t.dual_held_ms_now(2000), 500u);
  REQUIRE_EQ(t.dual_held_ms_now(11500), 10000u);
}

TEST("dual_held_ms_now: 0 after dual ended (one released)") {
  DualGestureTracker t;
  t.on_press(1, 1000);
  t.on_press(2, 1500);
  t.on_release(1, 12000);
  REQUIRE_EQ(t.dual_held_ms_now(12500), 0u);
}

// ============================================================================
// 5. Single-button accessors for LED state machine (independent of dual).
// ============================================================================

TEST("key1_held_ms_now: elapsed since K1 press") {
  DualGestureTracker t;
  t.on_press(1, 1000);
  REQUIRE_EQ(t.key1_held_ms_now(3500), 2500u);
}

TEST("key1_held_ms_now: 0 when not pressed") {
  DualGestureTracker t;
  REQUIRE_EQ(t.key1_held_ms_now(1000), 0u);
}

TEST("key2_held_ms_now: independent of K1") {
  DualGestureTracker t;
  t.on_press(1, 1000);
  t.on_press(2, 2000);
  REQUIRE_EQ(t.key2_held_ms_now(5000), 3000u);
}

TEST("key1_held_ms_now: 0 after release") {
  DualGestureTracker t;
  t.on_press(1, 1000);
  t.on_release(1, 3000);
  REQUIRE_EQ(t.key1_held_ms_now(5000), 0u);
}

// ============================================================================
// 6. State stays clean for the next gesture after a full cycle.
// ============================================================================

TEST("clean state after dual cycle: next single press works normally") {
  DualGestureTracker t;
  // Full dual cycle:
  t.on_press(1, 1000);
  t.on_press(2, 1500);
  t.on_release(1, 12000);
  t.on_release(2, 12100);
  t.take_pending_dual_held_ms();  // consume the pending value
  // Fresh single-button press should behave normally.
  t.on_press(1, 20000);
  REQUIRE_EQ(t.on_release(1, 21500), 1500u);  // clean single-button held_ms
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 0u);
}

TEST("clean state after suppressed second release: next dual cycle works") {
  DualGestureTracker t;
  // First dual cycle.
  t.on_press(1, 0);
  t.on_press(2, 500);
  t.on_release(1, 11000);
  t.on_release(2, 11500);
  t.take_pending_dual_held_ms();
  // Second dual cycle.
  t.on_press(1, 20000);
  t.on_press(2, 20500);
  REQUIRE_EQ(t.on_release(2, 31000), 0u);
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 10500u);
}

// ============================================================================
// 7. Edge cases: same key pressed twice without release, simultaneous events.
// ============================================================================

TEST("duplicate K1 press without release uses latest start (resilient)") {
  DualGestureTracker t;
  t.on_press(1, 1000);
  t.on_press(1, 2000);  // second press without a release
  REQUIRE_EQ(t.on_release(1, 4500), 2500u);  // measured from 2nd press
}

TEST("simultaneous presses: dual starts at the later of the two") {
  DualGestureTracker t;
  t.on_press(1, 5000);
  t.on_press(2, 5000);  // exact same timestamp
  REQUIRE_EQ(t.dual_held_ms_now(15000), 10000u);
  REQUIRE_EQ(t.on_release(1, 15000), 0u);
  REQUIRE_EQ(t.take_pending_dual_held_ms(), 10000u);
}

// ============================================================================
// 8. Commit threshold constant exists and matches design (10 seconds).
// ============================================================================

TEST("COMMIT_MS constant is 10s") {
  REQUIRE_EQ(DualGestureTracker::COMMIT_MS, 10000u);
}

int main() { return tu::run_all(); }
