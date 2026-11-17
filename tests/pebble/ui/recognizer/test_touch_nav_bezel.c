/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/kernel/time.h>
#include <pbl/services/touch/touch.h>
#include <pbl/util/trig.h>

#include <applib/ui/layer.h>
#include <applib/ui/recognizer/recognizer.h>
#include <applib/ui/recognizer/recognizer_list.h>
#include <applib/ui/recognizer/recognizer_manager.h>
#include <applib/ui/recognizer/recognizer_private.h>
#include <applib/ui/recognizer/touch_nav.h>
#include <applib/ui/window.h>
#include <clar.h>

// Fakes
#include <fake_rtc.h>
#include <stubs_app_install_manager.h>

// Stubs
#include <pbl/util/units.h>

// The test build is a 260x260 round screen (see CMakeLists.txt).
#define CENTER      (130)
#define EDGE_RADIUS (120) // inside every ring width (the thinnest starts at radius 112)
#define STEP_DEG    CONFIG_TOUCH_NAV_BEZEL_STEP_DEG

// ---------------------------------------------------------------------------------------------
// Collaborator stubs the recognizer manager needs (mirrors test_touch_nav.c).

static Layer *s_active_layer;
static RecognizerManager *s_manager;

RecognizerList *window_get_recognizer_list(Window *window) {
  return nullptr;
}

RecognizerManager *window_get_recognizer_manager(Window *window) {
  return s_manager;
}

struct Layer *window_get_root_layer(const Window *window) {
  if (!window) {
    return nullptr;
  }
  return &((Window *)window)->layer;
}

Layer *layer_find_layer_containing_point(const Layer *node, const GPoint *point) {
  return s_active_layer;
}

// ---------------------------------------------------------------------------------------------
// Gates, controlled by the tests.

static bool s_nav_enabled;
static bool s_bezel_enabled;

bool sys_touch_nav_enabled(void) {
  return s_nav_enabled;
}

bool sys_touch_app_nav_active(void) {
  return false;
}

bool sys_touch_bezel_nav_enabled(void) {
  return s_bezel_enabled;
}

static uint8_t s_haptic;
static uint8_t s_width;

uint8_t sys_touch_bezel_width(void) {
  return s_width;
}

uint8_t sys_touch_bezel_haptic(void) {
  return s_haptic;
}

// ---------------------------------------------------------------------------------------------
// Fake vibe pattern service, recording the haptic ticks.

typedef struct FakeVibe {
  bool busy; // another pattern is playing: enqueues are refused
  int enqueue_count;
  int trigger_count;
  uint32_t last_duration_ms;
  bool last_on;
} FakeVibe;

static FakeVibe s_vibe;

bool sys_vibe_pattern_enqueue_step(uint32_t step_duration_ms, bool on) {
  if (s_vibe.busy) {
    return false;
  }
  s_vibe.enqueue_count++;
  s_vibe.last_duration_ms = step_duration_ms;
  s_vibe.last_on = on;
  return true;
}

void sys_vibe_pattern_trigger_start(void) {
  s_vibe.trigger_count++;
}

// ---------------------------------------------------------------------------------------------
// Fake TouchNavOps, recording every emitted button.

typedef struct FakeOps {
  bool animating;
  bool bridge_disabled;
  int pop_count;
  int emit_count;
  int up_count;
  int down_count;
  ButtonId last_emit;
  bool last_emit_was_bezel_step;
} FakeOps;

static FakeOps s_fake;

static bool prv_is_animating(void *ctx) {
  return ((FakeOps *)ctx)->animating;
}
static bool prv_top_overrides_back(void *ctx) {
  return false;
}
static bool prv_top_bridge_disabled(void *ctx) {
  return ((FakeOps *)ctx)->bridge_disabled;
}
static void prv_pop_top(void *ctx) {
  ((FakeOps *)ctx)->pop_count++;
}
static TouchNavState s_state;

static void prv_emit_button(void *ctx, ButtonId button) {
  FakeOps *ops = ctx;
  ops->last_emit_was_bezel_step = touch_nav_is_bezel_step(&s_state);
  ops->emit_count++;
  ops->last_emit = button;
  if (button == BUTTON_ID_UP) {
    ops->up_count++;
  } else if (button == BUTTON_ID_DOWN) {
    ops->down_count++;
  }
}

