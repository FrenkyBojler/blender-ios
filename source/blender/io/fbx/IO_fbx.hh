/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#pragma once

#include "BLI_path_utils.hh"

#include "DNA_ID.h"

#include "IO_orientation.hh"

struct Mesh;
struct bContext;
struct ReportList;

struct FBXImportParams {
  char filepath[FILE_MAX] = "";
  eIOAxis forward_axis = IO_AXIS_Y;
  eIOAxis up_axis = IO_AXIS_Z;
  float global_scale = 1.0f;
  bool validate_meshes = true;
  bool use_custom_normals = true;
  bool use_subsurf = false;
  bool use_custom_props = true;

  bool use_anim = true;
  float anim_offset = 1.0f;

  ReportList *reports = nullptr;
};

void FBX_import(bContext *C, const FBXImportParams &params);
