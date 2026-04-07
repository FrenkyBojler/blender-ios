/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "BLI_map.hh"

#include "DNA_vfont_types.h"

#include "COM_cached_resource.hh"
#include "COM_result.hh"

namespace blender::compositor {

class Context;

/* ------------------------------------------------------------------------------------------------
 * String Image Key.
 */
class StringImageKey {
 public:
  const std::string string;
  const VFont *font;
  const float size;

  StringImageKey(const std::string string, const VFont *font, const float size);

  uint64_t hash() const;
  friend bool operator==(const StringImageKey &a, const StringImageKey &b) = default;
};

/* -------------------------------------------------------------------------------------------------
 * String Image.
 *
 * A cached resource that computes and caches a result containing a string. */
class StringImage : public CachedResource {
 public:
  Result result;

  StringImage(Context &context, const std::string string, const VFont *font, const float size);

  ~StringImage();
};

/* ------------------------------------------------------------------------------------------------
 * String Image Container.
 */
class StringImageContainer : CachedResourceContainer {
 private:
  Map<StringImageKey, std::unique_ptr<StringImage>> map_;

 public:
  void reset() override;

  /* Check if there is an available StringImage cached resource with the given parameters in the
   * container, if one exists, return it, otherwise, return a newly created one and add it to the
   * container. In both cases, tag the cached resource as needed to keep it cached for the next
   * evaluation. */
  Result &get(Context &context, const std::string string, const VFont *font, const float size);
};

}  // namespace blender::compositor
