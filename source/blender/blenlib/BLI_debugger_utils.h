/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_sys_types.h"

/** \file
 * \ingroup bli
 * \brief Debugger utilities
 */

/**
 * Check if blender is running under a debugger
 */
bool BLI_debugger_present();

/**
 * Generate a breakpoint if a debugger was found, otherwise do nothing.
 * Returns 0 if a breakpoint was created, -1 otherwise.
 */
int BLI_debugger_breakpoint(const char *name);
