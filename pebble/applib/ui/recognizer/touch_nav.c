/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "touch_nav.h"

#include "pan.h"
#include "recognizer.h"
#include "recognizer_manager.h"
#include "recognizer_private.h"
#include "swipe.h"
#include "tap.h"

#include <stddef.h>

#include <pbl/kernel/assert.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/services/touch/touch.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>
#include <pbl/util/trig.h>

#include <applib/graphics/gtypes.h>
#include <applib/ui/layer.h>
#include <syscall/syscall.h>

// All four directions are accepted by the system swipe recognizer; the bridge maps each one.
#define TOUCH_NAV_SWIPE_MASK \
  (SwipeDirection_Up | SwipeDirection_Down | SwipeDirection_Left | SwipeDirection_Right)

// The unified widget set is a content scroller (vertical pan) with horizontal-only swipe
// navigation; vertical swipes stay pans so a flick scrolls/flings instead of being eaten by a
// swipe.
#define TOUCH_NAV_WIDGET_SWIPE_MASK (SwipeDirection_Left | SwipeDirection_Right)

// Minimum wall-clock spacing between live-scroll updates applied from pan Updated events, matching
// the per-widget sets this unifies.
#define TOUCH_NAV_WIDGET_PAN_UPDATE_MIN_INTERVAL_MS 16

// ---------------------------------------------------------------------------------------------
// Observability

static void prv_log_push(TouchNavState *state, TouchNavLogKind kind, uint8_t detail) {
  state->log[state->log_head] = (TouchNavLogEntry){.kind = (uint8_t)kind, .detail = detail};
  state->log_head = (uint8_t)((state->log_head + 1) % TOUCH_NAV_LOG_ENTRIES);
  if (state->log_count < TOUCH_NAV_LOG_ENTRIES) {
    state->log_count++;
  }
}

// ---------------------------------------------------------------------------------------------
// Tier-1 widget registry (intrusive, no limit)

static TouchNavWidgetNode **prv_registry_head(TouchNavState *state, TouchNavWidgetType type) {
  switch (type) {
    case TouchNavWidgetType_Menu:
      return &state->menu_head;
    case TouchNavWidgetType_Scroll:
      return &state->scroll_head;
    case TouchNavWidgetType_Swap:
    default:
      return &state->swap_head;
  }
}

void touch_nav_registry_add(TouchNavState *state, TouchNavWidgetType type, TouchNavWidgetNode *node,
                            struct Layer *layer, const TouchNavWidgetOps *ops, void *widget) {
  if (!state || !node) {
    return;
  }
  TouchNavWidgetNode **head = prv_registry_head(state, type);
  // Dedupe by address: a re-add is a WARN and a no-op. Walk with a predecessor so we never touch a
  // node that isn't already threaded onto this list (robust to *_init zeroing the node).
  for (TouchNavWidgetNode *cur = *head; cur; cur = cur->next) {
    if (cur == node) {
      PBL_LOG_WRN("touch_nav: widget node %p re-added", (void *)node);
      // A widget re-init zeroes the node while it is still threaded here; restore the layer/vtable
      // so an init-without-deinit keeps routing to and driving the widget instead of matching a
      // NULL layer or dispatching through a NULL vtable.
      cur->layer = layer;
      cur->ops = ops;
      cur->widget = widget;
      return;
    }
  }
  node->layer = layer;
  node->ops = ops;
  node->widget = widget;
  node->next = *head;
  *head = node;
}

void touch_nav_registry_remove(TouchNavState *state, TouchNavWidgetType type,
                               TouchNavWidgetNode *node) {
  if (!state || !node) {
    return;
  }
  // UAF hook: if the node being removed is the latched widget target, drop the weak ref BEFORE the
  // caller cancels any in-flight gesture, so the resulting Cancelled dispatch cannot reach the
  // widget's (now torn-down) apply logic.
  if (state->latched_target == node) {
    state->latched_target = nullptr;
  }
  TouchNavWidgetNode **head = prv_registry_head(state, type);
  TouchNavWidgetNode *prev = nullptr;
  for (TouchNavWidgetNode *cur = *head; cur; prev = cur, cur = cur->next) {
    if (cur == node) {
      if (prev) {
        prev->next = cur->next;
      } else {
        *head = cur->next;
      }
      node->next = nullptr;
      return;
    }
  }
  // Not present: a safe no-op (e.g. the node was zeroed by *_init and never added).
}

// Whether \a node is still threaded onto any registry list (weak-ref re-validation for the latched
// widget target). A node zeroed/removed by a widget deinit mid-gesture will not be found.
static bool prv_registry_contains_node(TouchNavState *state, const TouchNavWidgetNode *node) {
  const TouchNavWidgetNode *const heads[] = {
    state->menu_head, state->swap_head, state->scroll_head
  };
  for (unsigned i = 0; i < ARRAY_LENGTH(heads); i++) {
    for (const TouchNavWidgetNode *n = heads[i]; n; n = n->next) {
      if (n == node) {
        return true;
      }
    }
  }
  return false;
}

static bool prv_registry_contains_layer(TouchNavState *state, const struct Layer *layer) {
  const TouchNavWidgetNode *const heads[] = {
    state->menu_head, state->scroll_head, state->swap_head
  };
  for (unsigned i = 0; i < ARRAY_LENGTH(heads); i++) {
    for (const TouchNavWidgetNode *n = heads[i]; n; n = n->next) {
      if (n->layer == layer) {
        return true;
      }
    }
  }
  return false;
}

