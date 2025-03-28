/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "BKE_customdata.hh"

#include "BLI_function_ref.hh"

#include <tuple>
#include <unordered_map>

struct BMLoop;
struct BMesh;
struct Scene;

bool UV_get_edgeloops(
    const Scene *scene,
    BMesh *bm,
    blender::Vector<blender::Vector<blender::Vector<BMLoop *>>> *edgeloops,
    blender::FunctionRef<bool(const Scene *scene, BMLoop *l, const BMUVOffsets offsets)> callback);
