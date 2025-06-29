/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This adds helpers to #uiLayout which can't be added easily to RNA itself.
 */
#define PY_SSIZE_T_CLEAN

#include <Python.h>

#include "MEM_guardedalloc.h"

#include "../generic/py_capi_utils.hh"

#include "UI_interface_layout.hh"

#include "bpy_rna.hh"
#include "bpy_rna_ui.hh" /* Declare #BPY_rna_uilayout_introspect_method_def. */

PyDoc_STRVAR(
    /* Wrap. */
    bpy_rna_uilayout_introspect_doc,
    ".. method:: introspect()\n"
    "\n"
    "   Return a dictionary containing a textual representation of the UI layout.\n");
static PyObject *bpy_rna_uilayout_introspect(PyObject *self)
{
  BPy_StructRNA *pyrna = (BPy_StructRNA *)self;
  uiLayout *layout = static_cast<uiLayout *>(pyrna->ptr->data);

  const char *expr = UI_layout_introspect(layout);
  PyObject *main_mod = PyC_MainModule_Backup();
  PyObject *py_dict = PyC_DefaultNameSpace("<introspect>");
  PyObject *result = PyRun_String(expr, Py_eval_input, py_dict, py_dict);
  MEM_freeN(expr);
  Py_DECREF(py_dict);
  PyC_MainModule_Restore(main_mod);
  return result;
}

PyDoc_STRVAR(
    /* Wrap. */
    bpy_rna_uilayout_context_string_get_doc,
    ".. method::context_string_get(name)\n"
    "   :arg name: Name of entry in the context.\n"
    "   :type name: str\n"
    "\n"
    "   :return: String set in the context\n"
    "   :rtype: str | None\n");
static PyObject *bpy_rna_uilayout_context_string_get(PyObject *self, PyObject *value)
{
  Py_ssize_t name_str_len;
  const char *name = PyUnicode_AsUTF8AndSize(value, &name_str_len);
  if (name == nullptr) {
    PyErr_SetString(PyExc_TypeError, "expected name paramether");
    return nullptr;
  }
  BPy_StructRNA *pyrna = (BPy_StructRNA *)self;
  uiLayout *layout = static_cast<uiLayout *>(pyrna->ptr->data);
  std::optional<blender::StringRefNull> context_string = layout->context_string_get(
      blender::StringRef(name, name_str_len));
  if (!context_string) {
    Py_RETURN_NONE;
  }
  return PyUnicode_FromStringAndSize(context_string.value().c_str(),
                                     context_string.value().size());
}

PyDoc_STRVAR(
    /* Wrap. */
    bpy_rna_uilayout_context_int_get_doc,
    ".. method::context_int_get(name)\n"
    "   :arg name: Name of entry in the context.\n"
    "   :type name: str\n"
    "\n"
    "   :return: Int set in the context\n"
    "   :rtype: int | None\n");
static PyObject *bpy_rna_uilayout_context_int_get(PyObject *self, PyObject *value)
{
  Py_ssize_t name_str_len;
  const char *name = PyUnicode_AsUTF8AndSize(value, &name_str_len);
  if (name == nullptr) {
    PyErr_SetString(PyExc_TypeError, "expected name paramether");
    return nullptr;
  }
  BPy_StructRNA *pyrna = (BPy_StructRNA *)self;
  uiLayout *layout = static_cast<uiLayout *>(pyrna->ptr->data);
  std::optional<int64_t> context_int = layout->context_int_get(blender::StringRef(name, name_str_len));
  if (!context_int) {
    Py_RETURN_NONE;
  }
  return PyLong_FromLongLong(context_int.value());
}

PyDoc_STRVAR(
    /* Wrap. */
    bpy_rna_uilayout_context_int_set_doc,
    ".. method::context_int_set(name,value)\n"
    "   :arg name: Name of entry in the context.\n"
    "   :type name: str\n"
    "   :arg value: Int to put in the context.\n"
    "   :type value: int\n");
