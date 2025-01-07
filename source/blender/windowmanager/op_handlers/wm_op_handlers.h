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

struct wmOpHandlers *WM_op_handlers_create(void);
void WM_op_handlers_destroy(struct wmOpHandlers *opHandlers);

wmOpHandlerData *WM_get_op_handlers(struct wmOpHandlers *op_handlers, const char *op_name);

void WM_op_handlers_append(
    struct wmOpHandlers *op_handlers,
    int id,
    void *handle,
    const char *op_name,
    bool (*cb)(
        struct bContext *, const struct wmEvent *event, void *, PointerRNA *properties, int),
    int (*check)(void *, void *, void *),
    bool (*poll)(struct bContext *, const struct wmEvent *event, void *, PointerRNA *properties),
    void *py_data);

int WM_op_handlers_remove(
    struct wmOpHandlers *op_handlers, int id, const char *op_name, void *cb, void *owner);

bool WM_op_handlers_operator_pre_invoke(struct bContext *C,
                                        const struct wmEvent *event,
                                        struct wmOpHandlers *op_handlers,
                                        struct wmOperatorType *ot,
                                        PointerRNA *properties);
void WM_op_handlers_operator_post_invoke(struct bContext *C,
                                         const struct wmEvent *event,
                                         struct wmOpHandlers *op_handlers,
                                         struct wmOperatorType *ot,
                                         PointerRNA *properties,
                                         int retval);

bool WM_op_handlers_operator_modal(struct bContext *C,
                                   const struct wmEvent *event,
                                   struct wmOpHandlers *op_handlers,
                                   struct wmOperatorType *ot,
                                   int retval);

void WM_op_handlers_operator_modal_end(struct bContext *C,
                                       const struct wmEvent *event,
                                       struct wmOpHandlers *op_handlers,
                                       struct wmOperatorType *ot,
                                       int retval);

#ifdef __cplusplus
}
#endif
