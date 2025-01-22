/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "BLI_map.hh"

namespace blender::realtime_compositor {

class Context;
class Result;

/* ------------------------------------------------------------------------------------------------
 * Denoised Auxiliary Pass Key.
 */
class DenoisedAuxiliaryPassKey {
 public:
  std::string pass_name;

  DenoisedAuxiliaryPassKey(const char *pass_name);

  uint64_t hash() const;
};

bool operator==(const DenoisedAuxiliaryPassKey &a, const DenoisedAuxiliaryPassKey &b);

/* -------------------------------------------------------------------------------------------------
 * Denoised Result.
 *
 * A derived result that stores a denoised version of the source result, assuming it is either an
 * albedo or a normal pass. */
class DenoisedAuxiliaryPass {
 public:
  float *denoised_buffer = nullptr;

 public:
  DenoisedAuxiliaryPass(Context &context, const Result &source_result, const char *pass_name);

  ~DenoisedAuxiliaryPass();
};

/* ------------------------------------------------------------------------------------------------
 * Denoised Auxiliary Pass Container.
 */
class DenoisedAuxiliaryPassContainer {
 private:
  Map<DenoisedAuxiliaryPassKey, std::unique_ptr<DenoisedAuxiliaryPass>> map_;

 public:
  /* Check if there is an available DenoisedAuxiliaryPass derived resource with the given
   * parameters in the container, if one exists, return it, otherwise, return a newly created one
   * and add it to the container. pass_name is expected to be either "albedo" or "normal". */
  DenoisedAuxiliaryPass &get(Context &context, const Result &source_result, const char *pass_name);
};

}  // namespace blender::realtime_compositor
