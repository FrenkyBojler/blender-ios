/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file implements the BPyOpsCallable type, a C++ implementation of a callable
 * Blender operator for improved performance over Python-based wrappers.
 */

#include <Python.h>

#include "BLI_string.h"

#include "../generic/py_capi_utils.hh"
#include "../generic/python_compat.hh" /* IWYU pragma: keep. */

#include "bpy_operator_function.hh"

/** Utility functions for BPyOpsCallable. */
static bool BPyOpsCallable_parse_args(PyObject *args, const char **context_str, bool *is_undo)
{
  const char *C_exec = "EXEC_DEFAULT";
  bool C_undo = false;
  bool is_exec = false;
  bool is_undo_set = false;

  Py_ssize_t args_len = PyTuple_Size(args);
  for (Py_ssize_t i = 0; i < args_len; i++) {
    PyObject *arg = PyTuple_GET_ITEM(args, i);

    if (!is_exec && PyUnicode_Check(arg)) {
      if (is_undo_set) {
        PyErr_SetString(PyExc_ValueError, "string arg must come before the boolean");
        return false;
      }
      C_exec = PyUnicode_AsUTF8(arg);
      is_exec = true;
    }
    else if (!is_undo_set && (PyBool_Check(arg) || PyLong_Check(arg))) {
      C_undo = PyObject_IsTrue(arg);
      is_undo_set = true;
    }
    else {
      PyErr_SetString(PyExc_ValueError, "1-2 args execution context is supported");
      return false;
    }
  }

  *context_str = C_exec;
  *is_undo = C_undo;
  return true;
}

/**
 * Deallocate a BPyOpsCallable object.
 */
static void BPyOpsCallable_dealloc(BPyOpsCallable *self)
{
  Py_TYPE(self)->tp_free((PyObject *)self);
}

/**
 * __call__(context='EXEC_DEFAULT', undo=None, **kwargs)
 *
 * Execute the operator with the given parameters.
 *
 * :arg context: Execution context (optional)
 * :type context: str
 * :arg undo: Force undo behavior (optional)
 * :type undo: bool
 * :arg kwargs: Operator properties
 * :return: Set of completion status flags
 * :rtype: set[str]
 */
static PyObject *BPyOpsCallable_call(BPyOpsCallable *self, PyObject *args, PyObject *kwargs)
{
  /* Build args tuple for pyop_call: (opname, kw, ...extra args...).
   * Create the child objects first so we can handle allocation failures cleanly. */
  Py_ssize_t args_len = PyTuple_Size(args);
  PyObject *new_args = PyTuple_New(2 + args_len);
  if (!new_args) {
    return nullptr;
  }

  PyObject *opname = PyUnicode_FromString(self->idname_py);
  if (!opname) {
    Py_DECREF(new_args);
    return nullptr;
  }

  PyObject *kwobj = kwargs ? Py_NewRef(kwargs) : PyDict_New();
  if (!kwobj) {
    Py_DECREF(opname);
    Py_DECREF(new_args);
    return nullptr;
  }

  /* Steal references into the tuple. */
  PyTuple_SET_ITEM(new_args, 0, opname);
  PyTuple_SET_ITEM(new_args, 1, kwobj);

  for (Py_ssize_t i = 0; i < args_len; i++) {
    /* PyTuple_GET_ITEM returns a borrowed reference; create a new ref for the tuple. */
    PyObject *item = Py_NewRef(PyTuple_GET_ITEM(args, i));
    /* Py_NewRef should not fail for a valid borrowed item, but be defensive. */
    if (!item) {
      /* Cleanup: DECREF already-inserted items and the tuple container. */
      for (Py_ssize_t j = 0; j < i + 2; j++) {
        PyObject *tmp = PyTuple_GET_ITEM(new_args, j);
        Py_XDECREF(tmp);
      }
      Py_DECREF(new_args);
      return nullptr;
    }
    PyTuple_SET_ITEM(new_args, i + 2, item);
  }

  PyObject *result = pyop_call(nullptr, new_args);
  Py_DECREF(new_args);
  return result;
}

/**
 * Test if the operator can be executed in the current context.
 * This is the equivalent of the poll() method in Python.
 */
static PyObject *BPyOpsCallable_poll(BPyOpsCallable *self, PyObject *args)
{
  const char *context_str;
  bool is_undo;
  if (!BPyOpsCallable_parse_args(args, &context_str, &is_undo)) {
    return nullptr;
  }

  /* Create arguments for pyop_poll and check allocations carefully. */
  PyObject *poll_args = PyTuple_New(2);
  if (!poll_args) {
    return nullptr;
  }

  PyObject *idname_obj = PyUnicode_FromString(self->idname_py);
  if (!idname_obj) {
    Py_DECREF(poll_args);
    return nullptr;
  }

  PyObject *context_obj = PyUnicode_FromString(context_str);
  if (!context_obj) {
    Py_DECREF(idname_obj);
    Py_DECREF(poll_args);
    return nullptr;
  }

  PyTuple_SET_ITEM(poll_args, 0, idname_obj);
  PyTuple_SET_ITEM(poll_args, 1, context_obj);

  PyObject *result = pyop_poll(nullptr, poll_args);
  Py_DECREF(poll_args);
  return result;
}

