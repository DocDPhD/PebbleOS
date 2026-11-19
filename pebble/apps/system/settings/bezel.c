/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Settings > Display > Bezel Scrolling: the switch, the strength of the tick felt on every step,
//! and the apps the bezel also works in.

#include "bezel.h"

#include "bezel_width.h"

#ifdef CONFIG_TOUCH_NAV_BEZEL

#include "menu.h"
#include "option_menu.h"
#include "window.h"

#include <stdio.h>

#include <pbl/i18n/i18n.h>
#include <pbl/kernel/fatal.h>
#include <pbl/services/touch/touch.h>
#include <pbl/util/size.h>

#include <applib/app_malloc.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/ui.h>
#include <process_management/app_install_manager.h>
#include <process_management/app_menu_data_source.h>
#include <shell/prefs.h>

static const char *s_width_labels[TouchBezelWidthCount] = {
  [TouchBezelWidth_Thin] = pbl_i18n_noop("Thin"),
  [TouchBezelWidth_Standard] = pbl_i18n_noop("Standard"),
  [TouchBezelWidth_Large] = pbl_i18n_noop("Large"),
};

static const char *s_haptic_labels[TouchBezelHapticCount] = {
  [TouchBezelHaptic_Off] = pbl_i18n_ctx_noop("BezelHaptic", "Off"),
  [TouchBezelHaptic_Light] = pbl_i18n_noop("Light"),
  [TouchBezelHaptic_Medium] = pbl_i18n_noop("Medium"),
  [TouchBezelHaptic_Strong] = pbl_i18n_noop("Strong"),
};

// Apps list
//////////////////////////////////////////////////////////////////////////////

typedef struct SettingsBezelAppsData {
  SettingsCallbacks callbacks;
  AppMenuDataSource data_source;
} SettingsBezelAppsData;

static bool prv_apps_filter(struct AppMenuDataSource *source, AppInstallEntry *entry) {
  // Watchapps only: watchfaces never get touch, and actions are not apps.
  return !app_install_entry_is_watchface(entry) && !app_install_entry_is_hidden(entry) &&
         !app_install_entry_is_quick_launch_visible_only(entry);
}

static void prv_apps_changed(void *context) {
  settings_menu_reload_data(SettingsMenuItemDisplay);
}

static uint16_t prv_apps_num_rows_cb(SettingsCallbacks *context) {
  SettingsBezelAppsData *data = (SettingsBezelAppsData *)context;
  return app_menu_data_source_get_count(&data->data_source);
}

static void prv_apps_draw_row_cb(SettingsCallbacks *context, GContext *ctx, const Layer *cell_layer,
                                 uint16_t row, bool selected) {
  SettingsBezelAppsData *data = (SettingsBezelAppsData *)context;
  const AppMenuNode *node = app_menu_data_source_get_node_at_index(&data->data_source, row);
  if (!node) {
    return;
  }
  const char *subtitle =
      touch_bezel_app_is_enabled(&node->uuid) ? pbl_i18n_noop("On") : pbl_i18n_noop("Off");
  menu_cell_basic_draw(ctx, cell_layer, node->name, pbl_i18n_get(subtitle, data), nullptr);
}

static void prv_apps_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  SettingsBezelAppsData *data = (SettingsBezelAppsData *)context;
  const AppMenuNode *node = app_menu_data_source_get_node_at_index(&data->data_source, row);
  if (!node) {
    return;
  }
  touch_bezel_app_set_enabled(&node->uuid, !touch_bezel_app_is_enabled(&node->uuid));
  settings_menu_reload_data(SettingsMenuItemDisplay);
  settings_menu_mark_dirty(SettingsMenuItemDisplay);
}

static void prv_apps_deinit_cb(SettingsCallbacks *context) {
  SettingsBezelAppsData *data = (SettingsBezelAppsData *)context;
  app_menu_data_source_deinit(&data->data_source);
  pbl_i18n_free_all(data);
  app_free(data);
}

static void prv_apps_push(void) {
  SettingsBezelAppsData *data = app_zalloc_check(sizeof(*data));
  app_menu_data_source_init(&data->data_source,
                            &(AppMenuDataSourceCallbacks){
                              .changed = prv_apps_changed,
                              .filter = prv_apps_filter,
                            },
                            data);
  data->callbacks = (SettingsCallbacks){
    .deinit = prv_apps_deinit_cb,
    .draw_row = prv_apps_draw_row_cb,
    .select_click = prv_apps_select_click_cb,
    .num_rows = prv_apps_num_rows_cb,
  };
  /// Title of the list of apps the bezel scrolling can be turned on in, one by one.
  const char *title = pbl_i18n_noop("Use in Apps");
  Window *window =
      settings_window_create_with_title(SettingsMenuItemDisplay, title, &data->callbacks);
  app_window_stack_push(window, true /* animated */);
}

