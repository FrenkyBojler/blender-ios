/* SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstdio>

#define VOLK_CPP_NAMESPACE volk
#include "volk.h"
using namespace VOLK_CPP_NAMESPACE;

#define VMA_IMPLEMENTATION

#define VMA_LEAK_LOG_FORMAT(format, ...) \
  do { \
    fprintf(stderr, "VMA: " format "\n", __VA_ARGS__); \
  } while (false)

/*
 * Disabling internal asserts of VMA.
 */
#define VMA_ASSERT(test)

#include "vk_mem_alloc.h"
