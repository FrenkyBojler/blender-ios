/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 */

#pragma once

#include <Python.h>

namespace blender {

extern bool BPY_driver_secure_bytecode_test_ex(PyObject *expr_code,
                                               PyObject *py_namespace_array[],
                                               const bool verbose,
                                               const char *error_prefix);

}  // namespace blender
