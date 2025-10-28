/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#define HANDLER_TYPE_ALL 0
#define HANDLER_TYPE_PRE_INVOKE 1
#define HANDLER_TYPE_POST_INVOKE 2
#define HANDLER_TYPE_MODAL 3
#define HANDLER_TYPE_MODAL_END 4

#pragma once

struct wmOpHandlers;

#include "op_handlers/wm_op_handlers.hh"