static TouchNavOps s_ops;

// ---------------------------------------------------------------------------------------------
// Fixture

static Window s_window;
static Layer s_child_layer;
static RecognizerList s_global_list;
static RecognizerManager s_recognizer_manager;

void test_touch_nav_bezel__initialize(void) {
  fake_rtc_init(0, 0);
  s_nav_enabled = true;
  s_bezel_enabled = true;
  s_haptic = TouchBezelHaptic_Light;
  s_width = TouchBezelWidth_Standard;
  s_vibe = (FakeVibe){};
  s_active_layer = nullptr;
  s_fake = (FakeOps){};
  s_ops = (TouchNavOps){
    .is_animating = prv_is_animating,
    .top_overrides_back = prv_top_overrides_back,
    .top_bridge_disabled = prv_top_bridge_disabled,
    .pop_top = prv_pop_top,
    .emit_button = prv_emit_button,
    .ctx = &s_fake,
  };

  s_window = (Window){};
  layer_init(&s_window.layer, &GRectZero);
  layer_init(&s_child_layer, &GRectZero);
  layer_add_child(&s_window.layer, &s_child_layer);

  recognizer_list_init(&s_global_list);
  recognizer_manager_init(&s_recognizer_manager);
  s_recognizer_manager.window = &s_window;
  s_recognizer_manager.global_list = &s_global_list;
  s_manager = &s_recognizer_manager;

  touch_nav_state_init(&s_state, &s_recognizer_manager, &s_ops);
}

void test_touch_nav_bezel__cleanup(void) {
  touch_nav_state_deinit(&s_state);
}

// ---------------------------------------------------------------------------------------------
// Helpers

static void prv_dispatch(TouchEventType type, int16_t x, int16_t y) {
  const TouchEvent e = {.type = type, .x = x, .y = y};
  touch_nav_dispatch(&e, &s_state);
}

static void prv_advance_ms(uint32_t ms) {
  fake_rtc_increment_ticks((uint64_t)ms * PBL_TICK_HZ / PBL_MSEC_PER_SEC);
}

//! Point at \a deg degrees clockwise from 3 o'clock on a circle of \a radius around the center.
static GPoint prv_on_circle(int32_t deg, int32_t radius) {
  const int32_t angle = DEG_TO_TRIGANGLE(deg);
  return GPoint(CENTER + radius * cos_lookup(angle) / TRIG_MAX_RATIO,
                CENTER + radius * sin_lookup(angle) / TRIG_MAX_RATIO);
}

//! Touch down at \a from_deg on the edge and slide to \a to_deg in 3 degree samples, without
//! lifting.
static void prv_rotate(int32_t from_deg, int32_t to_deg) {
  GPoint p = prv_on_circle(from_deg, EDGE_RADIUS);
  prv_dispatch(TouchEvent_Touchdown, p.x, p.y);
  const int32_t dir = (to_deg >= from_deg) ? 3 : -3;
  for (int32_t deg = from_deg + dir; (dir > 0) ? deg <= to_deg : deg >= to_deg; deg += dir) {
    prv_advance_ms(10);
    p = prv_on_circle(deg, EDGE_RADIUS);
    prv_dispatch(TouchEvent_PositionUpdate, p.x, p.y);
  }
}

static void prv_liftoff(void) {
  prv_advance_ms(10);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
}

// ---------------------------------------------------------------------------------------------
// Geometry

void test_touch_nav_bezel__ring(void) {
  cl_assert(!touch_nav_bezel_in_ring(GPoint(130, 130), 260, 260, 36));
  cl_assert(!touch_nav_bezel_in_ring(GPoint(130 + 93, 130), 260, 260, 36));
  cl_assert(touch_nav_bezel_in_ring(GPoint(130 + 94, 130), 260, 260, 36));
  cl_assert(touch_nav_bezel_in_ring(GPoint(130, 2), 260, 260, 36));
  cl_assert(touch_nav_bezel_in_ring(GPoint(0, 0), 260, 260, 36));
}

