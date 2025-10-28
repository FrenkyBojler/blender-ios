/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 */

#pragma once

#include <Python.h>

[[nodiscard]] PyObject *BPY_operator_module();

PyObject *op_handler_append(PyObject *self, PyObject *args, PyObject *kw);
PyObject *op_handler_remove(PyObject *self, PyObject *args, PyObject *kw);
