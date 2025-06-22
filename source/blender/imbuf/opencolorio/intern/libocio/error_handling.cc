/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "error_handling.hh"

#if defined(WITH_OPENCOLORIO)

#  include "CLG_log.h"

#  include "../opencolorio.hh"

namespace blender::ocio {

void report_exception(const OCIO_NAMESPACE::Exception &exception)
{
  CLOG_ERROR(LOG_IMAGE_COLOR_MANAGEMENT, "OpenColorIO Error: %s", exception.what());
}

void report_error(const StringRefNull error)
{
  CLOG_ERROR(LOG_IMAGE_COLOR_MANAGEMENT, "OpenColorIO Error: %s", error.c_str());
}

}  // namespace blender::ocio

#endif