void test_touch_nav_bezel__angle_is_clockwise(void) {
  cl_assert_equal_i(touch_nav_bezel_angle(GPoint(250, 130), 260, 260), 0);
  // Below the center is a quarter turn clockwise on screen.
  cl_assert_equal_i(TRIGANGLE_TO_DEG(touch_nav_bezel_angle(GPoint(130, 250), 260, 260)), 90);
  cl_assert_equal_i(TRIGANGLE_TO_DEG(touch_nav_bezel_angle(GPoint(130, 10), 260, 260)), 270);
}

void test_touch_nav_bezel__angle_delta_wraps(void) {
  cl_assert_equal_i(touch_nav_bezel_angle_delta(DEG_TO_TRIGANGLE(350), DEG_TO_TRIGANGLE(10)),
                    DEG_TO_TRIGANGLE(10) - DEG_TO_TRIGANGLE(350) + TRIG_MAX_ANGLE);
  cl_assert(touch_nav_bezel_angle_delta(DEG_TO_TRIGANGLE(10), DEG_TO_TRIGANGLE(350)) < 0);
  cl_assert_equal_i(touch_nav_bezel_angle_delta(DEG_TO_TRIGANGLE(90), DEG_TO_TRIGANGLE(90)), 0);
}

// ---------------------------------------------------------------------------------------------
// Rotation

void test_touch_nav_bezel__clockwise_emits_down_per_step(void) {
  // From 12 o'clock to 3 o'clock, clockwise.
  prv_rotate(-90, 0);
  cl_assert_equal_i(s_state.route, TouchNavRoute_Bezel);
  cl_assert_equal_i(s_fake.down_count, 90 / STEP_DEG);
  cl_assert_equal_i(s_fake.up_count, 0);
  prv_liftoff();
  cl_assert_equal_i(s_state.bezel.phase, TouchNavBezelPhase_Idle);
  // The rotation never reached the recognizers.
  cl_assert_equal_i(s_state.counters.completed, 0);
  cl_assert_equal_i(s_state.counters.started, 0);
}

void test_touch_nav_bezel__counter_clockwise_emits_up(void) {
  prv_rotate(90, 0);
  cl_assert_equal_i(s_fake.up_count, 90 / STEP_DEG);
  cl_assert_equal_i(s_fake.down_count, 0);
  prv_liftoff();
}

void test_touch_nav_bezel__reversing_direction(void) {
  // Sample points are whole pixels, so allow a few degrees over each multiple of the step.
  prv_rotate(0, 63);
  cl_assert_equal_i(s_fake.down_count, 4);
  for (int32_t deg = 60; deg >= -3; deg -= 3) {
    prv_advance_ms(10);
    const GPoint p = prv_on_circle(deg, EDGE_RADIUS);
    prv_dispatch(TouchEvent_PositionUpdate, p.x, p.y);
  }
  cl_assert_equal_i(s_fake.up_count, 4);
  cl_assert_equal_i(s_fake.down_count, 4);
  prv_liftoff();
}

void test_touch_nav_bezel__crosses_angle_wrap(void) {
  // Clockwise through 3 o'clock, where the angle wraps from 360 back to 0.
  prv_rotate(-45, 45);
  cl_assert_equal_i(s_fake.down_count, 90 / STEP_DEG);
  prv_liftoff();
}

void test_touch_nav_bezel__active_follows_finger_inward(void) {
  prv_rotate(0, 30);
  const int before = s_fake.down_count;
  // Once committed, the finger may drift off the ring and still turn the bezel.
  for (int32_t deg = 33; deg <= 60; deg += 3) {
    prv_advance_ms(10);
    const GPoint p = prv_on_circle(deg, 70);
    prv_dispatch(TouchEvent_PositionUpdate, p.x, p.y);
  }
  cl_assert(s_fake.down_count > before);
  prv_liftoff();
}

void test_touch_nav_bezel__below_commit_emits_nothing(void) {
  prv_rotate(0, CONFIG_TOUCH_NAV_BEZEL_COMMIT_DEG - 3);
  cl_assert_equal_i(s_state.bezel.phase, TouchNavBezelPhase_Pending);
  cl_assert_equal_i(s_fake.emit_count, 0);
}

