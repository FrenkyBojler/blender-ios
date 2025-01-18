/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#pragma once

struct wmOpHandlerData;

typedef struct wmHandlerData {
  /* Pointer to wmOpHandlerData.id_name. */
  char *id_name;
  void *py_handle;
  void *py_data;
  /* Callback function */
  bool (*cb)(struct bContext *, const wmEvent *event, void *, PointerRNA *properties, int);
  int (*check)(void *, void *, void *);
  bool (*poll)(struct bContext *, const wmEvent *event, void *, PointerRNA *properties);
} wmHandlerData;

bool WM_op_handlers_operator_exec(bContext *C,
                                  const wmEvent *event,
                                  blender::Vector<wmHandlerData> &list,
                                  wmOperatorType *ot,
                                  PointerRNA *properties,
                                  int retval);

int WM_op_handlers_remove_all(wmOpHandlers *op_handlers, void *cb, void *owner);

blender::Vector<wmHandlerData> &WM_op_handlers_get_handler_list(wmOpHandlerData *op_handler_data,
                                                                int id);
