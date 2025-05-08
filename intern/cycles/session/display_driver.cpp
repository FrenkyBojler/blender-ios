/* SPDX-FileCopyrightText: 2021-2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "session/display_driver.h"

#ifdef _WIN32
#  include "util/windows.h"
#else
#  include <unistd.h>
#endif

CCL_NAMESPACE_BEGIN

GraphicsInteropBuffer::~GraphicsInteropBuffer()
{
  clear();
}

void GraphicsInteropBuffer::clear()
{
  if (type == GraphicsInteropDevice::VULKAN && handle && need_recreate) {
#ifdef _WIN32
    CloseHandle(HANDLE(handle));
#else
    close(handle);
#endif
  }

  width = 0;
  height = 0;
  type = GraphicsInteropDevice::NONE;
  handle = 0;
  size = 0;
  need_clear = false;
  need_recreate = true;
}

void GraphicsInteropBuffer::take_ownership()
{
  need_recreate = false;
}

CCL_NAMESPACE_END
