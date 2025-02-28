/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "bpy_geometry_set.hh"

#include "BKE_geometry_set.hh"

using blender::bke::GeometrySet;

struct BPy_GeometrySet {
  PyObject_HEAD
  GeometrySet geometry;
};

static PyObject *BPy_GeometrySet_new(PyTypeObject *type, PyObject * /*args*/, PyObject * /*kwds*/)
{
  BPy_GeometrySet *self = reinterpret_cast<BPy_GeometrySet *>(type->tp_alloc(type, 0));
  if (self == nullptr) {
    return nullptr;
  }
  new (&self->geometry) GeometrySet();
  return reinterpret_cast<PyObject *>(self);
}

static void BPy_GeometrySet_dealloc(BPy_GeometrySet *self)
{
  std::destroy_at(&self->geometry);
  Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

static PyMethodDef BPy_GeometrySet_methods[] = {
    {nullptr, nullptr, 0, nullptr},
};

PyTypeObject bpy_geometry_set_Type = {
    /*ob_base*/ PyVarObject_HEAD_INIT(nullptr, 0)
    /*tp_name*/ "GeometrySet",
    /*tp_basicsize*/ sizeof(BPy_GeometrySet),
    /*tp_itemsize*/ 0,
    /*tp_dealloc*/ reinterpret_cast<destructor>(BPy_GeometrySet_dealloc),
    /*tp_vectorcall_offset*/ 0,
    /*tp_getattr*/ nullptr,
    /*tp_setattr*/ nullptr,
    /*tp_as_async*/ nullptr,
    /*tp_repr*/ nullptr,
    /*tp_as_number*/ nullptr,
    /*tp_as_sequence*/ nullptr,
    /*tp_as_mapping*/ nullptr,
    /*tp_hash*/ nullptr,
    /*tp_call*/ nullptr,
    /*tp_str*/ nullptr,
    /*tp_getattro*/ nullptr,
    /*tp_setattro*/ nullptr,
    /*tp_as_buffer*/ nullptr,
    /*tp_flags*/ Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    /*tp_doc*/ nullptr,
    /*tp_traverse*/ nullptr,
    /*tp_clear*/ nullptr,
    /*tp_richcompare*/ nullptr,
    /*tp_weaklistoffset*/ 0,
    /*tp_iter*/ nullptr,
    /*tp_iternext*/ nullptr,
    /*tp_methods*/ BPy_GeometrySet_methods,
    /*tp_members*/ nullptr,
    /*tp_getset*/ nullptr,
    /*tp_base*/ nullptr,
    /*tp_dict*/ nullptr,
    /*tp_descr_get*/ nullptr,
    /*tp_descr_set*/ nullptr,
    /*tp_dictoffset*/ 0,
    /*tp_init*/ nullptr,
    /*tp_alloc*/ nullptr,
    /*tp_new*/ BPy_GeometrySet_new,
};

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
  PyObject *m = PyModule_Create(&_bpy_geometry_set_module_def);
  if (PyType_Ready(&bpy_geometry_set_Type) < 0) {
    return nullptr;
  }
  PyModule_AddObject(m, "GeometrySet", reinterpret_cast<PyObject *>(&bpy_geometry_set_Type));
  return m;
}
