/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#pragma once

#include "RNA_types.hh"
#include <map>
#include <stdio.h>

#include "intern/wm_op_handlers_intern.hh"

#ifdef __cplusplus
extern "C" {
#endif

namespace blender {

typedef struct wmOpHandlerData {
  char id_name[OP_MAX_TYPENAME];
  Vector<wmHandlerData> pre_invoke;
  Vector<wmHandlerData> post_invoke;
  Vector<wmHandlerData> modal;
  Vector<wmHandlerData> modal_end;
} wmOpHandlerData;

struct wmOpHandlers {
  /* Map Key matches wmOpHandlerData::id_name. */
  std::map<std::string, wmOpHandlerData> handlers;
};

wmOpHandlers *WM_op_handlers_create(void);
void WM_op_handlers_destroy(struct wmOpHandlers *opHandlers);

wmOpHandlerData *WM_get_op_handlers(wmOpHandlers *op_handlers, const char *op_name);

using wmOpHandlerCb =
    bool (*)(bContext *, const wmEvent *event, void *py_data, PointerRNA *properties, int);

void WM_op_handlers_append(
    wmOpHandlers *op_handlers,
    int id,
    void *handle,
    const char *op_name,
    wmOpHandlerCb cb,
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

}  // namespace blender

#ifdef __cplusplus
}
#endif
