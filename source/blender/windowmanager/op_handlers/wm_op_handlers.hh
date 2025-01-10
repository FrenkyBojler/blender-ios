/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#pragma once

#include "RNA_types.hh"
#include <stdio.h>

#define HANDLER_TYPE_ALL 0
#define HANDLER_TYPE_PRE_INVOKE 1
#define HANDLER_TYPE_POST_INVOKE 2
#define HANDLER_TYPE_MODAL 3
#define HANDLER_TYPE_MODAL_END 4

#ifdef __cplusplus
extern "C" {
#endif

struct wmOpHandlers {
  /** Handlers in order of being added. */
  ListBase handlers;
};

typedef struct wmOpHandlerData {
  struct wmOpHandlerData *next, *prev;
  char id_name[OP_MAX_TYPENAME];
  /** Handlers in order of being added. */
  ListBase pre_invoke;
  ListBase post_invoke;
  ListBase modal;
  ListBase modal_end;
} wmOpHandlerData;

wmOpHandlers *WM_op_handlers_create(void);
void WM_op_handlers_destroy(struct wmOpHandlers *opHandlers);

wmOpHandlerData *WM_get_op_handlers(wmOpHandlers *op_handlers, const char *op_name);

typedef bool(wm_op_handler_cb)(
    bContext *, const wmEvent *event, void *py_data, PointerRNA *properties, int);

void WM_op_handlers_append(
    wmOpHandlers *op_handlers,
    int id,
    void *handle,
    const char *op_name,
    wm_op_handler_cb *cb,
    int (*check)(void *, void *, void *),
    bool (*poll)(bContext *, const wmEvent *event, void *, PointerRNA *properties),
    void *py_data);

int WM_op_handlers_remove(
    wmOpHandlers *op_handlers, int id, const char *op_name, void *cb, void *owner);

bool WM_op_handlers_operator_pre_invoke(bContext *C,
                                        const wmEvent *event,
                                        wmOpHandlers *op_handlers,
                                        wmOperatorType *ot,
                                        PointerRNA *properties);
void WM_op_handlers_operator_post_invoke(bContext *C,
                                         const wmEvent *event,
                                         wmOpHandlers *op_handlers,
                                         wmOperatorType *ot,
                                         PointerRNA *properties,
                                         int retval);

bool WM_op_handlers_operator_modal(
    bContext *C, const wmEvent *event, wmOpHandlers *op_handlers, wmOperatorType *ot, int retval);

void WM_op_handlers_operator_modal_end(
    bContext *C, const wmEvent *event, wmOpHandlers *op_handlers, wmOperatorType *ot, int retval);

#ifdef __cplusplus
}
#endif