void test_touch_nav_bezel__steps_dropped_while_animating(void) {
  s_fake.animating = true;
  prv_rotate(0, 45);
  cl_assert_equal_i(s_fake.emit_count, 0);
  cl_assert_equal_i(s_state.counters.dropped, 45 / STEP_DEG);
  prv_liftoff();
}

// ---------------------------------------------------------------------------------------------
// Ordinary gestures that start at the edge are replayed to the recognizers.

void test_touch_nav_bezel__edge_tap_is_select(void) {
  const GPoint p = prv_on_circle(0, EDGE_RADIUS);
  prv_dispatch(TouchEvent_Touchdown, p.x, p.y);
  cl_assert_equal_i(s_state.route, TouchNavRoute_Bezel);
  prv_advance_ms(30);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(s_state.route, TouchNavRoute_Tier2);
  cl_assert_equal_i(s_fake.emit_count, 1);
  cl_assert_equal_i(s_fake.last_emit, BUTTON_ID_SELECT);
}

void test_touch_nav_bezel__long_edge_hold_does_nothing(void) {
  const GPoint p = prv_on_circle(0, EDGE_RADIUS);
  prv_dispatch(TouchEvent_Touchdown, p.x, p.y);
  prv_advance_ms(800);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(s_fake.emit_count, 0);
  cl_assert_equal_i(s_fake.pop_count, 0);
}

void test_touch_nav_bezel__inward_swipe_from_edge_is_back(void) {
  // From the left edge toward the center: an ordinary left-to-right swipe (BACK, pops).
  prv_dispatch(TouchEvent_Touchdown, 12, 130);
  cl_assert_equal_i(s_state.route, TouchNavRoute_Bezel);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 60, 130);
  cl_assert_equal_i(s_state.bezel.phase, TouchNavBezelPhase_Idle);
  cl_assert_equal_i(s_state.route, TouchNavRoute_Tier2);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 110, 130);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(s_fake.pop_count, 1);
  cl_assert_equal_i(s_fake.emit_count, 0);
}

void test_touch_nav_bezel__center_swipe_unaffected(void) {
  prv_dispatch(TouchEvent_Touchdown, 130, 170);
  cl_assert_equal_i(s_state.route, TouchNavRoute_Tier2);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 130, 130);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 130, 90);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(s_fake.emit_count, 1);
  cl_assert_equal_i(s_fake.last_emit, BUTTON_ID_DOWN);
}

// ---------------------------------------------------------------------------------------------
// Gates

void test_touch_nav_bezel__pref_off_routes_normally(void) {
  s_bezel_enabled = false;
  prv_rotate(-90, 0);
  cl_assert(s_state.route != TouchNavRoute_Bezel);
  cl_assert_equal_i(s_state.bezel.phase, TouchNavBezelPhase_Idle);
  // No step emits during the slide: at most one ordinary swipe completion on liftoff.
  cl_assert_equal_i(s_fake.emit_count + s_fake.pop_count, 0);
  prv_liftoff();
  cl_assert(s_fake.emit_count + s_fake.pop_count <= 1);
}

void test_touch_nav_bezel__system_nav_off_is_inert(void) {
  s_nav_enabled = false;
  prv_rotate(-90, 0);
  prv_liftoff();
  cl_assert_equal_i(s_fake.emit_count, 0);
}

void test_touch_nav_bezel__bridge_disabled_window_gets_no_bezel(void) {
  s_fake.bridge_disabled = true;
  // Start at 3 o'clock, clear of the status-bar dead zone at the top.
  prv_rotate(0, 90);
  cl_assert_equal_i(s_state.route, TouchNavRoute_None);
  prv_liftoff();
  cl_assert_equal_i(s_fake.emit_count, 0);
}

void test_touch_nav_bezel__bridge_disabled_mid_rotation_drops(void) {
  prv_rotate(0, 30);
  const int before = s_fake.emit_count;
  s_fake.bridge_disabled = true;
  for (int32_t deg = 33; deg <= 90; deg += 3) {
    prv_advance_ms(10);
    const GPoint p = prv_on_circle(deg, EDGE_RADIUS);
    prv_dispatch(TouchEvent_PositionUpdate, p.x, p.y);
  }
  cl_assert_equal_i(s_fake.emit_count, before);
  prv_liftoff();
}

