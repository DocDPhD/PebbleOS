/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

/** @brief Touch event type */
typedef enum TouchEventType {
  /** A finger touched the screen. */
  TouchEvent_Touchdown,
  /** The finger left the screen. */
  TouchEvent_Liftoff,
  /** The finger moved while touching the screen. */
  TouchEvent_PositionUpdate,
} TouchEventType;

/** @brief Touch event data, carried directly in PebbleTouchEvent */
typedef struct TouchEvent {
  /** Event type. */
  TouchEventType type : 8;
  /**
   * true when the touch must not drive navigation: the interaction session
   * was inactive at Touchdown (unarmed contact on the idle watchface).
   * Latched on Touchdown and carried across the whole gesture.
   */
  bool non_navigational;
  /** Horizontal position, in screen pixels. */
  int16_t x;
  /** Vertical position, in screen pixels. */
  int16_t y;
} TouchEvent;

/** @brief What the app's own touch handler gets of an event the system handler saw first. */
typedef enum TouchRawDisposition {
  /** Deliver the event. */
  TouchRawDisposition_Pass,
  /** Hold it back: keep a Touchdown, drop anything else, until a Release or Consume. */
  TouchRawDisposition_Hold,
  /** Deliver the held Touchdown, if any, then the event. */
  TouchRawDisposition_Release,
  /** Drop the event and the held Touchdown: the system used the gesture. */
  TouchRawDisposition_Consume,
} TouchRawDisposition;

static_assert(sizeof(TouchEvent) <= 9, "TouchEvent must stay small; it rides inside PebbleEvent");
