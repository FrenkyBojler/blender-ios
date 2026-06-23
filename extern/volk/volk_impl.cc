/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#define VOLK_NAMESPACE
#define VOLK_IMPLEMENTATION
#ifdef _WIN32
#  define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__APPLE__)
#  define VK_USE_PLATFORM_METAL_EXT
#else
#  define VK_USE_PLATFORM_WAYLAND_KHR
#  define VK_USE_PLATFORM_XLIB_KHR
#endif
#define VOLK_NO_GLOBAL_PROTOTYPES
#include "volk.h"
