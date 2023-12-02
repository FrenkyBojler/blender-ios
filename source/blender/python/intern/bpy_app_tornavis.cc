/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 */

#include <Python.h>

#include "BLI_utildefines.h"

#include "bpy_app_tornavis.h"

#include "MB_tornavis.h"

static PyTypeObject BlenderAppTornavisType;

static PyStructSequence_Field app_tornavis_info_fields[] = {
    {"patches", nullptr},
    {nullptr},
};

static PyStructSequence_Desc app_tornavis_info_desc = {
    "bpy.app.tornavis",                                                /* name */
    "This module contains options about tornavis project", /* doc */
    app_tornavis_info_fields,                                              /* fields */
    ARRAY_SIZE(app_tornavis_info_fields) - 1,
};

static PyObject *make_tornavis_info()
{
  PyObject *tornavis_info;
  PyObject *list;
  int pos=0;
  char *patch = nullptr;
  
  tornavis_info = PyStructSequence_New(&BlenderAppTornavisType);
  if (tornavis_info == nullptr) {
    return nullptr;
  }
  
  list = PyList_New(0);
  
  PyStructSequence_SET_ITEM(tornavis_info, pos++, list);
  
  
  for (int i =0 ; (patch = MB_patch_get(i)); i++) {
    PyList_Append(list, PyUnicode_FromString(patch));
  }
  

  return tornavis_info;
}

PyObject *BPY_app_tornavis_struct()
{
  PyObject *ret;

  PyStructSequence_InitType(&BlenderAppTornavisType, &app_tornavis_info_desc);

  ret = make_tornavis_info();

  /* prevent user from creating new instances */
  BlenderAppTornavisType.tp_init = nullptr;
  BlenderAppTornavisType.tp_new = nullptr;
  /* Without this we can't do `set(sys.modules)` #29635. */
  BlenderAppTornavisType.tp_hash = (hashfunc)_Py_HashPointer;

  return ret;
}
