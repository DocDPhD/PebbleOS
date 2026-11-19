/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Settings > Display > Bezel Scrolling > Bezel Width: a full-screen preview of the ring where a
//! bezel rotation starts. On open, the screen's content shrinks inward, with a bounce, to reveal
//! the ring in the theme highlight color. UP/DOWN (buttons or swipes) switch between Thin,
//! Standard and Large, and the ring springs to the new width. Turning the bezel only tests the
//! ring: a dot follows each step around it. Every change is saved at once, so the next turn
//! already uses it.

#include "bezel_width.h"

#ifdef CONFIG_TOUCH_NAV_BEZEL

#include <pbl/i18n/i18n.h>
#include <pbl/services/touch/touch.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>
#include <pbl/util/trig.h>

#include <applib/app_malloc.h>
#include <applib/fonts/fonts.h>
#include <applib/graphics/graphics.h>
#include <applib/graphics/graphics_circle.h>
#include <applib/graphics/text.h>
#include <applib/ui/animation.h>
#include <applib/ui/animation_interpolate.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/recognizer/touch_nav.h>
#include <applib/ui/window.h>
#include <process_state/app_state/app_state.h>
#include <shell/prefs.h>
#include <shell/system_theme.h>

// Where the test dot starts: 12 o'clock (TRIG angles grow clockwise from 3 o'clock on screen).
#define DOT_START_ANGLE (TRIG_MAX_ANGLE * 3 / 4)

static const char *s_width_names[TouchBezelWidthCount] = {
  [TouchBezelWidth_Thin] = pbl_i18n_noop("Thin"),
  [TouchBezelWidth_Standard] = pbl_i18n_noop("Standard"),
  [TouchBezelWidth_Large] = pbl_i18n_noop("Large"),
};

typedef struct BezelWidthData {
  Window window;
  Layer canvas;
  Animation *animation;
  uint8_t width;      //!< The selected TouchBezelWidth.
  int16_t ring_from;  //!< Ring width, in pixels, the running animation starts from.
  int16_t ring_to;    //!< Ring width, in pixels, the running animation ends at.
  int16_t ring_drawn; //!< Ring width, in pixels, drawn now; may overshoot while it bounces.
  int32_t dot_angle;  //!< Where the test dot sits on the ring (TRIG_MAX_ANGLE units, clockwise).
  bool dot_shown;     //!< The bezel has been turned on this screen.
  bool revealing;     //!< The running animation is the opening reveal.
} BezelWidthData;

// Drawing
//////////////////////////////////////////////////////////////////////////////

static void prv_draw_centered(GContext *ctx, const char *text, GFont font, GColor color,
                              int16_t center_x, int16_t top, int16_t width, int16_t height) {
  graphics_context_set_text_color(ctx, color);
  const GRect box = GRect(center_x - width / 2, top, width, height);
  graphics_draw_text(ctx, text, font, box, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     nullptr);
}

