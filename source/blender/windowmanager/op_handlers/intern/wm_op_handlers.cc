/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_windowmanager_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "WM_types.hh"

#include "op_handlers/intern/wm_op_handlers_intern.hh"
#include "op_handlers/wm_op_handlers.hh"

/* -------------------------------------------------------------------------- */
/** \name Public API
 * \{ */

wmOpHandlers *WM_op_handlers_create(void)
{
  std::map<std::string, wmOpHandlerData> op_handlers_data;
  wmOpHandlers *op_handlers = (wmOpHandlers *)MEM_callocN(sizeof(wmOpHandlers), __func__);
  /* std::map needs to be constructed. */
  op_handlers->handlers = std::move(op_handlers_data);
  return op_handlers;
}

void WM_op_handlers_destroy(wmOpHandlers *op_handlers)
{
  for (auto it = op_handlers->handlers.begin(); it != op_handlers->handlers.end(); it++) {
    // wmOpHandlerData *op_handler_data = &(*it->second);
  }
  MEM_delete(op_handlers);
  op_handlers = nullptr;
}

blender::Vector<wmHandlerData> &WM_op_handlers_get_handler_list(wmOpHandlerData *op_handler_data,
                                                                int id)
{
  switch (id) {
    case HANDLER_TYPE_PRE_INVOKE:
      return op_handler_data->pre_invoke;
      break;
    case HANDLER_TYPE_POST_INVOKE:
      return op_handler_data->post_invoke;
      break;
    case HANDLER_TYPE_MODAL:
      return op_handler_data->modal;
      break;
    case HANDLER_TYPE_MODAL_END:
      return op_handler_data->modal_end;
      break;
    default:
      BLI_assert(false);
  }
  /* Should never get here. */
  return op_handler_data->pre_invoke;
}

wmOpHandlerData *WM_get_op_handlers(wmOpHandlers *op_handlers, const char *op_name)
{
  auto it = op_handlers->handlers.find(op_name);
  return (it == op_handlers->handlers.end()) ? nullptr : &(it->second);
}

void WM_op_handlers_append(
    wmOpHandlers *op_handlers,
    int id,
    void *py_handle,
    const char *op_name,
    wmOpHandlerCb cb,
    int (*check)(void *, void *, void *),
    bool (*poll)(bContext *, const wmEvent *event, void *, PointerRNA *properties),
    void *py_data)
{
  wmOpHandlerData *op_handler_data = WM_get_op_handlers(op_handlers, op_name);
  wmHandlerData data;

  if (op_handler_data == nullptr) {
    /* Create. */
    wmOpHandlerData new_data = {0};
    op_handler_data = &(op_handlers->handlers[op_name] = std::move(new_data));
    BLI_strncpy(op_handler_data->id_name, op_name, OP_MAX_TYPENAME);
  }

  data.py_handle = py_handle;
  data.id_name = op_handler_data->id_name;
  data.cb = cb;
  data.check = check;
  data.poll = poll;
  data.py_data = py_data;

  WM_op_handlers_get_handler_list(op_handler_data, id).append(data);
}

int WM_op_handlers_remove_all(wmOpHandlers *op_handlers, void *cb, void *owner)
{
  int ret = 0;
  for (auto &[op_name, op_handler_data] : op_handlers->handlers) {
    ret += WM_op_handlers_remove(
        op_handlers, HANDLER_TYPE_PRE_INVOKE, op_handler_data.id_name, cb, owner);
    ret += WM_op_handlers_remove(
        op_handlers, HANDLER_TYPE_POST_INVOKE, op_handler_data.id_name, cb, owner);
    ret += WM_op_handlers_remove(
        op_handlers, HANDLER_TYPE_MODAL, op_handler_data.id_name, cb, owner);
    ret += WM_op_handlers_remove(
        op_handlers, HANDLER_TYPE_MODAL_END, op_handler_data.id_name, cb, owner);
  }
  return ret;
}

