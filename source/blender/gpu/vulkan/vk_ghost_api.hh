/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

/** This file contains API that the GHOST_ContextVK can invoke directly. */

namespace blender::gpu {
bool GPU_vulkan_is_supported_driver(VkPhysicalDevice vk_physical_device);
}