static void prv_canvas_update(Layer *layer, GContext *ctx) {
  BezelWidthData *data = window_get_user_data(layer_get_window(layer));
  const GRect bounds = layer->bounds;
  const GPoint center = grect_center_point(&bounds);
  const int16_t radius = MIN(bounds.size.w, bounds.size.h) / 2;
  const GColor bg = system_theme_get_bg_color();
  const GColor fg = system_theme_get_fg_color();

  // The ring where a rotation starts, in the theme color, around the shrunken content.
  graphics_context_set_fill_color(ctx, shell_prefs_get_theme_highlight_color());
  graphics_fill_circle(ctx, center, radius);
  graphics_context_set_fill_color(ctx, bg);
  const int16_t ring = CLIP(data->ring_drawn, 0, radius);
  graphics_fill_circle(ctx, center, radius - ring);

  // The test dot rides in the middle of the ring, one step per bezel step.
  if (data->dot_shown && ring > 4) {
    const int32_t track = radius - ring / 2;
    const GPoint dot = GPoint(center.x + track * cos_lookup(data->dot_angle) / TRIG_MAX_RATIO,
                              center.y + track * sin_lookup(data->dot_angle) / TRIG_MAX_RATIO);
    graphics_fill_circle(ctx, dot, MAX(ring / 2 - 4, 3));
  }

  GFont small = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont small_bold = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont large = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  const int16_t text_width = 2 * (radius - touch_nav_bezel_ring_px(TouchBezelWidth_Large)) - 8;

  /// Heading of the screen that previews the ring where a bezel rotation starts.
  prv_draw_centered(ctx, pbl_i18n_get("Bezel Width", data), small, fg, center.x, center.y - 70,
                    text_width, 24);
  prv_draw_centered(ctx, pbl_i18n_get(s_width_names[data->width], data), large, fg, center.x,
                    center.y - 46, text_width, 34);

  // The three modes side by side, each as wide as its text, the selected one in bold.
  const char *names[TouchBezelWidthCount];
  int16_t widths[TouchBezelWidthCount];
  const int16_t gap = 12;
  int16_t row_width = gap * (TouchBezelWidthCount - 1);
  for (int i = 0; i < TouchBezelWidthCount; i++) {
    names[i] = pbl_i18n_get(s_width_names[i], data);
    const GFont font = (i == data->width) ? small_bold : small;
    widths[i] = app_graphics_text_layout_get_content_size(
                    names[i], font, GRect(0, 0, text_width, 24), GTextOverflowModeTrailingEllipsis,
                    GTextAlignmentLeft)
                    .w +
                2;
    row_width += widths[i];
  }
  int16_t x = center.x - row_width / 2;
  for (int i = 0; i < TouchBezelWidthCount; i++) {
    const bool selected = (i == data->width);
    prv_draw_centered(ctx, names[i], selected ? small_bold : small, fg, x + widths[i] / 2,
                      center.y - 4, widths[i] + 4, 24);
    x += widths[i] + gap;
  }

  /// Hint under the bezel width choices: the buttons change the width.
  prv_draw_centered(ctx, pbl_i18n_get("Up/Down to change", data), small, fg, center.x,
                    center.y + 24, text_width, 24);
  /// Hint under the bezel width choices: turning the bezel here only tests the ring.
  prv_draw_centered(ctx, pbl_i18n_get("Slide the ring to test", data), small, fg, center.x,
                    center.y + 44, text_width, 24);
}

// Animation
//////////////////////////////////////////////////////////////////////////////

// The reveal springs harder than the system's standard moook: it grows through a few soft frames,
// overshoots by twice the usual bounce-back and settles, like the timeline peek's extended moook.
static const int32_t s_reveal_moook_in[] = {0, 1, 4};
static const int32_t s_reveal_moook_out[] = {
  2 * INTERPOLATE_MOOOK_BOUNCE_BACK, INTERPOLATE_MOOOK_BOUNCE_BACK, 2, 1, 0
};
static const MoookConfig s_reveal_moook_config = {
  .frames_in = s_reveal_moook_in,
  .num_frames_in = ARRAY_LENGTH(s_reveal_moook_in),
  .frames_out = s_reveal_moook_out,
  .num_frames_out = ARRAY_LENGTH(s_reveal_moook_out),
  .num_frames_mid = 3,
};

// Hold the reveal until the window has slid in, so the bounce is seen.
#define REVEAL_DELAY_MS (250)

static void prv_animation_update(Animation *animation, const AnimationProgress progress) {
  BezelWidthData *data = animation_get_context(animation);
  // Moook: the Pebble UI's own spring, overshooting the target and settling back.
  data->ring_drawn =
      (int16_t)(data->revealing ? interpolate_moook_custom(progress, data->ring_from, data->ring_to,
                                                           &s_reveal_moook_config)
                                : interpolate_moook(progress, data->ring_from, data->ring_to));
  layer_mark_dirty(&data->canvas);
}

static void prv_animation_stopped(Animation *animation, bool finished, void *context) {
  BezelWidthData *data = context;
  if (data->animation == animation) {
    data->animation = nullptr;
    data->revealing = false;
  }
}

static const AnimationImplementation s_animation_impl = {
  .update = prv_animation_update,
};