// @return the single registered widget layer across both registries, or NULL if there is not
// exactly one.
static struct Layer *prv_registry_sole_widget(TouchNavState *state) {
  struct Layer *sole = nullptr;
  uint32_t count = 0;
  for (TouchNavWidgetNode *n = state->menu_head; n; n = n->next) {
    sole = n->layer;
    count++;
  }
  for (TouchNavWidgetNode *n = state->swap_head; n; n = n->next) {
    sole = n->layer;
    count++;
  }
  for (TouchNavWidgetNode *n = state->scroll_head; n; n = n->next) {
    sole = n->layer;
    count++;
  }
  return (count == 1) ? sole : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Action-bar tap zoning

void touch_nav_set_action_bar(TouchNavState *state, const GRect *frame, uint8_t icon_mask) {
  if (!state) {
    return;
  }
  if (frame) {
    state->action_bar = (TouchNavActionBar){
      .frame = *frame,
      .icon_mask = icon_mask,
      .present = true,
    };
  } else {
    state->action_bar = (TouchNavActionBar){.present = false};
  }
}

ButtonId touch_nav_action_bar_zone_button(const TouchNavActionBar *bar, GPoint point,
                                          bool require_icon_zone) {
  // The fallback for any tap that does not land on an icon zone: a plain SELECT normally, or no
  // button at all for a window that only accepts taps on the bar's icons.
  const ButtonId fallback = require_icon_zone ? NUM_BUTTONS : BUTTON_ID_SELECT;
  // No bar (or a degenerate frame) / a tap outside the bar.
  if (!bar->present || bar->frame.size.h <= 0 || !grect_contains_point(&bar->frame, &point)) {
    return fallback;
  }
  // Split the bar vertically into three equal zones with half-open bounds: top = UP, middle =
  // SELECT, bottom = DOWN. zone i maps to icon bit i and to button (BUTTON_ID_UP + i).
  int zone = (point.y - bar->frame.origin.y) * 3 / bar->frame.size.h;
  zone = CLIP(zone, 0, 2);
  if (!(bar->icon_mask & (1 << zone))) {
    // The zone has no icon.
    return fallback;
  }
  return (ButtonId)(BUTTON_ID_UP + zone);
}

// ---------------------------------------------------------------------------------------------
// Unified widget routing

// Resolve the migrated widget the current gesture belongs to, or NULL.
//
// Walk the latched active layer up its parent chain (leaf -> root). At EACH layer, check the
// registry heads in menu -> scroll -> swap order and stop at the FIRST registered node found: the
// first registered node on the walk wins, and the head order is only a same-layer tiebreak, never a
// cross-depth precedence (a deeper widget always beats a shallower one). This deliberately replaces
// the old attach-order arbitration. If that winning node is un-migrated (ops == NULL) the gesture
// belongs to the widget's own recognizer set, so return NULL and let that set drive it -- the
// unified set only ever drives ops-bearing (migrated) widgets.
static TouchNavWidgetNode *prv_resolve_widget_target(TouchNavState *state) {
  if (!state || !state->manager) {
    return nullptr;
  }
  TouchNavWidgetNode *const heads[] = {state->menu_head, state->scroll_head, state->swap_head};
  for (struct Layer *layer = state->manager->active_layer; layer; layer = layer->parent) {
    for (unsigned i = 0; i < ARRAY_LENGTH(heads); i++) {
      for (TouchNavWidgetNode *n = heads[i]; n; n = n->next) {
        if (n->layer == layer) {
          return n->ops ? n : nullptr;
        }
      }
    }
  }
  return nullptr;
}

// Touch filter for the unified widget set: handle a gesture only when the latched active layer
// resolves to a migrated (ops-bearing) widget. A filtered-out recognizer returns without advancing,
// so on a bridge / un-migrated-widget route it never Starts and never competes with the bridge set
// or an un-migrated widget's own set on this task's global recognizer list.
static bool prv_widget_touch_filter(const Recognizer *recognizer, const TouchEvent *touch_event) {
  TouchNavState *state = recognizer_get_user_data(recognizer);
  return prv_resolve_widget_target(state) != nullptr;
}

// Unified widget dispatch. Mirrors the per-widget recognizer callbacks this replaces, but drives
// the latched target through its \ref TouchNavWidgetOps vtable instead of a concrete widget type.
// The target is latched on Touchdown (in touch_nav_dispatch) and re-validated as a weak ref here on
// every later event so a widget torn down mid-gesture is dropped with no callback into freed state.
static void prv_widget_recognizer_event(const Recognizer *recognizer, RecognizerEvent event) {
  TouchNavState *state = recognizer_get_user_data(recognizer);
  if (!state) {
    return;
  }
  TouchNavWidgetNode *node = state->latched_target;
  // Weak-ref re-validation: a NULL or no-longer-registered target means the widget went away
  // (UAF hook / deinit). Drop every event; nothing to clear (the latch is already gone).
  if (!node || !prv_registry_contains_node(state, node)) {
    return;
  }
  const TouchNavWidgetOps *ops = node->ops;
  void *w = node->widget;

  switch (event) {
    case RecognizerEvent_Started:
      if (recognizer == state->widget_pan) {
        if (ops->can_start && !ops->can_start(w)) {
          // The widget is not ready (e.g. its layout has not loaded). Decline the whole gesture
          // WITHOUT set_failed (the pan is already Started, not Possible) or cancel (we are inside
          // a Started callback -- re-entrancy). Every later vtable call is gated on `declined`.
          state->declined = true;
          return;
        }
        ops->pan_started(w);
        state->gesture_base = ops->get_base_offset(w);
        state->last_update_ticks = 0;
      }
      break;
    case RecognizerEvent_Updated:
      if (!state->declined && recognizer == state->widget_pan) {
        const uint64_t now = sys_get_ticks();
        const uint64_t min_ticks =
            (uint64_t)TOUCH_NAV_WIDGET_PAN_UPDATE_MIN_INTERVAL_MS * PBL_TICK_HZ / 1000;
        if (now - state->last_update_ticks >= min_ticks) {
          state->last_update_ticks = now;
          ops->pan_update(w, state->gesture_base,
                          pan_recognizer_get_delta_since_start((Recognizer *)recognizer));
        }
      }
      break;
    case RecognizerEvent_Completed:
      if (!state->declined) {
        if (recognizer == state->widget_pan) {
          ops->pan_snap(w, state->gesture_base,
                        pan_recognizer_get_delta_since_start((Recognizer *)recognizer),
                        pan_recognizer_get_velocity((Recognizer *)recognizer));
        } else if (recognizer == state->widget_tap) {
          if (ops->tap) {
            ops->tap(w, tap_recognizer_get_tap_point((Recognizer *)recognizer));
          }
        } else if (recognizer == state->widget_swipe) {
          const SwipeDirection dir = swipe_recognizer_get_direction((Recognizer *)recognizer);
          if (dir == SwipeDirection_Left || dir == SwipeDirection_Right) {
            ops->swipe(w, dir);
          }
        }
      }
      state->latched_target = nullptr;
      state->declined = false;
      break;
    case RecognizerEvent_Cancelled:
      if (!state->declined && recognizer == state->widget_pan) {
        ops->pan_cancel(w);
      }
      state->latched_target = nullptr;
      state->declined = false;
      break;
  }
}

// ---------------------------------------------------------------------------------------------
// Routing

static TouchNavRoute prv_resolve_route(TouchNavState *state, const TouchEvent *touchdown) {
  // Walk parents from the active layer; the first Tier-1 widget in the registry wins.
  for (struct Layer *layer = state->manager->active_layer; layer; layer = layer->parent) {
    if (prv_registry_contains_layer(state, layer)) {
      return TouchNavRoute_Tier1;
    }
  }

  // Status-bar dead zone: route to the window's sole widget, else drop.
  if (touchdown->y < TOUCH_NAV_STATUS_BAR_DEAD_ZONE_PX) {
    return prv_registry_sole_widget(state) ? TouchNavRoute_Tier1 : TouchNavRoute_Dropped;
  }

  // Otherwise the Tier-2 bridge, unless the window opted out.
  if (state->ops->top_bridge_disabled && state->ops->top_bridge_disabled(state->ops->ctx)) {
    return TouchNavRoute_None;
  }
  return TouchNavRoute_Tier2;
}

static void prv_fail(TouchNavState *state, Recognizer *recognizer) {
  // set_failed asserts the recognizer is Possible; only fail an as-yet-undecided one.
  if (recognizer_get_state(recognizer) == RecognizerState_Possible) {
    recognizer_set_failed(recognizer);
    state->counters.failed++;
  }
}

static void prv_apply_tier_exclusion(TouchNavState *state, TouchNavRoute route) {
  switch (route) {
    case TouchNavRoute_Tier2:
      // Fail pan so it does not Start at the 8px threshold and thereby fail the swipe.
      prv_fail(state, state->pan);
      break;
    case TouchNavRoute_None:
    case TouchNavRoute_Dropped:
    case TouchNavRoute_Tier1:
    case TouchNavRoute_Bezel:
      // Nobody / dead-zone drop / a Tier-1 widget owning the gesture: the system set must not
      // emulate, so fail all three.
      prv_fail(state, state->tap);
      prv_fail(state, state->pan);
      prv_fail(state, state->swipe);
      break;
  }
}

// ---------------------------------------------------------------------------------------------
// Bridge

static ButtonId prv_swipe_button(SwipeDirection direction) {
  // Content-scroll convention: the emulated button opposes finger travel, matching native
  // MenuLayer/ScrollLayer touch. Finger up scrolls content up (= DOWN button); finger down = UP.
  // The horizontal axis is not a scroll: swiping left-to-right (right) = BACK, right-to-left
  // (left) = SELECT.
  switch (direction) {
    case SwipeDirection_Up:
      return BUTTON_ID_DOWN;
    case SwipeDirection_Down:
      return BUTTON_ID_UP;
    case SwipeDirection_Left:
      return BUTTON_ID_SELECT;
    case SwipeDirection_Right:
      return BUTTON_ID_BACK;
    case SwipeDirection_None:
    default:
      return NUM_BUTTONS;
  }
}

// @return true if the button was emitted (or the BACK pop done), false if it was dropped.
static bool prv_emit_click(TouchNavState *state, ButtonId button) {
  if (button >= NUM_BUTTONS) {
    return false;
  }
  // Guard the compositor/window transition first: a gesture that lands mid-animation is dropped.
  if (state->ops->is_animating && state->ops->is_animating(state->ops->ctx)) {
    state->counters.dropped++;
    prv_log_push(state, TouchNavLog_Dropped, (uint8_t)button);
    return false;
  }

  if (button == BUTTON_ID_BACK &&
      !(state->ops->top_overrides_back && state->ops->top_overrides_back(state->ops->ctx))) {
    // BACK on a window with no back handler pops the stack rather than feeding the click
    // recognizer.
    if (state->ops->pop_top) {
      state->ops->pop_top(state->ops->ctx);
    }
  } else if (state->ops->emit_button) {
    state->ops->emit_button(state->ops->ctx, button);
  }
  prv_log_push(state, TouchNavLog_Emit, (uint8_t)button);
  return true;
}

static void prv_recognizer_event(const Recognizer *recognizer, RecognizerEvent event) {
  TouchNavState *state = recognizer_get_user_data(recognizer);
  if (!state) {
    return;
  }
  switch (event) {
    case RecognizerEvent_Started:
      state->counters.started++;
      break;
    case RecognizerEvent_Updated:
      break;
    case RecognizerEvent_Cancelled:
      state->counters.cancelled++;
      break;
    case RecognizerEvent_Completed:
      state->counters.completed++;
      // The bridge only emulates on a Tier-2 route; pan never completes there (it was failed).
      if (state->route == TouchNavRoute_Tier2) {
        // Liftoff re-check: re-run the bridge-vs-app arbitration at the emit point, not only at the
        // Touchdown latch. With the sensor held persistently (system hold) the app can opt this
        // window out of the bridge AFTER the Touchdown already latched Tier-2; emitting anyway
        // would fire alongside the app's own handler -> double action on the first gesture after
        // the window opens. If the window is now bridge-disabled, drop instead of emitting. This
        // closes the persistent-sensor timing window that a Touchdown-only latch leaves open.
        if (state->ops->top_bridge_disabled && state->ops->top_bridge_disabled(state->ops->ctx)) {
          state->counters.dropped++;
          prv_log_push(state, TouchNavLog_Dropped, 0);
          break;
        }
        if (recognizer == state->swipe) {
          // A swipe over the bar stays full-screen: only taps are zoned.
          const SwipeDirection dir = swipe_recognizer_get_direction((Recognizer *)recognizer);
          prv_emit_click(state, prv_swipe_button(dir));
        } else if (recognizer == state->tap) {
          // Route the tap into the action-bar UP/SELECT/DOWN zone if the tap point is inside a
          // present bar; otherwise (no bar, or a tap outside it) this is a plain SELECT -- unless
          // the top window requires taps to land on an action-bar icon zone.
          const bool require_icon_zone = state->ops->top_tap_requires_action_bar &&
                                         state->ops->top_tap_requires_action_bar(state->ops->ctx);
          const GPoint tap_point = tap_recognizer_get_tap_point((Recognizer *)recognizer);
          prv_emit_click(state, touch_nav_action_bar_zone_button(&state->action_bar, tap_point,
                                                                 require_icon_zone));
        }
      }
      break;
  }
}

// Route a navigational Touchdown: hand it to the manager, then resolve and latch the route.
static void prv_handle_touchdown(TouchNavState *state, const TouchEvent *touch_event) {
  RecognizerManager *manager = state->manager;

  // Hand the Touchdown to the manager first so it activates and the set is reset to Possible,
  // then resolve the route once and latch it.
  recognizer_manager_handle_touch_event(touch_event, manager);
  if (manager->state == RecognizerManagerState_WaitForTouchdown) {
    // The manager declined to activate (should not happen for a navigational fresh Touchdown).
    state->route = TouchNavRoute_None;
    state->latched_target = nullptr;
    state->declined = false;
    return;
  }
  state->route = prv_resolve_route(state, touch_event);
  prv_log_push(state, TouchNavLog_Route, (uint8_t)state->route);
  // tier-exclusion stays BRIDGE-ONLY: it governs the bridge (tap, pan, swipe) set by route. The
  // unified widget set is governed solely by its filter plus the fail-at-latch below.
  prv_apply_tier_exclusion(state, state->route);

  // Latch the unified widget target for the whole gesture, mirroring the bridge route latch. Do
  // NOT read latched_target during this Touchdown dispatch: it is set AFTER the manager processed
  // the Touchdown and is only consumed by later events.
  state->latched_target = prv_resolve_widget_target(state);
  state->declined = false;
  if (state->latched_target) {
    // Catch-to-stop: let the widget react to the bare Touchdown (e.g. stop a coasting fling)
    // before any recognizer triggers.
    if (state->latched_target->ops->touchdown) {
      state->latched_target->ops->touchdown(state->latched_target->widget);
    }
  } else {
    // The gesture belongs to the bridge or an un-migrated widget's own set, not the unified set:
    // fail the unified tap/pan/swipe now, symmetrically to the bridge exclusion. On a fresh
    // Touchdown these three are provably still Possible (no recognizer triggers on Touchdown),
    // and prv_fail only fails a Possible recognizer, so the manager returns to idle by
    // construction after the gesture instead of leaving a filtered-out recognizer stuck Possible.
    prv_fail(state, state->widget_tap);
    prv_fail(state, state->widget_pan);
    prv_fail(state, state->widget_swipe);
  }
}

// ---------------------------------------------------------------------------------------------
// Bezel scrolling
//
// A finger that lands in a ring along the edge of the round screen and slides around the center
// acts like a rotating bezel: every step of rotation emits DOWN (clockwise) or UP
// (counter-clockwise). The Touchdown is held back from the recognizers until the finger has
// turned far enough to commit, so a tap or an inward swipe that starts at the edge still reaches
// them unchanged (it is replayed).

#ifdef CONFIG_TOUCH_NAV_BEZEL

bool touch_nav_bezel_in_ring(GPoint point, int16_t width, int16_t height, int16_t ring_px) {
  const int32_t dx = point.x - width / 2;
  const int32_t dy = point.y - height / 2;
  const int32_t inner = MAX(MIN(width, height) / 2 - ring_px, 0);
  return dx * dx + dy * dy >= inner * inner;
}

int32_t touch_nav_bezel_angle(GPoint point, int16_t width, int16_t height) {
  // Screen y grows downward, so the angle atan2 measures from +x grows clockwise on screen.
  return atan2_lookup((int16_t)(point.y - height / 2), (int16_t)(point.x - width / 2));
}

int32_t touch_nav_bezel_angle_delta(int32_t from, int32_t to) {
  int32_t delta = (to - from) % TRIG_MAX_ANGLE;
  if (delta > TRIG_MAX_ANGLE / 2) {
    delta -= TRIG_MAX_ANGLE;
  } else if (delta <= -TRIG_MAX_ANGLE / 2) {
    delta += TRIG_MAX_ANGLE;
  }
  return delta;
}

#define BEZEL_WIDTH  ((int16_t)CONFIG_PLATFORM_DISPLAY_WIDTH)
#define BEZEL_HEIGHT ((int16_t)CONFIG_PLATFORM_DISPLAY_HEIGHT)
#define BEZEL_STEP   DEG_TO_TRIGANGLE(CONFIG_TOUCH_NAV_BEZEL_STEP_DEG)
#define BEZEL_COMMIT DEG_TO_TRIGANGLE(CONFIG_TOUCH_NAV_BEZEL_COMMIT_DEG)
// Inward slack past the ring before a pending touch is handed back as a normal gesture.
#define BEZEL_EXIT_SLACK_PX (12)
// Samples closer than this to the center are ignored: their angle is too noisy.
#define BEZEL_MIN_RADIUS_PX (40)
// A pending touch held longer than this without rotating is not replayed as a tap (matches
// TAP_MAX_DURATION_MS in tap.c).
#define BEZEL_TAP_MAX_MS (300)

// Ring width per TouchBezelWidth: how far in from the edge a rotation may start.
static const int16_t s_bezel_ring_px[TouchBezelWidthCount] = {
  [TouchBezelWidth_Thin] = 18,
  [TouchBezelWidth_Standard] = 26,
  [TouchBezelWidth_Large] = 36,
};

bool touch_nav_is_bezel_step(const TouchNavState *state) {
  return state && state->emitting_bezel_step;
}

int16_t touch_nav_bezel_ring_px(uint8_t width) {
  return (width < TouchBezelWidthCount) ? s_bezel_ring_px[width]
                                        : s_bezel_ring_px[TouchBezelWidth_Standard];
}

// Tick length per TouchBezelHaptic strength: a longer pulse lets the motor build up further.
static const uint16_t s_bezel_haptic_ms[TouchBezelHapticCount] = {
  [TouchBezelHaptic_Off] = 0,
  [TouchBezelHaptic_Light] = 15,
  [TouchBezelHaptic_Medium] = 25,
  [TouchBezelHaptic_Strong] = 40,
};

uint16_t touch_nav_bezel_haptic_ms(uint8_t haptic) {
  return (haptic < TouchBezelHapticCount) ? s_bezel_haptic_ms[haptic]
                                          : s_bezel_haptic_ms[TouchBezelHaptic_Light];
}

// A short pulse per step, like the detents of a real bezel, at the user's vibration strength.
// The pattern service refuses it while another pattern plays, so a tick never cuts into a
// notification, and steps faster than a tick merge into it.
static void prv_bezel_haptic_tick(const TouchNavBezel *bezel) {
  if (bezel->haptic_ms == 0) {
    return;
  }
  if (sys_vibe_pattern_enqueue_step(bezel->haptic_ms, true)) {
    sys_vibe_pattern_trigger_start();
  }
}

// Whether the bezel must stay out of the top window: it opted out of the bridge, or the app
// handles touch itself. An app the user picked for the bezel gets it regardless.
static bool prv_bezel_blocked(TouchNavState *state) {
  return !state->bezel_app && state->ops->top_bridge_disabled &&
         state->ops->top_bridge_disabled(state->ops->ctx);
}

static bool prv_bezel_try_start(TouchNavState *state, const TouchEvent *touchdown) {
  // Bezel scrolling is part of system navigation, behind its own pref.
  if (!sys_touch_nav_enabled() || !sys_touch_bezel_nav_enabled()) {
    return false;
  }
  const GPoint point = GPoint(touchdown->x, touchdown->y);
  const int16_t ring_px = touch_nav_bezel_ring_px(sys_touch_bezel_width());
  if (!touch_nav_bezel_in_ring(point, BEZEL_WIDTH, BEZEL_HEIGHT, ring_px)) {
    return false;
  }
  if (prv_bezel_blocked(state)) {
    return false;
  }
  state->bezel = (TouchNavBezel){
    .phase = TouchNavBezelPhase_Pending,
    .touchdown = *touchdown,
    .touchdown_ticks = sys_get_ticks(),
    .last_angle = touch_nav_bezel_angle(point, BEZEL_WIDTH, BEZEL_HEIGHT),
    .haptic_ms = touch_nav_bezel_haptic_ms(sys_touch_bezel_haptic()),
    .ring_px = ring_px,
  };
  state->route = TouchNavRoute_Bezel;
  state->latched_target = nullptr;
  state->declined = false;
  prv_log_push(state, TouchNavLog_Route, (uint8_t)TouchNavRoute_Bezel);
  return true;
}

// Hand a held-back gesture on: to the recognizers (its Touchdown, then the current event) unless
// the twin only drives the bezel, and to the app's own handler.
static TouchRawDisposition prv_bezel_replay(TouchNavState *state, const TouchEvent *current) {
  state->bezel.phase = TouchNavBezelPhase_Idle;
  if (state->bezel_only) {
    state->route = TouchNavRoute_None;
  } else {
    prv_handle_touchdown(state, &state->bezel.touchdown);
    recognizer_manager_handle_touch_event(current, state->manager);
  }
  return TouchRawDisposition_Release;
}

static void prv_bezel_track(TouchNavBezel *bezel, GPoint point) {
  const int32_t angle = touch_nav_bezel_angle(point, BEZEL_WIDTH, BEZEL_HEIGHT);
  bezel->accum += touch_nav_bezel_angle_delta(bezel->last_angle, angle);
  bezel->last_angle = angle;
}

static void prv_bezel_emit(TouchNavState *state, ButtonId button) {
  // Same re-check as the bridge's Liftoff emit: the window may have opted out mid-gesture.
  if (prv_bezel_blocked(state)) {
    state->counters.dropped++;
    prv_log_push(state, TouchNavLog_Dropped, (uint8_t)button);
    return;
  }
  state->emitting_bezel_step = true;
  const bool emitted = prv_emit_click(state, button);
  state->emitting_bezel_step = false;
  if (emitted) {
    prv_bezel_haptic_tick(&state->bezel);
  }
}

static void prv_bezel_emit_steps(TouchNavState *state) {
  TouchNavBezel *bezel = &state->bezel;
  while (bezel->accum >= BEZEL_STEP) {
    bezel->accum -= BEZEL_STEP;
    prv_bezel_emit(state, BUTTON_ID_DOWN);
  }
  while (bezel->accum <= -BEZEL_STEP) {
    bezel->accum += BEZEL_STEP;
    prv_bezel_emit(state, BUTTON_ID_UP);
  }
}

// @return what the app's own touch handler gets of \a touch_event.
static TouchRawDisposition prv_bezel_handle_event(TouchNavState *state,
                                                  const TouchEvent *touch_event) {
  TouchNavBezel *bezel = &state->bezel;
  const GPoint point = GPoint(touch_event->x, touch_event->y);

  if (bezel->phase == TouchNavBezelPhase_Pending) {
    if (touch_event->type == TouchEvent_Liftoff) {
      const uint64_t elapsed_ms = (sys_get_ticks() - bezel->touchdown_ticks) * 1000 / PBL_TICK_HZ;
      if (elapsed_ms <= BEZEL_TAP_MAX_MS) {
        // A quick touch at the edge: let the recognizers see it (a tap, or a short swipe).
        return prv_bezel_replay(state, touch_event);
      }
      // A long stationary hold did nothing in navigation before bezel scrolling either. The
      // app's own handler still gets it.
      bezel->phase = TouchNavBezelPhase_Idle;
      state->route = TouchNavRoute_Dropped;
      return TouchRawDisposition_Release;
    }
    prv_bezel_track(bezel, point);
    if (!touch_nav_bezel_in_ring(point, BEZEL_WIDTH, BEZEL_HEIGHT,
                                 bezel->ring_px + BEZEL_EXIT_SLACK_PX)) {
      // The finger moved inward: an ordinary swipe or drag that started at the edge.
      return prv_bezel_replay(state, touch_event);
    }
    if (ABS(bezel->accum) >= BEZEL_COMMIT) {
      bezel->phase = TouchNavBezelPhase_Active;
      prv_bezel_emit_steps(state);
      return TouchRawDisposition_Consume;
    }
    return TouchRawDisposition_Hold;
  }

  // Active: follow the finger around the center until Liftoff, wherever it drifts.
  if (touch_event->type == TouchEvent_Liftoff) {
    bezel->phase = TouchNavBezelPhase_Idle;
    return TouchRawDisposition_Consume;
  }
  if (touch_nav_bezel_in_ring(point, BEZEL_WIDTH, BEZEL_HEIGHT,
                              MIN(BEZEL_WIDTH, BEZEL_HEIGHT) / 2 - BEZEL_MIN_RADIUS_PX)) {
    prv_bezel_track(bezel, point);
    prv_bezel_emit_steps(state);
  }
  return TouchRawDisposition_Consume;
}

#endif // CONFIG_TOUCH_NAV_BEZEL

// ---------------------------------------------------------------------------------------------
// Dispatcher

TouchRawDisposition touch_nav_raw_disposition(void *context) {
  const TouchNavState *state = context;
  return state ? state->raw_disposition : TouchRawDisposition_Pass;
}

void touch_nav_dispatch(const TouchEvent *touch_event, void *context) {
  TouchNavState *state = context;
  PBL_ASSERTN(state && state->manager && state->ops);
  state->raw_disposition = TouchRawDisposition_Pass;

  // Nothing navigating: inert even if a stale subscription remains. The recognizer set is never
  // activated, no route is resolved, and the bridge synthesizes nothing. System nav covers the
  // kernel twin and participating apps; app-nav-active covers an opted-in app riding the master
  // pref alone.
  if (!sys_touch_nav_enabled() && !sys_touch_app_nav_active()) {
    return;
  }

  RecognizerManager *manager = state->manager;

  if (touch_event->type == TouchEvent_Touchdown) {
    // A Touchdown always starts a fresh gesture.
    state->bezel.phase = TouchNavBezelPhase_Idle;

    // Live gate re-check: the navigational/gated decision is read from the touch EVENT itself
    // (event-time), not cached from the last focus-change event. The dispatcher runs per touch
    // event, so a delayed/missed PEBBLE_APP_DID_CHANGE_FOCUS_EVENT cannot stale this decision.
    if (touch_event->non_navigational) {
      // Session-gated: skip routing and set_failed, keep the set Possible, and unwind any
      // mid-gesture so a stuck recognizer does not linger. The next navigational Touchdown starts
      // fresh from its first event.
      state->counters.gated++;
      state->route = TouchNavRoute_Dropped;
      // Drop the latch BEFORE unwinding the gesture (the UAF-safe idiom shared with the deregister
      // path). This intentionally does NOT run ops->pan_cancel: the pan recognizer's cancel op
      // returns false (see pan.c prv_cancel) and it never self-transitions to Cancelled, so
      // cancel_and_reset delivers no RecognizerEvent_Cancelled to the unified pan on any
      // pre-emption (gated or navigational) -- exactly as the per-widget sets behaved before this
      // refactor. Synthesising a pan_cancel here would be a new side effect (Ф1 is a
      // no-behaviour-change rebuild), so a mid-pan wake/DnD tap unwinds the gesture silently, as it
      // always has.
      state->latched_target = nullptr;
      state->declined = false;
      prv_log_push(state, TouchNavLog_Gated, 0);
      recognizer_manager_cancel_and_reset(manager);
      return;
    }

    if (state->ops->idle_refresh) {
      // Under the nav gate: only refresh idle when nav actually drives the touch.
      state->ops->idle_refresh(state->ops->ctx);
    }

#ifdef CONFIG_TOUCH_NAV_BEZEL
    if (prv_bezel_try_start(state, touch_event)) {
      state->raw_disposition = TouchRawDisposition_Hold;
      return;
    }
#endif
    if (state->bezel_only) {
      // Installed only for the bezel: the rest of the gesture is the app's alone.
      state->route = TouchNavRoute_None;
      return;
    }
    prv_handle_touchdown(state, touch_event);
  } else {
#ifdef CONFIG_TOUCH_NAV_BEZEL
    if (state->bezel.phase != TouchNavBezelPhase_Idle) {
      state->raw_disposition = prv_bezel_handle_event(state, touch_event);
      return;
    }
#endif
    if (state->bezel_only) {
      return;
    }
    recognizer_manager_handle_touch_event(touch_event, manager);
  }
}

// ---------------------------------------------------------------------------------------------
// Lifecycle

void touch_nav_state_init(TouchNavState *state, RecognizerManager *manager,
                          const TouchNavOps *ops) {
  PBL_ASSERTN(state && manager && ops);
  *state = (TouchNavState){
    .manager = manager,
    .ops = ops,
    .route = TouchNavRoute_None,
  };

  state->tap = tap_recognizer_init_static(state->tap_storage, prv_recognizer_event, state);
  state->pan =
      pan_recognizer_init_static(state->pan_storage, prv_recognizer_event, state, PanAxis_Vertical);
  state->swipe = swipe_recognizer_init_static(state->swipe_storage, prv_recognizer_event, state,
                                              TOUCH_NAV_SWIPE_MASK);

  // The unified widget set: one (tap, pan, swipe) set per twin that drives every migrated Tier-1
  // widget through its vtable. Scoped by prv_widget_touch_filter so it only handles gestures that
  // resolve to a migrated widget and never competes with the bridge set or an un-migrated widget's
  // own set on this shared global list.
  state->widget_tap =
      tap_recognizer_init_static(state->widget_tap_storage, prv_widget_recognizer_event, state);
  state->widget_pan = pan_recognizer_init_static(
      state->widget_pan_storage, prv_widget_recognizer_event, state, PanAxis_Vertical);
  state->widget_swipe = swipe_recognizer_init_static(
      state->widget_swipe_storage, prv_widget_recognizer_event, state, TOUCH_NAV_WIDGET_SWIPE_MASK);
  recognizer_set_touch_filter(state->widget_tap, prv_widget_touch_filter);
  recognizer_set_touch_filter(state->widget_pan, prv_widget_touch_filter);
  recognizer_set_touch_filter(state->widget_swipe, prv_widget_touch_filter);

  RecognizerList *global_list = manager->global_list;
  recognizer_add_to_list(state->tap, global_list);
  recognizer_add_to_list(state->pan, global_list);
  recognizer_add_to_list(state->swipe, global_list);
  recognizer_add_to_list(state->widget_tap, global_list);
  recognizer_add_to_list(state->widget_pan, global_list);
  recognizer_add_to_list(state->widget_swipe, global_list);
}

void touch_nav_transaction_apply(const TouchNavTxnOps *ops, bool enable) {
  PBL_ASSERTN(ops && ops->persist);
  if (enable) {
    // (1) persist the value and flip the master gate.
    ops->persist(ops->ctx, true);
    // (2) subscribe the kernel slot and take the permanent sensor hold (on KernelMain).
    if (ops->kernel_subscribe) {
      ops->kernel_subscribe(ops->ctx);
    }
    if (ops->take_system_hold) {
      ops->take_system_hold(ops->ctx);
    }
  } else {
    // (1) persist the value and flip the master gate so the bridge goes inert immediately.
    ops->persist(ops->ctx, false);
    // (2) synthesize a Liftoff for an in-flight touch (backlight+driver only).
    if (ops->synthesize_liftoff) {
      ops->synthesize_liftoff(ops->ctx);
    }
    // (3) cancel_and_reset the kernel manager and unsubscribe the kernel slot.
    if (ops->kernel_cancel_reset_unsub) {
      ops->kernel_cancel_reset_unsub(ops->ctx);
    }
    // (4) tell the app task to cancel_and_reset its manager and drop its subscription.
    if (ops->app_unsubscribe) {
      ops->app_unsubscribe(ops->ctx);
    }
    // (5) release the permanent sensor hold.
    if (ops->release_system_hold) {
      ops->release_system_hold(ops->ctx);
    }
  }
}

bool touch_nav_app_twin_active(bool system_nav_enabled, bool master_nav_enabled, bool participating,
                               bool opted_in) {
  // Two ways in, nothing else: a participating app (system apps by default) rides the SYSTEM nav
  // state (master pref AND the Touch Navigation sub-pref), while an app that explicitly opted in
  // (app_touch_navigation_enable) follows the master pref alone -- a user who turned the system
  // navigation off has not turned off the app's own gestures.
  return (system_nav_enabled && participating) || (master_nav_enabled && opted_in);
}

bool touch_nav_app_bridge_disabled(bool window_opt_out, bool app_has_raw_subscriber) {
  // Either path disables the Tier-2 bridge: the window explicitly opted out, or the app owns touch
  // through its own raw subscriber and must not be handed synthesized buttons on top of it.
  return window_opt_out || app_has_raw_subscriber;
}

static bool prv_app_twin_should_be_active(const TouchNavTwinOps *ops, bool participating,
                                          bool opted_in) {
  const bool master = ops->master_enabled ? ops->master_enabled(ops->ctx) : false;
  return touch_nav_app_twin_active(ops->pref_enabled(ops->ctx), master, participating, opted_in);
}

void touch_nav_app_twin_subscribe(const TouchNavTwinOps *ops, bool participating, bool opted_in) {
  PBL_ASSERTN(ops && ops->pref_enabled && ops->install_handler);
  if (prv_app_twin_should_be_active(ops, participating, opted_in)) {
    ops->install_handler(ops->ctx);
  }
}

void touch_nav_app_twin_reconcile(const TouchNavTwinOps *ops, bool *participating, bool *opted_in,
                                  bool enable) {
  PBL_ASSERTN(ops && participating && opted_in && ops->remove_handler);
  if ((*participating == enable) && (*opted_in == enable)) {
    // No transition: never double-subscribe, never unsubscribe a twin we did not subscribe.
    return;
  }
  *participating = enable;
  *opted_in = enable;
  if (enable) {
    touch_nav_app_twin_subscribe(ops, true, true);
  } else {
    ops->remove_handler(ops->ctx);
  }
}

void touch_nav_app_twin_resync(const TouchNavTwinOps *ops, bool participating, bool opted_in) {
  PBL_ASSERTN(ops && ops->pref_enabled && ops->install_handler && ops->remove_handler);
  // Bring the installed state in line with the current prefs for the RUNNING app: a pref flip must
  // not blindly unsubscribe an opted-in app (it follows the master pref) nor leave a
  // no-longer-eligible app subscribed. Install and remove are both idempotent.
  if (prv_app_twin_should_be_active(ops, participating, opted_in)) {
    ops->install_handler(ops->ctx);
  } else {
    ops->remove_handler(ops->ctx);
  }
}

void touch_nav_state_deinit(TouchNavState *state) {
  if (!state || !state->manager) {
    return;
  }
  // Cancel any in-flight gesture WITHOUT invoking client callbacks: this may run during an in-place
  // widget rebuild while the window is still alive, so a callback into freed client state is a UAF.
  recognizer_manager_cancel_and_reset(state->manager);

  RecognizerList *global_list = state->manager->global_list;
  recognizer_remove_from_list(state->tap, global_list);
  recognizer_remove_from_list(state->pan, global_list);
  recognizer_remove_from_list(state->swipe, global_list);
  recognizer_remove_from_list(state->widget_tap, global_list);
  recognizer_remove_from_list(state->widget_pan, global_list);
  recognizer_remove_from_list(state->widget_swipe, global_list);
  state->tap = nullptr;
  state->pan = nullptr;
  state->swipe = nullptr;
  state->widget_tap = nullptr;
  state->widget_pan = nullptr;
  state->widget_swipe = nullptr;
  state->latched_target = nullptr;
  state->manager = nullptr;
}
