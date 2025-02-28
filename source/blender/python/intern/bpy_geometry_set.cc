/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <sstream>

#include "BKE_geometry_set.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_idtype.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "bpy_geometry_set.hh"
#include "bpy_rna.hh"

using blender::bke::GeometrySet;

extern PyTypeObject bpy_geometry_set_Type;

struct BPy_GeometrySet {
  PyObject_HEAD
  GeometrySet geometry;
};

static BPy_GeometrySet *new_empty_BPy_GeometrySet()
{
  BPy_GeometrySet *self = reinterpret_cast<BPy_GeometrySet *>(
      bpy_geometry_set_Type.tp_alloc(&bpy_geometry_set_Type, 0));
  if (self == nullptr) {
    return nullptr;
  }
  new (&self->geometry) GeometrySet();
  return self;
}

static BPy_GeometrySet *BPy_GeometrySet_new(PyTypeObject * /*type*/,
                                            PyObject * /*args*/,
                                            PyObject * /*kwds*/)
{
  return new_empty_BPy_GeometrySet();
}

static void BPy_GeometrySet_dealloc(BPy_GeometrySet *self)
{
  std::destroy_at(&self->geometry);
  Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

static BPy_GeometrySet *BPy_GeometrySet_static_from_evaluated_object(PyObject * /*self*/,
                                                                     PyObject *args,
                                                                     PyObject *kwds)
{
  static const char *kwlist[] = {"evaluated_object", nullptr};
  PyObject *py_evaluated_object;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "O", const_cast<char **>(kwlist), &py_evaluated_object))
  {
    return nullptr;
  }
  ID *evaluated_object_id = nullptr;
  if (!pyrna_id_FromPyObject(py_evaluated_object, &evaluated_object_id)) {
    PyErr_Format(
        PyExc_TypeError, "Expected an Object, not %.200s", Py_TYPE(py_evaluated_object)->tp_name);
    return nullptr;
  }
  if (GS(evaluated_object_id->name) != ID_OB) {
    PyErr_Format(PyExc_TypeError,
                 "Expected an Object, not %.200s",
                 BKE_idtype_idcode_to_name(GS(evaluated_object_id->name)));
    return nullptr;
  }
  Object *evaluated_object = reinterpret_cast<Object *>(evaluated_object_id);
  if (!DEG_is_evaluated_object(evaluated_object)) {
    PyErr_SetString(PyExc_TypeError, "Expected an evaluated object");
    return nullptr;
  }
  if (!DEG_object_geometry_is_evaluated(*evaluated_object)) {
    PyErr_SetString(PyExc_TypeError,
                    "Object geometry is not yet evaluated, is the depsgraph evaluated?");
    return nullptr;
  }
  BPy_GeometrySet *self = new_empty_BPy_GeometrySet();
  self->geometry = blender::bke::object_get_evaluated_geometry_set(*evaluated_object);
  return self;
}

static PyObject *BPy_GeometrySet_repr(BPy_GeometrySet *self)
{
  std::stringstream ss;
  ss << self->geometry;
  std::string str = ss.str();
  return PyUnicode_FromString(str.c_str());
}

static PyObject *BPy_GeometrySet_get_mesh(BPy_GeometrySet *self, PyObject *args, PyObject *kwds)
{
  static const char *kwlist[] = {"readonly", nullptr};
  bool readonly = false;
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$p", const_cast<char **>(kwlist), &readonly)) {
    return nullptr;
  }
  if (readonly) {
    const Mesh *mesh = self->geometry.get_mesh();
    return pyrna_id_CreatePyObject(const_cast<ID *>(reinterpret_cast<const ID *>(mesh)));
  }
  Mesh *mesh = self->geometry.get_mesh_for_write();
  return pyrna_id_CreatePyObject(reinterpret_cast<ID *>(mesh));
}

