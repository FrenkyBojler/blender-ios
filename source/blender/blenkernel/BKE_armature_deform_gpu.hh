/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */
#pragma once

#include "DNA_scene_types.h"
#include "DNA_mesh_types.h"

struct Scene;
struct Depsgraph;
struct Mesh;

bool BKE_is_skinning_possible(const Object &ob, const Scene &scene, const Depsgraph &depsgraph);
// bool BKE_is_skinning_available_ob(Object* ob);
bool BKE_is_skinning_available_ob(const Mesh &mesh);

bool BKE_skinning_is_cycles_active(const Scene &scene, const Depsgraph &depsgraph);

bool BKE_skinning_available_user();