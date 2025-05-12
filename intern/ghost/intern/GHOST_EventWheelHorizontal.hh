/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_EventWheel class.
 */

#pragma once

#include "GHOST_Event.hh"

/**
 * Horizontal mouse wheel event.
 * Very similar to #GHOST_EventWheel.
 */
class GHOST_EventWheelHorizontal : public GHOST_Event {
 public:
  GHOST_EventWheelHorizontal(uint64_t msec, GHOST_IWindow *window, int32_t z)
      : GHOST_Event(msec, GHOST_kEventWheelHorizontal, window)
  {
    m_wheelEventData.z = z;
    m_data = &m_wheelEventData;
  }

 protected:
  GHOST_TEventWheelHorizontalData m_wheelEventData;
};
