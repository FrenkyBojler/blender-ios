/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup win32oskey
 *
 * For preventing Windows Explorer from showing Start Menu when Windows key is pressed.
 *
 * Workaround for low level keyboard hooks being incompatible with raw inputs.
 * Enable the hook from a subprocess, to send a dead key to stop Windows Explorer
 * from showing the Start Menu.
 */

#pragma once

#include "BLI_subprocess.hh"

#if BLI_SUBPROCESS_SUPPORT

namespace blender::win32oskey {
extern const char *const arg_name;
void enable_suppression(bool enable);

void subprocess_run(const char *parent_handle_string,
                    const char *enable_suppression_handle_string);
}  // namespace blender::win32oskey

#endif
