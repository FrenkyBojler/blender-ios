/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

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

#include "op_handlers/intern/wm_op_handlers_intern.h"
#include "op_handlers/wm_op_handlers.h"

/* -------------------------------------------------------------------------- */
/** \name Public API
 * \{ */

// Avoid Wmissing-declarations
bool WM_op_handlers_operator_exec(struct bContext *C,
                                  const wmEvent *event,
                                  ListBase *list,
                                  struct wmOperatorType *ot,
                                  struct PointerRNA *properties,
                                  int retval);

// Avoid Wmissing-declarations
int WM_op_handlers_remove_all(struct wmOpHandlers *op_handlers, void *cb, void *owner);

// Avoid Wmissing-declarations
ListBase *WM_op_handlers_get_handler_list(wmOpHandlerData *opHandlers, int id);

struct wmOpHandlers *WM_op_handlers_create(void)
{

  struct wmOpHandlers *op_handlers = (wmOpHandlers *)MEM_callocN(sizeof(*op_handlers), __func__);
  return op_handlers;
}

void WM_op_handlers_destroy(struct wmOpHandlers *op_handlers)
{
  LISTBASE_FOREACH (wmOpHandlerData *, opHandlers, &op_handlers->handlers) {
    BLI_freelistN(&opHandlers->pre_invoke);
    BLI_freelistN(&opHandlers->post_invoke);
    BLI_freelistN(&opHandlers->modal);
    BLI_freelistN(&opHandlers->modal_end);
  }
  BLI_freelistN(&op_handlers->handlers);
  MEM_freeN(op_handlers);
  op_handlers = nullptr;
}

ListBase *WM_op_handlers_get_handler_list(wmOpHandlerData *opHandlers, int id)
{
  ListBase *list = nullptr;
  switch (id) {
    case HANDLER_TYPE_PRE_INVOKE:
      list = &opHandlers->pre_invoke;
      break;
    case HANDLER_TYPE_POST_INVOKE:
      list = &opHandlers->post_invoke;
      break;
    case HANDLER_TYPE_MODAL:
      list = &opHandlers->modal;
      break;
    case HANDLER_TYPE_MODAL_END:
      list = &opHandlers->modal_end;
      break;
    default:
      BLI_assert(false);
  }
  return list;
}

wmOpHandlerData *WM_get_op_handlers(struct wmOpHandlers *op_handlers, const char *op_name)
{
  return (wmOpHandlerData *)BLI_findstring(
      &op_handlers->handlers, op_name, offsetof(wmOpHandlerData, id_name));
}

void WM_op_handlers_append(
    struct wmOpHandlers *op_handlers,
    int id,
    void *py_handle,
    const char *op_name,
    bool (*cb)(
        struct bContext *, const struct wmEvent *event, void *, PointerRNA *properties, int),
    int (*check)(void *, void *, void *),
    bool (*poll)(struct bContext *, const struct wmEvent *event, void *, PointerRNA *properties),
    void *py_data)
{
  wmOpHandlerData *opHandlers = WM_get_op_handlers(op_handlers, op_name);
  wmHandlerData *data = (wmHandlerData *)MEM_mallocN(sizeof(wmHandlerData), "wmHandlerData");

  if (opHandlers == nullptr) {
    // Create
    opHandlers = (wmOpHandlerData *)MEM_callocN(sizeof(wmOpHandlerData), "wmOpHandlerData");
    BLI_strncpy(opHandlers->id_name, op_name, OP_MAX_TYPENAME);
    BLI_addtail(&op_handlers->handlers, opHandlers);
  }

  data->py_handle = py_handle;
  data->id_name = opHandlers->id_name;
  data->cb = cb;
  data->check = check;
  data->poll = poll;
  data->py_data = py_data;

  ListBase *list = WM_op_handlers_get_handler_list(opHandlers, id);
  BLI_addtail(list, data);
}

int WM_op_handlers_remove_all(struct wmOpHandlers *op_handlers, void *cb, void *owner)
{
  int ret = 0;
  LISTBASE_FOREACH (wmOpHandlerData *, opHandlers, &op_handlers->handlers) {
    ret += WM_op_handlers_remove(
        op_handlers, HANDLER_TYPE_PRE_INVOKE, opHandlers->id_name, cb, owner);
    ret += WM_op_handlers_remove(
        op_handlers, HANDLER_TYPE_POST_INVOKE, opHandlers->id_name, cb, owner);
    ret += WM_op_handlers_remove(op_handlers, HANDLER_TYPE_MODAL, opHandlers->id_name, cb, owner);
    ret += WM_op_handlers_remove(
        op_handlers, HANDLER_TYPE_MODAL_END, opHandlers->id_name, cb, owner);
  }
  return ret;
}

