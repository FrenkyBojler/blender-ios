/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Accumulated buffer accesses for the current rendering scope.
 *
 * During draw call recording, each draw node accumulates its vertex/index/indirect
 * buffer handles into this struct instead of adding per-draw buffer links. At
 * `rendering_end()`, the accumulated accesses are transferred to the BEGIN_RENDERING
 * node's buffer links, consolidating all read-only buffer dependencies into a
 * single node.
 */

#pragma once

#include "BLI_map.hh"

#include "vk_common.hh"

namespace blender::gpu {

struct VKRenderScopeAccess {
  /**
   * Accumulated vertex/index/indirect buffer handles and their access flags.
   *
   * Populated by draw node `build_links` via `*_accumulate_to_set()` functions,
   * and consumed at `rendering_end()` to build buffer links on the
   * BEGIN_RENDERING node.
   */
  Map<ResourceHandle, VkAccessFlags> buffers;
};

}  // namespace blender::gpu
