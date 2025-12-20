/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Operations to move virtual shadow map pages between heaps and tiles.
 * We reuse the blender::vector class denomination.
 *
 * The needed resources for this lib are:
 * - tiles_buf
 * - pages_free_buf
 * - pages_cached_buf
 * - pages_infos_buf
 *
 * A page is can be in 3 state (free, cached, acquired). Each one correspond to a different owner.
 *
 * - The pages_free_buf works in a regular stack containing only the page coordinates.
 *
 * - The pages_cached_buf is a ring buffer where newly cached pages gets added at the end and the
 *   old cached pages gets defragmented at the start of the used portion.
 *
 * - The tiles_buf only owns a page if it is used. If the page is cached, the tile contains a
 *   reference index inside the pages_cached_buf.
 *
 * IMPORTANT: Do not forget to manually store the tile data after doing operations on them.
 */

#include "infos/eevee_shadow_pipeline_infos.hh"

#ifdef GPU_LIBRARY_SHADER
SHADER_LIBRARY_CREATE_INFO(eevee_shadow_page_free)
#endif

#include "eevee_shadow_tilemap_lib.glsl"