void test_touch_nav_bezel__touchdown_resets_stale_gesture(void) {
  // A rotation whose Liftoff was lost does not leak into the next gesture.
  prv_rotate(0, 30);
  prv_dispatch(TouchEvent_Touchdown, 130, 170);
  cl_assert_equal_i(s_state.bezel.phase, TouchNavBezelPhase_Idle);
  cl_assert_equal_i(s_state.route, TouchNavRoute_Tier2);
}

// ---------------------------------------------------------------------------------------------
// Haptics

void test_touch_nav_bezel__each_step_ticks(void) {
  prv_rotate(-90, 0);
  cl_assert_equal_i(s_vibe.trigger_count, 90 / STEP_DEG);
  cl_assert_equal_i(s_vibe.enqueue_count, 90 / STEP_DEG);
  cl_assert_equal_i(s_vibe.last_duration_ms, 15);
  cl_assert(s_vibe.last_on);
  prv_rotate(90, 0);
  cl_assert_equal_i(s_vibe.trigger_count, 2 * (90 / STEP_DEG));
  prv_liftoff();
}

void test_touch_nav_bezel__no_tick_below_a_step(void) {
  prv_rotate(0, STEP_DEG - 3);
  cl_assert_equal_i(s_vibe.trigger_count, 0);
  prv_liftoff();
}

void test_touch_nav_bezel__dropped_steps_do_not_tick(void) {
  s_fake.animating = true;
  prv_rotate(0, 45);
  cl_assert_equal_i(s_vibe.trigger_count, 0);
  prv_liftoff();
}

void test_touch_nav_bezel__bridge_disabled_steps_do_not_tick(void) {
  prv_rotate(0, 30);
  const int before = s_vibe.trigger_count;
  s_fake.bridge_disabled = true;
  for (int32_t deg = 33; deg <= 90; deg += 3) {
    prv_advance_ms(10);
    const GPoint p = prv_on_circle(deg, EDGE_RADIUS);
    prv_dispatch(TouchEvent_PositionUpdate, p.x, p.y);
  }
  cl_assert_equal_i(s_vibe.trigger_count, before);
  prv_liftoff();
}

void test_touch_nav_bezel__busy_vibe_is_left_alone(void) {
  // A notification is vibrating: the steps still scroll, but no tick is started over it.
  s_vibe.busy = true;
  prv_rotate(0, 45);
  cl_assert_equal_i(s_fake.down_count, 45 / STEP_DEG);
  cl_assert_equal_i(s_vibe.trigger_count, 0);
  prv_liftoff();
}

void test_touch_nav_bezel__edge_tap_does_not_tick(void) {
  const GPoint p = prv_on_circle(0, EDGE_RADIUS);
  prv_dispatch(TouchEvent_Touchdown, p.x, p.y);
  prv_advance_ms(30);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(s_fake.last_emit, BUTTON_ID_SELECT);
  cl_assert_equal_i(s_vibe.trigger_count, 0);
}

void test_touch_nav_bezel__haptic_strengths(void) {
  cl_assert_equal_i(touch_nav_bezel_haptic_ms(TouchBezelHaptic_Off), 0);
  cl_assert(touch_nav_bezel_haptic_ms(TouchBezelHaptic_Light) <
            touch_nav_bezel_haptic_ms(TouchBezelHaptic_Medium));
  cl_assert(touch_nav_bezel_haptic_ms(TouchBezelHaptic_Medium) <
            touch_nav_bezel_haptic_ms(TouchBezelHaptic_Strong));
  // An unknown stored value falls back to Light.
  cl_assert_equal_i(touch_nav_bezel_haptic_ms(200),
                    touch_nav_bezel_haptic_ms(TouchBezelHaptic_Light));
}

void test_touch_nav_bezel__haptic_off_scrolls_silently(void) {
  s_haptic = TouchBezelHaptic_Off;
  prv_rotate(0, 45);
  cl_assert_equal_i(s_fake.down_count, 45 / STEP_DEG);
  cl_assert_equal_i(s_vibe.trigger_count, 0);
  prv_liftoff();
}