/**
 * Get the RNA type definition for this operator.
 * Used for introspection and documentation generation.
 */
static PyObject *BPyOpsCallable_get_rna_type(BPyOpsCallable *self, PyObject * /*args*/)
{
  PyObject *idname_obj = PyUnicode_FromString(self->idname_bl);
  if (!idname_obj) {
    return nullptr;
  }

  PyObject *result = pyop_getrna_type(nullptr, idname_obj);
  Py_DECREF(idname_obj);
  return result;
}

/**
 * Get the documentation string for this operator.
 * Returns the operator signature for use in help() and documentation.
 */
static PyObject *BPyOpsCallable_get_doc(BPyOpsCallable *self)
{
  PyObject *args = PyTuple_New(1);
  if (!args) {
    return nullptr;
  }
  PyTuple_SET_ITEM(args, 0, PyUnicode_FromString(self->idname_py));

  PyObject *result = pyop_as_string(nullptr, args);
  Py_DECREF(args);

  if (!result) {
    /* Fallback to simple string if pyop_as_string fails. */
    PyErr_Clear();
    result = PyUnicode_FromFormat("bpy.ops.%s(...)", self->idname_py);
  }

  return result;
}

/**
 * Return a string representation of the operator for debugging.
 */
static PyObject *BPyOpsCallable_repr(BPyOpsCallable *self)
{
  return PyUnicode_FromFormat("<bpy.ops.%s callable>", self->idname_py);
}

/**
 * Return a user-friendly string representation of the operator.
 */
static PyObject *BPyOpsCallable_str(BPyOpsCallable *self)
{
  /* Extract module and function from idname_py. */
  const char *dot_pos = strchr(self->idname_py, '.');
  if (!dot_pos) {
    return PyUnicode_FromFormat("<function bpy.ops.%s at %p>", self->idname_py, (void *)self);
  }

  size_t module_len = dot_pos - self->idname_py;
  char module[OP_MAX_TYPENAME];
  char func[OP_MAX_TYPENAME];

  BLI_strncpy(module, self->idname_py, module_len + 1);
  BLI_strncpy(func, dot_pos + 1, sizeof(func));

  return PyUnicode_FromFormat("<function bpy.ops.%s.%s at %p>", module, func, (void *)self);
}

/* Docstrings for BPyOpsCallable methods. */
PyDoc_STRVAR(BPyOpsCallable_poll_doc,
             "poll(context='EXEC_DEFAULT')\n"
             "\n"
             "Test if the operator can be executed in the current context.\n"
             "\n"
             ":arg context: Execution context (optional)\n"
             ":type context: str\n"
             ":return: True if the operator can be executed\n"
             ":rtype: bool");

PyDoc_STRVAR(BPyOpsCallable_get_rna_type_doc,
             "get_rna_type()\n"
             "\n"
             "Get the RNA type definition for this operator.\n"
             "\n"
             ":return: RNA type object for introspection\n"
             ":rtype: bpy.types.Struct");

/* Method definitions for BPyOpsCallable. */
static PyMethodDef BPyOpsCallable_methods[] = {
    {"poll", (PyCFunction)BPyOpsCallable_poll, METH_VARARGS, BPyOpsCallable_poll_doc},
    {"get_rna_type",
     (PyCFunction)BPyOpsCallable_get_rna_type,
     METH_NOARGS,
     BPyOpsCallable_get_rna_type_doc},
    {nullptr, nullptr, 0, nullptr}};

/**
 * Get the bl_options property for this operator.
 * Returns a set of option flags like 'REGISTER', 'UNDO', etc.
 */
static PyObject *BPyOpsCallable_get_bl_options_property(BPyOpsCallable *self, void * /*closure*/)
{
  PyObject *idname_obj = PyUnicode_FromString(self->idname_bl);
  if (!idname_obj) {
    return nullptr;
  }

  PyObject *result = pyop_get_bl_options(nullptr, idname_obj);
  Py_DECREF(idname_obj);
  return result;
}

static PyObject *BPyOpsCallable_get_doc_property(BPyOpsCallable *self, void * /*closure*/)
{
  return BPyOpsCallable_get_doc(self);
}

static PyGetSetDef BPyOpsCallable_getsetters[] = {
    {"bl_options",
     (getter)BPyOpsCallable_get_bl_options_property,
     nullptr,
     "Set of option flags for this operator (e.g. 'REGISTER', 'UNDO')",
     nullptr},
    {"__doc__",
     (getter)BPyOpsCallable_get_doc_property,
     nullptr,
     "Operator documentation string with signature and description",
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}};

