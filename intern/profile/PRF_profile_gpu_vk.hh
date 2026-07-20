/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "PRF_profile.hh"
#include "PRF_profile_gpu_common.hh"

#if defined(WITH_TRACY) && defined(WITH_TRACY_GPU) && defined(WITH_OPENGL_BACKEND)
#  define TRACY_VK_USE_SYMBOL_TABLE
#  include "tracy/TracyVulkan.hpp"
using VKProfileScope = tracy::VkCtxScope;
#else
struct VKProfileScope {};
#endif
