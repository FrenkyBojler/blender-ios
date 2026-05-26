/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 */

#pragma once

#include <Python.h>

#include <string>

namespace blender {

/* GPU Device Python object structure */
typedef struct {
  PyObject_HEAD int index;
  std::string identifier;
  std::string name;
} BPyGPUDevice;

extern PyTypeObject BPyGPU_DeviceType;

[[nodiscard]] PyObject *bpygpu_platform_init();

}  // namespace blender
