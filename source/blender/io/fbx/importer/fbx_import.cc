/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#include <cstdio>

#include "IO_fbx.hh"

#include "fbx_import.hh"

#include "CLG_log.h"
static CLG_LogRef LOG = {"io.fbx"};

namespace blender::io::fbx {

void fbx_import_report_error(FILE *file)
{
  CLOG_ERROR(&LOG, "FBX Importer: failed to read file");
  if (feof(file)) {
    CLOG_ERROR(&LOG, "End of file reached");
  }
  else if (ferror(file)) {
    perror("Error");
  }
}

void importer_main(Main *bmain,
                   Scene *scene,
                   ViewLayer *view_layer,
                   const FBXImportParams &import_params)
{
  UNUSED_VARS(bmain, scene, view_layer, import_params);
}

}  // namespace blender::io::fbx