int WM_op_handlers_remove(
    wmOpHandlers *op_handlers, int id, const char *op_name, void *cb, void *owner)
{
  int ret = 0;
  if (id == HANDLER_TYPE_ALL) {
    ret = WM_op_handlers_remove_all(op_handlers, cb, owner);
  }
  else {
    wmOpHandlerData *op_handler_data = WM_get_op_handlers(op_handlers, op_name);
    if (op_handler_data != nullptr) {
      for (auto data : WM_op_handlers_get_handler_list(op_handler_data, id)) {
        if (data.check(data.py_data, owner, cb)) {
          ret++;
        }
      }
    }
  }
  return ret;
}

bool WM_op_handlers_operator_exec(bContext *C,
                                  const wmEvent *event,
                                  blender::Vector<wmHandlerData> &list,
                                  wmOperatorType * /* ot */,
                                  PointerRNA *properties,
                                  int retval)
{
  bool ret = true;
  for (auto &data : list) {
    if (data.poll == nullptr || data.poll(C, event, data.py_data, properties)) {
      ret = ret && data.cb(C, event, data.py_data, properties, retval);
    }
  }
  return ret;
}

/* Previous to invoke. */
bool WM_op_handlers_operator_pre_invoke(bContext *C,
                                        const wmEvent *event,
                                        wmOpHandlers *op_handlers,
                                        wmOperatorType *ot,
                                        PointerRNA *properties)
{
  bool ret = true;
  if (op_handlers != nullptr) {
    wmOpHandlerData *op_handler_data = WM_get_op_handlers(op_handlers, ot->idname);
    if (op_handler_data != nullptr) {
      ret = ret && WM_op_handlers_operator_exec(
                       C,
                       event,
                       WM_op_handlers_get_handler_list(op_handler_data, HANDLER_TYPE_PRE_INVOKE),
                       ot,
                       properties,
                       0);
    }
  }
  return ret;
}

/* Post To invoke. */
void WM_op_handlers_operator_post_invoke(bContext *C,
                                         const wmEvent *event,
                                         wmOpHandlers *op_handlers,
                                         struct wmOperatorType *ot,
                                         PointerRNA *properties,
                                         int retval)
{
  if (op_handlers != nullptr) {
    wmOpHandlerData *op_handler_data = WM_get_op_handlers(op_handlers, ot->idname);
    if (op_handler_data != nullptr) {
      WM_op_handlers_operator_exec(
          C,
          event,
          WM_op_handlers_get_handler_list(op_handler_data, HANDLER_TYPE_POST_INVOKE),
          ot,
          properties,
          retval);
    }
  }
}

bool WM_op_handlers_operator_modal(
    bContext *C, const wmEvent *event, wmOpHandlers *op_handlers, wmOperatorType *ot, int retval)
{
  bool ret = true;
  if (op_handlers != nullptr) {
    wmOpHandlerData *op_handler_data = WM_get_op_handlers(op_handlers, ot->idname);
    if (op_handler_data != nullptr) {
      ret = ret && WM_op_handlers_operator_exec(
                       C,
                       event,
                       WM_op_handlers_get_handler_list(op_handler_data, HANDLER_TYPE_MODAL),
                       ot,
                       nullptr,
                       retval);
    }
  }
  return ret;
}

void WM_op_handlers_operator_modal_end(
    bContext *C, const wmEvent *event, wmOpHandlers *op_handlers, wmOperatorType *ot, int retval)
{
  if (op_handlers != nullptr) {
    wmOpHandlerData *op_handler_data = WM_get_op_handlers(op_handlers, ot->idname);
    if (op_handler_data != nullptr) {
      WM_op_handlers_operator_exec(
          C,
          event,
          WM_op_handlers_get_handler_list(op_handler_data, HANDLER_TYPE_MODAL_END),
          ot,
          nullptr,
          retval);
    }
  }
}

/** \} */
