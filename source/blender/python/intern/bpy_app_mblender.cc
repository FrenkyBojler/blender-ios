/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 */

#include <Python.h>

#include "BLI_utildefines.h"

#include "bpy_app_mblender.h"

#include "MB_blender.h"

static PyTypeObject BlenderAppMblenderType;

static PyStructSequence_Field app_mblender_info_fields[] = {
    {"patches", nullptr},
    {nullptr},
};

static PyStructSequence_Desc app_mblender_info_desc = {
    "bpy.app.mblender",                                                /* name */
    "This module contains information about options blender is built with", /* doc */
    app_mblender_info_fields,                                              /* fields */
    ARRAY_SIZE(app_mblender_info_fields) - 1,
};

static PyObject *make_mblender_info()
{
  PyObject *mblender_info;
  PyObject *list;
  int pos=0;
  char *patch = nullptr;
  
  mblender_info = PyStructSequence_New(&BlenderAppMblenderType);
  if (mblender_info == nullptr) {
    return nullptr;
  }
  
  list = PyList_New(0);
  
  PyStructSequence_SET_ITEM(mblender_info, pos++, list);
  
  
  for (int i =0 ; (patch = MB_patch_get(i)); i++) {
    PyList_Append(list, PyUnicode_FromString(patch));
  }
  

  return mblender_info;
}

PyObject *BPY_app_mblender_struct()
{
  PyObject *ret;

  PyStructSequence_InitType(&BlenderAppMblenderType, &app_mblender_info_desc);

  ret = make_mblender_info();

  /* prevent user from creating new instances */
  BlenderAppMblenderType.tp_init = nullptr;
  BlenderAppMblenderType.tp_new = nullptr;
  /* Without this we can't do `set(sys.modules)` #29635. */
  BlenderAppMblenderType.tp_hash = (hashfunc)_Py_HashPointer;

  return ret;
}
