/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

typedef struct UsdHookHandle {
  int id_val;
  char name[256];
  char identifier[256];
  bool enabled;
  bool valid;
} UsdHookHandle;
