/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "bpy_geometry_set.hh"

static PyModuleDef _bpy_geometry_set_module_def = {
    /*m_base*/ PyModuleDef_HEAD_INIT,
    /*m_name*/ "_bpy_geometry_set",
    /*m_doc*/ nullptr,
    /*m_size*/ 0,
    /*m_methods*/ nullptr,
    /*m_slots*/ nullptr,
    /*m_traverse*/ nullptr,
    /*m_clear*/ nullptr,
    /*m_free*/ nullptr,
};

PyObject *BPyInit_geometry_set()
{
  PyObject *submodule;

  submodule = PyModule_Create(&_bpy_geometry_set_module_def);

  return submodule;
}
