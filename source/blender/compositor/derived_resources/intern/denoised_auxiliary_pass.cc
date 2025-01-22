/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstdint>
#include <memory>

#include "BLI_assert.h"
#include "BLI_hash.hh"
#include "BLI_string_ref.hh"
#include "BLI_system.h"
#include "BLI_utildefines.h"

#include "GPU_texture.hh"

#include "COM_context.hh"
#include "COM_denoised_auxiliary_pass.hh"
#include "COM_result.hh"

#ifdef WITH_OPENIMAGEDENOISE
#  include <OpenImageDenoise/oidn.hpp>
#endif

namespace blender::realtime_compositor {

/* ------------------------------------------------------------------------------------------------
 * Denoised Auxiliary Pass Key.
 */
DenoisedAuxiliaryPassKey::DenoisedAuxiliaryPassKey(const char *pass_name) : pass_name(pass_name) {}

uint64_t DenoisedAuxiliaryPassKey::hash() const
{
  return get_default_hash(pass_name);
}

bool operator==(const DenoisedAuxiliaryPassKey &a, const DenoisedAuxiliaryPassKey &b)
{
  return a.pass_name == b.pass_name;
}

/* --------------------------------------------------------------------
 * Denoised Auxiliary Pass.
 */

/* A callback to cancel the filter operations by evaluating the context's is_canceled method. The
 * API specifies that true indicates the filter should continue, while false indicates it should
 * stop, so invert the condition. This callback can also be used to track progress using the given
 * n argument, but we currently don't make use of it. See OIDNProgressMonitorFunction in the API
 * for more information. */
[[maybe_unused]] static bool oidn_progress_monitor_function(void *user_ptr, double /*n*/)
{
  const Context *context = static_cast<const Context *>(user_ptr);
  return !context->is_canceled();
}

/* OIDN can be disabled as a build option, so check WITH_OPENIMAGEDENOISE. Additionally, it is
 * only supported at runtime for CPUs that supports SSE4.1, except for MacOS where it is always
 * supported through the Accelerate framework BNNS on macOS. */
static bool is_oidn_supported()
{
#ifndef WITH_OPENIMAGEDENOISE
  return false;
#else
#  ifdef __APPLE__
  return true;
#  else
  return BLI_cpu_support_sse42();
#  endif
#endif
}

DenoisedAuxiliaryPass::DenoisedAuxiliaryPass(Context &context,
                                             const Result &source_result,
                                             const char *pass_name)
{
  this->denoised_buffer = static_cast<float *>(
      GPU_texture_read(source_result.texture(), GPU_DATA_FLOAT, 0));

  if (!is_oidn_supported() || source_result.is_single_value()) {
    return;
  }

  const int width = source_result.domain().size.x;
  const int height = source_result.domain().size.y;
  const int pixel_stride = sizeof(float) * 4;

#ifdef WITH_OPENIMAGEDENOISE
  oidn::DeviceRef device = oidn::newDevice(oidn::DeviceType::CPU);
  device.commit();

  oidn::FilterRef filter = device.newFilter("RT");
  filter.setImage(
      pass_name, this->denoised_buffer, oidn::Format::Float3, width, height, 0, pixel_stride);
  filter.setImage(
      "output", this->denoised_buffer, oidn::Format::Float3, width, height, 0, pixel_stride);
  filter.setProgressMonitorFunction(oidn_progress_monitor_function, &context);
  filter.commit();
  filter.execute();
#endif
}

DenoisedAuxiliaryPass::~DenoisedAuxiliaryPass()
{
  MEM_freeN(this->denoised_buffer);
}

/* --------------------------------------------------------------------
 * Denoised Auxiliary Pass Container.
 */

DenoisedAuxiliaryPass &DenoisedAuxiliaryPassContainer::get(Context &context,
                                                           const Result &source_result,
                                                           const char *pass_name)
{
  BLI_assert(ELEM(StringRef(pass_name), "albedo", "normal"));

  const DenoisedAuxiliaryPassKey key(pass_name);

  return *map_.lookup_or_add_cb(key, [&]() {
    return std::make_unique<DenoisedAuxiliaryPass>(context, source_result, pass_name);
  });
}

}  // namespace blender::realtime_compositor