/* Define the documentation string for the BPyOpsCallableType. */
PyDoc_STRVAR(
    BPyOpsCallableType_doc,
    "Callable Blender operator.\n"
    "\n"
    "This object represents a Blender operator that can be called to perform actions.\n"
    "Operators are the primary way to interact with Blender's functionality from Python.\n"
    "\n"
    "Usage:\n"
    "   bpy.ops.object.select_all(action='SELECT')\n"
    "   \n"
    "   # Check if operator can run\n"
    "   if bpy.ops.object.select_all.poll():\n"
    "       bpy.ops.object.select_all(action='DESELECT')\n");

PyTypeObject BPyOpsCallableType = {
    /*ob_base*/ PyVarObject_HEAD_INIT(nullptr, 0)
    /*tp_name*/ "BPyOpsCallable",
    /*tp_basicsize*/ sizeof(BPyOpsCallable),
    /*tp_itemsize*/ 0,
    /*tp_dealloc*/ (destructor)BPyOpsCallable_dealloc,
    /*tp_print*/ 0,
    /*tp_getattr*/ nullptr,
    /*tp_setattr*/ nullptr,
    /*tp_as_async*/ nullptr,
    /*tp_repr*/ (reprfunc)BPyOpsCallable_repr,
    /*tp_as_number*/ nullptr,
    /*tp_as_sequence*/ nullptr,
    /*tp_as_mapping*/ nullptr,
    /*tp_hash*/ nullptr,
    /*tp_call*/ (ternaryfunc)BPyOpsCallable_call,
    /*tp_str*/ (reprfunc)BPyOpsCallable_str,
    /*tp_getattro*/ PyObject_GenericGetAttr,
    /*tp_setattro*/ nullptr,
    /*tp_as_buffer*/ nullptr,
    /*tp_flags*/ Py_TPFLAGS_DEFAULT,
    /*tp_doc*/ BPyOpsCallableType_doc,
    /*tp_traverse*/ nullptr,
    /*tp_clear*/ nullptr,
    /*tp_richcompare*/ nullptr,
    /*tp_weaklistoffset*/ 0,
    /*tp_iter*/ nullptr,
    /*tp_iternext*/ nullptr,
    /*tp_methods*/ BPyOpsCallable_methods,
    /*tp_members*/ nullptr,
    /*tp_getset*/ BPyOpsCallable_getsetters,
};

/**
 * Initialize the BPyOpsCallable type.
 * This must be called before using any BPyOpsCallable functions.
 *
 * :return: 0 on success, -1 on failure
 */
int BPy_OpsCallable_InitTypes(void)
{
  if (PyType_Ready(&BPyOpsCallableType) < 0) {
    return -1;
  }
  return 0;
}

/**
 * Create a new BPyOpsCallable object for the given operator module and function.
 */
PyObject *pyop_create_function(PyObject * /*self*/, PyObject *args)
{
  const char *module, *func;

  if (!PyArg_ParseTuple(args, "ss", &module, &func)) {
    return nullptr;
  }

  /* Create a new BPyOpsCallable instance for direct C++ operator execution. */
  BPyOpsCallable *callable = (BPyOpsCallable *)PyObject_New(BPyOpsCallable, &BPyOpsCallableType);
  if (!callable) {
    return nullptr;
  }

  /* Validate operator name lengths before constructing strings. */
  size_t py_len = strlen(module) + 1 + strlen(func) + 1; /* "." + null terminator */
  size_t bl_len = strlen(module) + 4 + strlen(func) + 1; /* "_OT_" + null terminator */
  if (py_len > OP_MAX_TYPENAME || bl_len > OP_MAX_TYPENAME) {
    PyErr_Format(PyExc_ValueError, "Operator name too long: %s.%s", module, func);
    Py_DECREF(callable);
    return nullptr;
  }

  /* Construct the Python idname (e.g., "object.select_all") */
  int py_result = snprintf(
      callable->idname_py, sizeof(callable->idname_py), "%s.%s", module, func);
  if (py_result < 0 || py_result >= int(sizeof(callable->idname_py))) {
    PyErr_Format(PyExc_ValueError, "Failed to format operator name: %s.%s", module, func);
    Py_DECREF(callable);
    return nullptr;
  }

  /* Construct the Blender idname (e.g., "OBJECT_OT_select_all") */
  char module_upper[OP_MAX_TYPENAME];
  BLI_strncpy(module_upper, module, sizeof(module_upper));
  BLI_str_toupper_ascii(module_upper, sizeof(module_upper));

  int bl_result = snprintf(
      callable->idname_bl, sizeof(callable->idname_bl), "%s_OT_%s", module_upper, func);
  if (bl_result < 0 || bl_result >= int(sizeof(callable->idname_bl))) {
    PyErr_Format(PyExc_ValueError, "Failed to format operator name: %s.%s", module, func);
    Py_DECREF(callable);
    return nullptr;
  }

  return (PyObject *)callable;
}
