/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <sstream>

#include "BKE_duplilist.hh"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_idtype.hh"
#include "BKE_instances.hh"
#include "BKE_lib_id.hh"
#include "BKE_pointcloud.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_ID.h"
#include "DNA_collection_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"

#include "RNA_prototypes.hh"

#include "bpy_geometry_set.hh"
#include "bpy_rna.hh"

using blender::bke::GeometrySet;

extern PyTypeObject bpy_geometry_set_Type;

struct BPy_GeometrySet {
  PyObject_HEAD
  GeometrySet geometry;
  PointCloud *instances_pointcloud;
};

static BPy_GeometrySet *python_object_from_geometry_set(GeometrySet geometry = {})
{
  BPy_GeometrySet *self = reinterpret_cast<BPy_GeometrySet *>(
      bpy_geometry_set_Type.tp_alloc(&bpy_geometry_set_Type, 0));
  if (self == nullptr) {
    return nullptr;
  }
  new (&self->geometry) GeometrySet(std::move(geometry));
  self->instances_pointcloud = nullptr;
  return self;
}

static BPy_GeometrySet *BPy_GeometrySet_new(PyTypeObject * /*type*/,
                                            PyObject * /*args*/,
                                            PyObject * /*kwds*/)
{
  return python_object_from_geometry_set();
}

static void BPy_GeometrySet_dealloc(BPy_GeometrySet *self)
{
  std::destroy_at(&self->geometry);
  if (self->instances_pointcloud) {
    BKE_id_free(nullptr, self->instances_pointcloud);
  }
  Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

static BPy_GeometrySet *BPy_GeometrySet_static_from_evaluated_object(PyObject * /*self*/,
                                                                     PyObject *args,
                                                                     PyObject *kwds)
{
  static const char *kwlist[] = {"evaluated_object", "depsgraph", nullptr};
  PyObject *py_evaluated_object;
  PyObject *py_depsgraph;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "OO", const_cast<char **>(kwlist), &py_evaluated_object, &py_depsgraph))
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
  if (!BPy_StructRNA_Check(py_depsgraph)) {
    PyErr_SetString(PyExc_TypeError, "Expected a depsgraph");
    return nullptr;
  }
  BPy_StructRNA *rna_depsgraph = reinterpret_cast<BPy_StructRNA *>(py_depsgraph);
  if (!rna_depsgraph->ptr || !RNA_struct_is_a(rna_depsgraph->ptr->type, &RNA_Depsgraph)) {
    PyErr_SetString(PyExc_TypeError, "Expected a depsgraph");
    return nullptr;
  }
  Depsgraph *depsgraph = static_cast<Depsgraph *>(rna_depsgraph->ptr->data);
  Scene *scene = DEG_get_input_scene(depsgraph);

  blender::bke::Instances instances = object_duplistlist_legacy_instances(
      *depsgraph, *scene, *evaluated_object);
  GeometrySet geometry = blender::bke::object_get_evaluated_geometry_set(*evaluated_object);
  if (instances.instances_num() > 0) {
    geometry.replace_instances(new blender::bke::Instances(std::move(instances)));
  }
  BPy_GeometrySet *self = python_object_from_geometry_set(std::move(geometry));
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

static PyObject *BPy_GeometrySet_get_instances_pointcloud(BPy_GeometrySet *self)
{
  using namespace blender;
  const bke::Instances *instances = self->geometry.get_instances();
  if (!instances) {
    Py_RETURN_NONE;
  }
  if (self->instances_pointcloud == nullptr) {
    const int instances_num = instances->instances_num();
    PointCloud *pointcloud = BKE_pointcloud_new_nomain(instances_num);
    bke::gather_attributes(instances->attributes(),
                           bke::AttrDomain::Instance,
                           bke::AttrDomain::Point,
                           {},
                           IndexMask(instances_num),
                           pointcloud->attributes_for_write());
    self->instances_pointcloud = pointcloud;
  }
  return pyrna_id_CreatePyObject(&self->instances_pointcloud->id);
}

static PyObject *BPy_GeometrySet_get_instance_references(BPy_GeometrySet *self)
{
  using namespace blender;
  const bke::Instances *instances = self->geometry.get_instances();
  if (!instances) {
    return PyList_New(0);
  }
  const Span<bke::InstanceReference> references = instances->references();
  PyObject *py_references = PyList_New(references.size());
  for (const int i : references.index_range()) {
    const bke::InstanceReference &reference = references[i];
    switch (reference.type()) {
      case bke::InstanceReference::Type::None: {
        PyList_SET_ITEM(py_references, i, Py_NewRef(Py_None));
        break;
      }
      case bke::InstanceReference::Type::Object: {
        Object &object = reference.object();
        PyList_SET_ITEM(py_references, i, pyrna_id_CreatePyObject(&object.id));
        break;
      }
      case bke::InstanceReference::Type::Collection: {
        Collection &collection = reference.collection();
        PyList_SET_ITEM(py_references, i, pyrna_id_CreatePyObject(&collection.id));
        break;
      }
      case bke::InstanceReference::Type::GeometrySet: {
        const bke::GeometrySet &geometry_set = reference.geometry_set();
        PyList_SET_ITEM(py_references, i, python_object_from_geometry_set(geometry_set));
        break;
      }
    }
  }
  return py_references;
}

static PyObject *BPy_GeometrySet_get_name(BPy_GeometrySet *self, void * /*closure*/)
{
  return PyUnicode_FromString(self->geometry.name.c_str());
}

static int BPy_GeometrySet_set_name(BPy_GeometrySet *self, PyObject *value, void * /*closure*/)
{
  if (!PyUnicode_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "expected a string");
    return -1;
  }
  const char *name = PyUnicode_AsUTF8(value);
  self->geometry.name = name ? name : "";
  return 0;
}

static PyGetSetDef BPy_GeometrySet_getseters[] = {
    {
        "name",
        reinterpret_cast<getter>(BPy_GeometrySet_get_name),
        reinterpret_cast<setter>(BPy_GeometrySet_set_name),
        "The name of the geometry set.",
        nullptr,
    },
    {nullptr},

};

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
    {"get_instances_pointcloud",
     reinterpret_cast<PyCFunction>(BPy_GeometrySet_get_instances_pointcloud),
     METH_NOARGS,
     nullptr},
    {"get_instance_references",
     reinterpret_cast<PyCFunction>(BPy_GeometrySet_get_instance_references),
     METH_NOARGS,
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
    /*tp_getset*/ BPy_GeometrySet_getseters,
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