static PyObject *bpy_rna_uilayout_context_int_set(PyObject *self, PyObject *args)
{
  const char *name = nullptr;
  Py_ssize_t name_len = 0;
  uint64_t value;

  if (!PyArg_ParseTuple(args, "s#L:context_int_set", &name, &name_len, &value)) {
    return nullptr;
  }
  if (name == nullptr) {
    PyErr_SetString(PyExc_TypeError, "expected name paramether");
    return nullptr;
  }
  BPy_StructRNA *pyrna = (BPy_StructRNA *)self;
  uiLayout *layout = static_cast<uiLayout *>(pyrna->ptr->data);
  layout->context_int_set(blender::StringRef(name, name_len), value);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    bpy_rna_uilayout_context_pointer_get_doc,
    ".. method::context_pointer_get(name,type)\n"
    "   :arg name: Name of entry in the context.\n"
    "   :type name: str\n"
    "   :arg type: RNA subclass of entry in the context.\n"
    "   :type type: :class:`bpy.types.Struct`\n"
    "\n"
    "   :return: RNA pointer set in the context.\n"
    "   :rtype: object | None\n");
static PyObject *bpy_rna_uilayout_context_pointer_get(PyObject *self, PyObject *args)
{
  const char *name = nullptr;
  Py_ssize_t name_len = 0;
  PyObject *py_type;

  if (!PyArg_ParseTuple(args, "s#O:context_pointer_get", &name, &name_len, &py_type)) {
    return nullptr;
  }
  if (name == nullptr) {
    PyErr_SetString(PyExc_TypeError, "expected name paramether");
    return nullptr;
  }
  if (py_type == nullptr) {
    PyErr_SetString(PyExc_TypeError, "expected type paramether");
    return nullptr;
  }
  const char *error_prefix = "UILayout.context_pointer_get";
  if (!PyType_Check(py_type)) {
    PyErr_Format(PyExc_ValueError,
                 "%s expected a class argument, not '%.200s'",
                 error_prefix,
                 Py_TYPE(py_type)->tp_name);
    return nullptr;
  }
  const StructRNA *srna = pyrna_struct_as_srna(py_type, false, error_prefix);
  if (!srna) {
    return nullptr;
  }
  BPy_StructRNA *pyrna = (BPy_StructRNA *)self;
  uiLayout *layout = static_cast<uiLayout *>(pyrna->ptr->data);
  const PointerRNA *ptr = layout->context_ptr_get(blender::StringRef(name, name_len), srna);
  if (!ptr) {
    Py_RETURN_NONE;
  }
  PointerRNA r_ptr = *ptr;
  return pyrna_struct_CreatePyObject(&r_ptr);
}

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wcast-function-type"
#  else
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-function-type"
#  endif
#endif

PyMethodDef BPY_rna_uilayout_introspect_method_def = {
    "introspect",
    (PyCFunction)bpy_rna_uilayout_introspect,
    METH_NOARGS,
    bpy_rna_uilayout_introspect_doc,
};

PyMethodDef BPY_rna_uilayout_context_string_get_def = {
    "context_string_get",
    (PyCFunction)bpy_rna_uilayout_context_string_get,
    METH_O,
    bpy_rna_uilayout_context_string_get_doc,
};

PyMethodDef BPY_rna_uilayout_context_int_get_def = {
    "context_int_get",
    (PyCFunction)bpy_rna_uilayout_context_int_get,
    METH_O,
    bpy_rna_uilayout_context_int_get_doc,
};
PyMethodDef BPY_rna_uilayout_context_int_set_def = {
    "context_int_set",
    (PyCFunction)bpy_rna_uilayout_context_int_set,
    METH_VARARGS,
    bpy_rna_uilayout_context_int_set_doc,
};

PyMethodDef BPY_rna_uilayout_context_pointer_get_def = {
    "context_pointer_get",
    (PyCFunction)bpy_rna_uilayout_context_pointer_get,
    METH_VARARGS,
    bpy_rna_uilayout_context_pointer_get_doc,
};

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic pop
#  else
#    pragma GCC diagnostic pop
#  endif
#endif