void test_touch_nav_bezel__haptic_strong_is_longer(void) {
  s_haptic = TouchBezelHaptic_Strong;
  prv_rotate(0, 20);
  cl_assert_equal_i(s_vibe.last_duration_ms, touch_nav_bezel_haptic_ms(TouchBezelHaptic_Strong));
  prv_liftoff();
}

// ---------------------------------------------------------------------------------------------
// What the app's own touch handler gets

void test_touch_nav_bezel__raw_rotation_is_held_then_consumed(void) {
  GPoint p = prv_on_circle(0, EDGE_RADIUS);
  prv_dispatch(TouchEvent_Touchdown, p.x, p.y);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Hold);
  prv_advance_ms(10);
  p = prv_on_circle(3, EDGE_RADIUS);
  prv_dispatch(TouchEvent_PositionUpdate, p.x, p.y);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Hold);
  prv_advance_ms(10);
  p = prv_on_circle(12, EDGE_RADIUS);
  prv_dispatch(TouchEvent_PositionUpdate, p.x, p.y);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Consume);
  prv_liftoff();
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Consume);
}

void test_touch_nav_bezel__raw_edge_tap_is_released(void) {
  const GPoint p = prv_on_circle(0, EDGE_RADIUS);
  prv_dispatch(TouchEvent_Touchdown, p.x, p.y);
  prv_advance_ms(30);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Release);
}

void test_touch_nav_bezel__raw_long_edge_hold_is_released(void) {
  const GPoint p = prv_on_circle(0, EDGE_RADIUS);
  prv_dispatch(TouchEvent_Touchdown, p.x, p.y);
  prv_advance_ms(800);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Release);
}

void test_touch_nav_bezel__raw_center_touch_passes(void) {
  prv_dispatch(TouchEvent_Touchdown, 130, 170);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Pass);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 130, 120);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Pass);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Pass);
}

// ---------------------------------------------------------------------------------------------
// Apps picked for the bezel

void test_touch_nav_bezel__picked_app_with_own_touch_gets_bezel(void) {
  // The app handles touch itself (bridge disabled), but the user picked it for the bezel.
  s_fake.bridge_disabled = true;
  s_state.bezel_app = true;
  prv_rotate(0, 45);
  cl_assert_equal_i(s_state.route, TouchNavRoute_Bezel);
  cl_assert_equal_i(s_fake.down_count, 45 / STEP_DEG);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Consume);
  prv_liftoff();
}

void test_touch_nav_bezel__picked_app_center_swipe_is_the_apps(void) {
  s_fake.bridge_disabled = true;
  s_state.bezel_app = true;
  prv_dispatch(TouchEvent_Touchdown, 130, 170);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 130, 90);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(s_fake.emit_count, 0);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Pass);
}

void test_touch_nav_bezel__bezel_only_synthesizes_nothing_else(void) {
  s_state.bezel_app = true;
  s_state.bezel_only = true;
  // A center swipe and an edge tap are left to the app: no buttons, no recognizers.
  prv_dispatch(TouchEvent_Touchdown, 130, 170);
  cl_assert_equal_i(s_state.route, TouchNavRoute_None);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 130, 90);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  const GPoint p = prv_on_circle(0, EDGE_RADIUS);
  prv_dispatch(TouchEvent_Touchdown, p.x, p.y);
  prv_advance_ms(30);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Release);
  cl_assert_equal_i(s_fake.emit_count, 0);
  cl_assert_equal_i(s_fake.pop_count, 0);
  cl_assert_equal_i(s_state.counters.completed, 0);
  // A rotation still scrolls.
  prv_rotate(0, 45);
  cl_assert_equal_i(s_fake.down_count, 45 / STEP_DEG);
  prv_liftoff();
}

void test_touch_nav_bezel__bezel_only_inward_drag_is_released(void) {
  s_state.bezel_app = true;
  s_state.bezel_only = true;
  prv_dispatch(TouchEvent_Touchdown, 12, 130);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 70, 130);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Release);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 110, 130);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Pass);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(s_fake.pop_count, 0);
  cl_assert_equal_i(s_fake.emit_count, 0);
}

