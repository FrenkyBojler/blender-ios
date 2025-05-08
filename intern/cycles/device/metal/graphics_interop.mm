/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_METAL

#  include "device/metal/graphics_interop.h"

#  include "device/metal/device_impl.h"

CCL_NAMESPACE_BEGIN

MetalDeviceGraphicsInterop::MetalDeviceGraphicsInterop(MetalDeviceQueue *queue)
    : queue_(queue), device_(static_cast<MetalDevice *>(queue->device))
{
}

MetalDeviceGraphicsInterop::~MetalDeviceGraphicsInterop() = default;

void MetalDeviceGraphicsInterop::set_buffer(GraphicsInteropBuffer &interop_buffer)
{
  /* Trivial implementation due to unified memory. */
  if (interop_buffer.type != GraphicsInteropDevice::METAL) {
    return;
  }

  need_clear_ |= interop_buffer.need_clear;

  if (!interop_buffer.need_recreate) {
    return;
  }

  buffer_ = reinterpret_cast<void *>(interop_buffer.handle);
  size_ = interop_buffer.width * interop_buffer.height * sizeof(half4);

  interop_buffer.take_ownership();
}

device_ptr MetalDeviceGraphicsInterop::map()
{
  if (buffer_ && need_clear_) {
    memset(buffer_, 0, size_);
  }

  return device_ptr(buffer_);
}

void MetalDeviceGraphicsInterop::unmap() {}

CCL_NAMESPACE_END

#endif