// Bezel Scrolling submenu
//////////////////////////////////////////////////////////////////////////////

typedef enum SettingsBezelItem {
  SettingsBezelEnabled,
  SettingsBezelWidth,
  SettingsBezelHaptic,
  SettingsBezelApps,
  SettingsBezelItemCount,
} SettingsBezelItem;

typedef struct SettingsBezelData {
  SettingsCallbacks callbacks;
  char apps_subtitle[24];
} SettingsBezelData;

static void prv_haptic_menu_select(OptionMenu *option_menu, int selection, void *context) {
  touch_bezel_haptic_set((uint8_t)selection);
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_haptic_menu_push(SettingsBezelData *data) {
  const OptionMenuCallbacks callbacks = {
    .select = prv_haptic_menu_select,
  };
  const char *title =
      PBL_IF_RECT_ELSE(pbl_i18n_noop("HAPTIC FEEDBACK"), pbl_i18n_noop("Haptic Feedback"));
  settings_option_menu_push(title, OptionMenuContentType_SingleLine, (int)touch_bezel_haptic_get(),
                            &callbacks, ARRAY_LENGTH(s_haptic_labels), true /* icons_enabled */,
                            s_haptic_labels, data);
}

static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  // The haptics and apps only matter while the bezel is on.
  return touch_bezel_nav_is_enabled() ? SettingsBezelItemCount : 1;
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx, const Layer *cell_layer,
                            uint16_t row, bool selected) {
  SettingsBezelData *data = (SettingsBezelData *)context;
  const char *title = nullptr;
  const char *subtitle = nullptr;
  switch ((SettingsBezelItem)row) {
    case SettingsBezelEnabled:
      title = pbl_i18n_noop("Bezel Scrolling");
      subtitle = touch_bezel_nav_is_enabled() ? pbl_i18n_noop("On") : pbl_i18n_noop("Off");
      break;
    case SettingsBezelWidth: {
      title = pbl_i18n_noop("Bezel Width");
      const uint8_t width = touch_bezel_width_get();
      subtitle = s_width_labels[(width < TouchBezelWidthCount) ? width : TouchBezelWidth_Standard];
      break;
    }
    case SettingsBezelHaptic: {
      title = pbl_i18n_noop("Haptic Feedback");
      const uint8_t haptic = touch_bezel_haptic_get();
      subtitle =
          s_haptic_labels[(haptic < TouchBezelHapticCount) ? haptic : TouchBezelHaptic_Light];
      break;
    }
    case SettingsBezelApps: {
      title = pbl_i18n_noop("Use in Apps");
      const uint8_t count = touch_bezel_app_count();
      if (count == 0) {
        /// Shown under "Use in Apps" when the bezel is not turned on in any app.
        subtitle = pbl_i18n_noop("Menus only");
      } else {
        /// Shown under "Use in Apps": how many apps the bezel is turned on in.
        snprintf(data->apps_subtitle, sizeof(data->apps_subtitle),
                 pbl_i18n_get(count == 1 ? "%u app" : "%u apps", data), count);
        menu_cell_basic_draw(ctx, cell_layer, pbl_i18n_get(title, data), data->apps_subtitle,
                             nullptr);
        return;
      }
      break;
    }
    default:
      pbl_panic();
  }
  menu_cell_basic_draw(ctx, cell_layer, pbl_i18n_get(title, data), pbl_i18n_get(subtitle, data),
                       nullptr);
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  SettingsBezelData *data = (SettingsBezelData *)context;
  switch ((SettingsBezelItem)row) {
    case SettingsBezelEnabled:
      touch_set_bezel_nav_pref(!touch_bezel_nav_is_enabled());
      break;
    case SettingsBezelWidth:
      settings_bezel_width_push();
      break;
    case SettingsBezelHaptic:
      prv_haptic_menu_push(data);
      break;
    case SettingsBezelApps:
      prv_apps_push();
      break;
    default:
      pbl_panic();
  }
  settings_menu_reload_data(SettingsMenuItemDisplay);
  settings_menu_mark_dirty(SettingsMenuItemDisplay);
}

static void prv_deinit_cb(SettingsCallbacks *context) {
  SettingsBezelData *data = (SettingsBezelData *)context;
  pbl_i18n_free_all(data);
  app_free(data);
}

void settings_bezel_menu_push(void) {
  SettingsBezelData *data = app_zalloc_check(sizeof(*data));
  data->callbacks = (SettingsCallbacks){
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
  };
  const char *title = pbl_i18n_noop("Bezel Scrolling");
  Window *window =
      settings_window_create_with_title(SettingsMenuItemDisplay, title, &data->callbacks);
  app_window_stack_push(window, true /* animated */);
}

#endif // CONFIG_TOUCH_NAV_BEZEL
