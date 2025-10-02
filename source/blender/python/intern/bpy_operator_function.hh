/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file defines the BPyOpFunction type, a C++ implementation of a callable
 * Blender operator for improved performance over Python-based wrappers.
 */

#pragma once

#include "DNA_windowmanager_types.h"
#include <Python.h>

/** C++ Operator Callable Type. */
typedef struct {
  PyObject_HEAD
  /** Cached operator ID name (e.g., "OBJECT_OT_select_all"). */
  char idname[OP_MAX_TYPENAME];
} BPyOpFunction;

extern PyTypeObject BPyOpFunctionType;

#define BPy_OpsCallable_Check(v) (PyObject_TypeCheck(v, &BPyOpFunctionType))

/* Forward declarations for external functions from bpy_operator.cc. */
PyObject *pyop_poll(PyObject *self, PyObject *args);
PyObject *pyop_call(PyObject *self, PyObject *args);
PyObject *pyop_as_string(PyObject *self, PyObject *args);
PyObject *pyop_getrna_type(PyObject *self, PyObject *value);
PyObject *pyop_get_bl_options(PyObject *self, PyObject *value);

/**
 * Create a new BPyOpFunction object for the given operator module and function.
 *
 * \param self Unused (required by Python C API).
 * \param args Python tuple containing module and function name strings.
 * \return A new #BPyOpFunction object or NULL on error.
 */
PyObject *pyop_create_function(PyObject *self, PyObject *args);

/**
 * Initialize the BPyOpFunction type.
 * This must be called before using any BPyOpFunction functions.
 *
 * \return 0 on success, -1 on failure
 */
int BPyOpFunction_InitTypes(void);