static PyObject *BPy_GeometrySet_get_curves(BPy_GeometrySet *self, PyObject *args, PyObject *kwds)
{
  static const char *kwlist[] = {"readonly", nullptr};
  bool readonly = false;
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$p", const_cast<char **>(kwlist), &readonly)) {
    return nullptr;
  }
  if (readonly) {
    const Curves *curves = self->geometry.get_curves();
    return pyrna_id_CreatePyObject(const_cast<ID *>(reinterpret_cast<const ID *>(curves)));
  }
  Curves *curves = self->geometry.get_curves_for_write();
  return pyrna_id_CreatePyObject(reinterpret_cast<ID *>(curves));
}

static PyObject *BPy_GeometrySet_get_volume(BPy_GeometrySet *self, PyObject *args, PyObject *kwds)
{
  static const char *kwlist[] = {"readonly", nullptr};
  bool readonly = false;
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$p", const_cast<char **>(kwlist), &readonly)) {
    return nullptr;
  }
  if (readonly) {
    const Volume *volume = self->geometry.get_volume();
    return pyrna_id_CreatePyObject(const_cast<ID *>(reinterpret_cast<const ID *>(volume)));
  }
  Volume *volume = self->geometry.get_volume_for_write();
  return pyrna_id_CreatePyObject(reinterpret_cast<ID *>(volume));
}

static PyObject *BPy_GeometrySet_get_grease_pencil(BPy_GeometrySet *self,
                                                   PyObject *args,
                                                   PyObject *kwds)
{
  static const char *kwlist[] = {"readonly", nullptr};
  bool readonly = false;
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$p", const_cast<char **>(kwlist), &readonly)) {
    return nullptr;
  }
  if (readonly) {
    const GreasePencil *grease_pencil = self->geometry.get_grease_pencil();
    return pyrna_id_CreatePyObject(const_cast<ID *>(reinterpret_cast<const ID *>(grease_pencil)));
  }
  GreasePencil *grease_pencil = self->geometry.get_grease_pencil_for_write();
  return pyrna_id_CreatePyObject(reinterpret_cast<ID *>(grease_pencil));
}

static PyObject *BPy_GeometrySet_get_pointcloud(BPy_GeometrySet *self,
                                                PyObject *args,
                                                PyObject *kwds)
{
  static const char *kwlist[] = {"readonly", nullptr};
  bool readonly = false;
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$p", const_cast<char **>(kwlist), &readonly)) {
    return nullptr;
  }
  if (readonly) {
    const PointCloud *pointcloud = self->geometry.get_pointcloud();
    return pyrna_id_CreatePyObject(const_cast<ID *>(reinterpret_cast<const ID *>(pointcloud)));
  }
  PointCloud *pointcloud = self->geometry.get_pointcloud_for_write();
  return pyrna_id_CreatePyObject(reinterpret_cast<ID *>(pointcloud));
}

static PyMethodDef BPy_GeometrySet_methods[] = {
    {"from_evaluated_object",
     reinterpret_cast<PyCFunction>(BPy_GeometrySet_static_from_evaluated_object),
     METH_VARARGS | METH_KEYWORDS | METH_STATIC,
     nullptr},
    {"get_mesh",
     reinterpret_cast<PyCFunction>(BPy_GeometrySet_get_mesh),
     METH_VARARGS | METH_KEYWORDS,
     nullptr},
    {"get_curves",
     reinterpret_cast<PyCFunction>(BPy_GeometrySet_get_curves),
     METH_VARARGS | METH_KEYWORDS,
     nullptr},
    {"get_volume",
     reinterpret_cast<PyCFunction>(BPy_GeometrySet_get_volume),
     METH_VARARGS | METH_KEYWORDS,
     nullptr},
    {"get_grease_pencil",
     reinterpret_cast<PyCFunction>(BPy_GeometrySet_get_grease_pencil),
     METH_VARARGS | METH_KEYWORDS,
     nullptr},
    {"get_pointcloud",
     reinterpret_cast<PyCFunction>(BPy_GeometrySet_get_pointcloud),
     METH_VARARGS | METH_KEYWORDS,
     nullptr},
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
    /*tp_repr*/ reinterpret_cast<reprfunc>(BPy_GeometrySet_repr),
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
    /*tp_new*/ reinterpret_cast<newfunc>(BPy_GeometrySet_new),
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
