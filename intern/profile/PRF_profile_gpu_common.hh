/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "PRF_profile.hh"

#if defined(WITH_TRACY)
#  define _PRF_DEBUG_CONCAT_(prefix, suffix) prefix##suffix
#  define _PRF_DEBUG_CONCAT(prefix, suffix) _PRF_DEBUG_CONCAT_(prefix, suffix)
#  define PrfSourceLocationUnique(name, category) \
    PrfSourceLocation _PRF_DEBUG_CONCAT(gpu_profile_loc_, __LINE__)( \
        name, TracyFunction, TracyFile, (uint32_t)TracyLine, uint32_t(category))
using PrfSourceLocation = tracy::SourceLocationData;
#else
#  define PrfSourceLocationUnique(name, category)
struct PrfSourceLocation {};
#endif
