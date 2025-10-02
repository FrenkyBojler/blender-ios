/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file defines the BPyOpsCallable type, a C++ implementation of a callable
 * Blender operator for improved performance over Python-based wrappers.
 */

#pragma once

#include "DNA_windowmanager_types.h"
#include <Python.h>

/** C++ Operator Callable Type. */
typedef struct {
  PyObject_HEAD
  /** Cached operator identifier (e.g., "object.select_all").
   * This bypasses Python's __call__ overhead for performance optimization. */
  char idname_py[OP_MAX_TYPENAME];
  /** Cached operator ID name (e.g., "OBJECT_OT_select_all"). */
  char idname_bl[OP_MAX_TYPENAME];
} BPyOpsCallable;

extern PyTypeObject BPyOpsCallableType;

#define BPy_OpsCallable_Check(v) (PyObject_TypeCheck(v, &BPyOpsCallableType))

/* Forward declarations for external functions from bpy_operator.cc. */
PyObject *pyop_poll(PyObject *self, PyObject *args);
PyObject *pyop_call(PyObject *self, PyObject *args);
PyObject *pyop_as_string(PyObject *self, PyObject *args);
PyObject *pyop_getrna_type(PyObject *self, PyObject *value);
PyObject *pyop_get_bl_options(PyObject *self, PyObject *value);

/** Create a new BPyOpsCallable object for the given operator module and function. */
PyObject *pyop_create_function(PyObject *self, PyObject *args);

/**
 * Initialize the BPyOpsCallable type.
 * This must be called before using any BPyOpsCallable functions.
 *
 * :return: 0 on success, -1 on failure
 */
int BPy_OpsCallable_InitTypes(void);
