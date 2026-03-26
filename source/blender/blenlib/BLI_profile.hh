/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * A tiny wrapper around the TracyClient library profiling API which takes care of including the
 * Tracy header and wrapping its macros inside of BLI_PROFILE_* macros.
 * When building without TracyClient enabled the macros are evaluated to no-op.
 */

#pragma once

#ifdef WITH_TRACY_CLIENT
#  include <tracy/tracy/Tracy.hpp>
#endif

/* Scoped zones. Create a profiling zone lasting until end of current scope. */
#ifdef WITH_TRACY_CLIENT
#  define BLI_profile_zone_scoped ZoneScoped
#  define BLI_profile_zone_scoped_n(name) ZoneScopedN(name)
#  define BLI_profile_zone_named(varname, active) ZoneNamed(varname, active)
#else
#  define BLI_profile_zone_scoped
#  define BLI_profile_zone_scoped_n(name)
#  define BLI_profile_zone_named(varname, active)
#endif

/* Frame marks. Delimit profiler frames. */
#ifdef WITH_TRACY_CLIENT
#  define BLI_profile_frame_mark FrameMark
#  define BLI_profile_frame_mark_start(name) FrameMarkStart(name)
#  define BLI_profile_frame_mark_end(name) FrameMarkEnd(name)
#else
#  define BLI_profile_frame_mark
#  define BLI_profile_frame_mark_start(name)
#  define BLI_profile_frame_mark_end(name)
#endif
