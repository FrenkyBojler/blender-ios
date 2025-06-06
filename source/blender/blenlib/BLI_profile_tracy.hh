/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * A tiny wrapper around TracyClient library API which takes care of including the Tracy headers
 * and ensuring the tracing macros are always defined: when building without TracyClient enabled
 * the macros are evaluated to no-op.
 */

#pragma once

#ifdef WITH_TRACY_CLIENT
#  include <tracy/tracy/Tracy.hpp>
#else
#  define ZoneNamed(varname, active)

#  define ZoneScoped

#  define FrameMark

#  define FrameMarkStart(name)
#  define FrameMarkEnd(name)

#endif
