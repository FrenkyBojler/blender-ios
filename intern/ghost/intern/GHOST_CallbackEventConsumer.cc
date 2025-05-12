/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

/**
 * Copyright (C) 2001 NaN Technologies B.V.
 */

#include "GHOST_CallbackEventConsumer.hh"

GHOST_CallbackEventConsumer::GHOST_CallbackEventConsumer(GHOST_EventCallbackProcPtr eventCallback,
                                                         GHOST_TUserDataPtr userData)
{
  m_eventCallback = eventCallback;
  m_userData = userData;
}

bool GHOST_CallbackEventConsumer::processEvent(const GHOST_IEvent *event)
{
  return m_eventCallback(event, m_userData);
}
