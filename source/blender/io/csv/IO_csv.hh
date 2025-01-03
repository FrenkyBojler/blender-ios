/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup csv
 */

#pragma once

#include "BLI_path_utils.hh"

struct PointCloud;
struct ReportList;

struct CSVImportParams {
  /** Full path to the source CSV file to import. */
  char filepath[FILE_MAX];

  ReportList *reports = nullptr;
};

PointCloud *CSV_import_point_cloud(const CSVImportParams *import_params);