static void prv_animate_ring_to(BezelWidthData *data, int16_t ring_to, bool reveal) {
  if (data->animation) {
    animation_unschedule(data->animation);
    data->animation = nullptr;
  }
  data->revealing = reveal;
  data->ring_from = data->ring_drawn;
  data->ring_to = ring_to;
  Animation *animation = animation_create();
  if (!animation) {
    data->revealing = false;
    data->ring_drawn = ring_to;
    layer_mark_dirty(&data->canvas);
    return;
  }
  animation_set_implementation(animation, &s_animation_impl);
  animation_set_duration(animation, reveal
                                        ? interpolate_moook_custom_duration(&s_reveal_moook_config)
                                        : interpolate_moook_duration());
  animation_set_delay(animation, reveal ? REVEAL_DELAY_MS : 0);
  animation_set_curve(animation, AnimationCurveLinear);
  animation_set_handlers(animation, (AnimationHandlers){.stopped = prv_animation_stopped}, data);
  data->animation = animation;
  animation_schedule(animation);
}

// Input
//////////////////////////////////////////////////////////////////////////////

static void prv_select_width(BezelWidthData *data, int delta) {
  const int width = (int)data->width + delta;
  if (width < 0 || width >= TouchBezelWidthCount) {
    return;
  }
  data->width = (uint8_t)width;
  // Saved at once, so the next rotation on this screen already uses it.
  touch_bezel_width_set(data->width);
  prv_animate_ring_to(data, touch_nav_bezel_ring_px(data->width), false /* reveal */);
}

//! Whether the click being handled is a step of the bezel rather than a button press or swipe.
static bool prv_is_bezel_step(void) {
  return touch_nav_is_bezel_step(app_state_get_touch_nav_state());
}

static void prv_test_step(BezelWidthData *data, int direction) {
  // Turning the bezel here tests the ring and leaves the width alone.
  const int32_t step = DEG_TO_TRIGANGLE(CONFIG_TOUCH_NAV_BEZEL_STEP_DEG);
  data->dot_shown = true;
  data->dot_angle = (data->dot_angle + direction * step + TRIG_MAX_ANGLE) % TRIG_MAX_ANGLE;
  layer_mark_dirty(&data->canvas);
}

static void prv_up_click(ClickRecognizerRef recognizer, void *context) {
  if (prv_is_bezel_step()) {
    prv_test_step(context, -1); // counter-clockwise
    return;
  }
  prv_select_width(context, -1);
}

static void prv_down_click(ClickRecognizerRef recognizer, void *context) {
  if (prv_is_bezel_step()) {
    prv_test_step(context, +1); // clockwise
    return;
  }
  prv_select_width(context, +1);
}

static void prv_select_click(ClickRecognizerRef recognizer, void *context) {
  BezelWidthData *data = context;
  app_window_stack_remove(&data->window, true /* animated */);
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
}

// Window
//////////////////////////////////////////////////////////////////////////////

static void prv_window_appear(Window *window) {
  BezelWidthData *data = window_get_user_data(window);
  // Shrink the content inward, with a bounce, to reveal the ring.
  data->ring_drawn = 0;
  prv_animate_ring_to(data, touch_nav_bezel_ring_px(data->width), true /* reveal */);
}

static void prv_window_unload(Window *window) {
  BezelWidthData *data = window_get_user_data(window);
  if (data->animation) {
    animation_unschedule(data->animation);
    data->animation = nullptr;
  }
  layer_deinit(&data->canvas);
  pbl_i18n_free_all(data);
  app_free(data);
}

void settings_bezel_width_push(void) {
  BezelWidthData *data = app_zalloc_check(sizeof(*data));
  const uint8_t width = touch_bezel_width_get();
  data->width = (width < TouchBezelWidthCount) ? width : TouchBezelWidth_Standard;
  data->dot_angle = DOT_START_ANGLE;

  Window *window = &data->window;
  window_init(window, WINDOW_NAME("Bezel Width"));
  window_set_user_data(window, data);
  window_set_background_color(window, system_theme_get_bg_color());
  window_set_click_config_provider_with_context(window, prv_click_config, data);
  window_set_window_handlers(window, &(WindowHandlers){
                                       .appear = prv_window_appear,
                                       .unload = prv_window_unload,
                                     });

  Layer *root = window_get_root_layer(window);
  layer_init(&data->canvas, &root->bounds);
  layer_set_update_proc(&data->canvas, prv_canvas_update);
  layer_add_child(root, &data->canvas);

  app_window_stack_push(window, true /* animated */);
}

#endif // CONFIG_TOUCH_NAV_BEZEL