// ---------------------------------------------------------------------------------------------
// Ring width

void test_touch_nav_bezel__widths_grow(void) {
  cl_assert(touch_nav_bezel_ring_px(TouchBezelWidth_Thin) <
            touch_nav_bezel_ring_px(TouchBezelWidth_Standard));
  cl_assert(touch_nav_bezel_ring_px(TouchBezelWidth_Standard) <
            touch_nav_bezel_ring_px(TouchBezelWidth_Large));
  // An unknown stored value falls back to Standard.
  cl_assert_equal_i(touch_nav_bezel_ring_px(200),
                    touch_nav_bezel_ring_px(TouchBezelWidth_Standard));
  // The thinnest ring still holds the touches the other tests use.
  cl_assert(EDGE_RADIUS >= CENTER - touch_nav_bezel_ring_px(TouchBezelWidth_Thin));
}

//! Touch down \a inset pixels in from the right edge and report whether the bezel took it.
static bool prv_starts_bezel_at_inset(int16_t inset) {
  prv_dispatch(TouchEvent_Touchdown, 2 * CENTER - inset, CENTER);
  const bool bezel = (s_state.route == TouchNavRoute_Bezel);
  prv_advance_ms(30);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  prv_advance_ms(500);
  return bezel;
}

void test_touch_nav_bezel__width_sets_where_rotation_starts(void) {
  const int16_t thin = touch_nav_bezel_ring_px(TouchBezelWidth_Thin);
  const int16_t large = touch_nav_bezel_ring_px(TouchBezelWidth_Large);
  const int16_t between = (thin + large) / 2;

  s_width = TouchBezelWidth_Thin;
  cl_assert(prv_starts_bezel_at_inset(thin - 2));
  cl_assert(!prv_starts_bezel_at_inset(between));

  s_width = TouchBezelWidth_Large;
  cl_assert(prv_starts_bezel_at_inset(between));
  cl_assert(!prv_starts_bezel_at_inset(large + 2));
}

void test_touch_nav_bezel__thin_ring_hands_inward_drag_back_sooner(void) {
  // The inward exit follows the ring the gesture started in.
  s_width = TouchBezelWidth_Thin;
  const int16_t exit_inset = touch_nav_bezel_ring_px(TouchBezelWidth_Thin) + 12;
  prv_dispatch(TouchEvent_Touchdown, 2 * CENTER - 8, CENTER);
  cl_assert_equal_i(s_state.route, TouchNavRoute_Bezel);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 2 * CENTER - exit_inset - 2, CENTER);
  cl_assert_equal_i(s_state.bezel.phase, TouchNavBezelPhase_Idle);
  cl_assert_equal_i(touch_nav_raw_disposition(&s_state), TouchRawDisposition_Release);
}

// ---------------------------------------------------------------------------------------------
// Telling a bezel step from other input

void test_touch_nav_bezel__steps_are_flagged(void) {
  prv_rotate(0, 30);
  cl_assert(s_fake.down_count > 0);
  cl_assert(s_fake.last_emit_was_bezel_step);
  prv_liftoff();
  cl_assert(!touch_nav_is_bezel_step(&s_state));
}

void test_touch_nav_bezel__swipes_and_taps_are_not_flagged(void) {
  prv_dispatch(TouchEvent_Touchdown, 130, 170);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 130, 130);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_PositionUpdate, 130, 90);
  prv_advance_ms(20);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(s_fake.last_emit, BUTTON_ID_DOWN);
  cl_assert(!s_fake.last_emit_was_bezel_step);

  const GPoint p = prv_on_circle(0, EDGE_RADIUS);
  prv_advance_ms(500);
  prv_dispatch(TouchEvent_Touchdown, p.x, p.y);
  prv_advance_ms(30);
  prv_dispatch(TouchEvent_Liftoff, 0, 0);
  cl_assert_equal_i(s_fake.last_emit, BUTTON_ID_SELECT);
  cl_assert(!s_fake.last_emit_was_bezel_step);
}

void test_touch_nav_bezel__no_state_is_not_a_step(void) {
  cl_assert(!touch_nav_is_bezel_step(nullptr));
}