int WM_op_handlers_remove(
    struct wmOpHandlers *op_handlers, int id, const char *op_name, void *cb, void *owner)
{
  int ret = 0;
  if (id == HANDLER_TYPE_ALL) {
    ret = WM_op_handlers_remove_all(op_handlers, cb, owner);
  }
  else {
    wmOpHandlerData *opHandlers = WM_get_op_handlers(op_handlers, op_name);

    if (opHandlers != nullptr) {
      ListBase *list = WM_op_handlers_get_handler_list(opHandlers, id);
      wmHandlerData *next;
      for (wmHandlerData *data = (wmHandlerData *)list->first; data; data = next) {
        next = data->next;
        if (data->check(data->py_data, owner, cb)) {
          BLI_freelinkN(list, data);
          ret++;
        }
      }
    }
  }
  return ret;
}

bool WM_op_handlers_operator_exec(struct bContext *C,
                                  const wmEvent *event,
                                  ListBase *list,
                                  struct wmOperatorType * /* ot */,
                                  struct PointerRNA *properties,
                                  int retval)
{
  bool ret = true;
  LISTBASE_FOREACH (wmHandlerData *, data, list) {
    if (data->poll == nullptr || data->poll(C, event, data->py_data, properties)) {
      ret = ret && data->cb(C, event, data->py_data, properties, retval);
    }
  }
  return ret;
}

// Previous to invoke
bool WM_op_handlers_operator_pre_invoke(struct bContext *C,
                                        const wmEvent *event,
                                        struct wmOpHandlers *op_handlers,
                                        struct wmOperatorType *ot,
                                        PointerRNA *properties)
{
  bool ret = true;
  if (op_handlers != nullptr) {
    wmOpHandlerData *opHandlers = WM_get_op_handlers(op_handlers, ot->idname);
    if (opHandlers != nullptr) {
      ListBase *list = WM_op_handlers_get_handler_list(opHandlers, HANDLER_TYPE_PRE_INVOKE);
      ret = ret && WM_op_handlers_operator_exec(C, event, list, ot, properties, 0);
    }
  }
  return ret;
}

// Post To invoke
void WM_op_handlers_operator_post_invoke(struct bContext *C,
                                         const wmEvent *event,
                                         struct wmOpHandlers *op_handlers,
                                         struct wmOperatorType *ot,
                                         struct PointerRNA *properties,
                                         int retval)
{
  if (op_handlers != nullptr) {
    wmOpHandlerData *opHandlers = WM_get_op_handlers(op_handlers, ot->idname);
    if (opHandlers != nullptr) {
      ListBase *list = WM_op_handlers_get_handler_list(opHandlers, HANDLER_TYPE_POST_INVOKE);
      WM_op_handlers_operator_exec(C, event, list, ot, properties, retval);
    }
  }
}

bool WM_op_handlers_operator_modal(struct bContext *C,
                                   const wmEvent *event,
                                   struct wmOpHandlers *op_handlers,
                                   struct wmOperatorType *ot,
                                   int retval)
{
  bool ret = true;
  if (op_handlers != nullptr) {
    wmOpHandlerData *opHandlers = WM_get_op_handlers(op_handlers, ot->idname);
    if (opHandlers != nullptr) {
      ListBase *list = WM_op_handlers_get_handler_list(opHandlers, HANDLER_TYPE_MODAL);
      ret = ret && WM_op_handlers_operator_exec(C, event, list, ot, nullptr, retval);
    }
  }
  return ret;
}

void WM_op_handlers_operator_modal_end(struct bContext *C,
                                       const wmEvent *event,
                                       struct wmOpHandlers *op_handlers,
                                       struct wmOperatorType *ot,
                                       int retval)
{
  if (op_handlers != nullptr) {
    wmOpHandlerData *opHandlers = WM_get_op_handlers(op_handlers, ot->idname);
    if (opHandlers != nullptr) {
      ListBase *list = WM_op_handlers_get_handler_list(opHandlers, HANDLER_TYPE_MODAL_END);
      WM_op_handlers_operator_exec(C, event, list, ot, nullptr, retval);
    }
  }
}

/** \} */
