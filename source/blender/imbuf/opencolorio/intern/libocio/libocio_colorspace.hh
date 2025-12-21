/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#if defined(WITH_OPENCOLORIO)

#  include <string>

#  include "MEM_guardedalloc.h"

#  include "OCIO_colorspace.hh"

#  include "../cpu_processor_cache.hh"
#  include "../opencolorio.hh"

namespace blender::ocio {

class LibOCIOColorSpace : public ColorSpace {
  OCIO_NAMESPACE::ConstConfigRcPtr ocio_config_;
  OCIO_NAMESPACE::ConstColorSpaceRcPtr ocio_color_space_;

  std::string clean_description_;
  StringRefNull interop_id_;
  bool is_primary_interop_id_ = false;

  bool is_invertible_ = false;

  /* Mutable because they are lazily initialized and cached from the is_scene_linear() and
   * is_srgb(). */
  mutable bool is_info_cached_ = false;
  mutable bool is_scene_linear_ = false;
  mutable bool is_srgb_ = false;

  mutable std::string icc_profile_path_cache_;
  mutable bool is_icc_profile_path_cached_ = false;

  CPUProcessorCache to_scene_linear_cpu_processor_;
  CPUProcessorCache from_scene_linear_cpu_processor_;

 public:
  LibOCIOColorSpace(int index,
                    const OCIO_NAMESPACE::ConstConfigRcPtr &ocio_config,
                    const OCIO_NAMESPACE::ConstColorSpaceRcPtr &ocio_color_space);

  StringRefNull name() const override
  {
    /* TODO(sergey): Avoid construction StringRefNull on every call? */
    return ocio_color_space_->getName();
  }
  StringRefNull description() const override
  {
    return clean_description_;
  }

  StringRefNull interop_id() const override
  {
    return interop_id_;
  }
  bool is_primary_interop_id() const override;

  StringRefNull icc_profile_name() const override
  {
    #if OCIO_VERSION_HEX >= 0x02050000
        if (!is_icc_profile_path_cached_) {
          const std::string icc_profile_name_str = ocio_color_space_->getICCProfileName();
          if (icc_profile_name_str.empty()) {
            icc_profile_path_cache_.clear();
          }
          else {
            try {
              icc_profile_path_cache_ = ocio_config_->getCurrentContext()->resolveFileLocation(
                  icc_profile_name_str);
            }
            catch (const OCIO_NAMESPACE::Exception &) {
              /* Fallback to original name if the path can't be resolved. */
              icc_profile_path_cache_ = icc_profile_name_str;
            }
          }
          is_icc_profile_path_cached_ = true;
        }
        return StringRefNull(icc_profile_path_cache_);
    #else
        return StringRefNull();
    #endif
  }

  bool is_invertible() const override
  {
    return is_invertible_;
  }

  bool is_scene_linear() const override;
  bool is_srgb() const override;

  bool is_data() const override
  {
    return ocio_color_space_->isData();
  }

  bool is_display_referred() const override
  {
    return ocio_color_space_->getReferenceSpaceType() == OCIO_NAMESPACE::REFERENCE_SPACE_DISPLAY;
  }

  const CPUProcessor *get_to_scene_linear_cpu_processor() const override;
  const CPUProcessor *get_from_scene_linear_cpu_processor() const override;

  void clear_caches();

  MEM_CXX_CLASS_ALLOC_FUNCS("LibOCIOColorSpace");

 private:
  void ensure_srgb_scene_linear_info() const;
};

}  // namespace blender::ocio

#endif
