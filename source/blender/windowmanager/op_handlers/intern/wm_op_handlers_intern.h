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

#include "../wm_op_handlers.h"

typedef struct wmHandlerData {
  struct wmHandlerData *next, *prev;
  char *id_name;  // Pointer to wmOpHandlerData.id_name
  void *py_handle;
  void *py_data;
  bool (*cb)(
      struct bContext *, const wmEvent *event, void *, PointerRNA *properties, int);  // callback
  int (*check)(void *, void *, void *);
  bool (*poll)(struct bContext *, const wmEvent *event, void *, PointerRNA *properties);
} wmHandlerData;
