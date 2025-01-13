/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#pragma once

typedef struct wmHandlerData {
  // Pointer to wmOpHandlerData.id_name
  char *id_name;
  void *py_handle;
  void *py_data;
  bool (*cb)(
      struct bContext *, const wmEvent *event, void *, PointerRNA *properties, int);  // callback
  int (*check)(void *, void *, void *);
  bool (*poll)(struct bContext *, const wmEvent *event, void *, PointerRNA *properties);
} wmHandlerData;
