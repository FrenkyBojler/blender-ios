/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_memory_cache_file_load.hh"
#include "BLI_struct_equality_utils.hh"
#include "BLI_vector.hh"

namespace blender::memory_cache {

class LoadFileKey : public GenericKey {
 public:
  Vector<std::string> file_paths;

  uint64_t hash() const override
  {
    return get_default_hash(this->file_paths);
  }

  BLI_STRUCT_EQUALITY_OPERATORS_1(LoadFileKey, file_paths)

  bool equal_to(const GenericKey &other) const override
  {
    if (const auto *other_typed = dynamic_cast<const LoadFileKey *>(&other)) {
      return *this == *other_typed;
    }
    return false;
  }

  std::unique_ptr<GenericKey> to_storable() const override
  {
    return std::make_unique<LoadFileKey>(*this);
  }
};

std::shared_ptr<CachedValue> get_loaded_base(const GenericKey &loader_key,
                                             Span<StringRef> file_paths,
                                             FunctionRef<std::unique_ptr<CachedValue>()> load_fn)
{
  return {};
}

}  // namespace blender::memory_cache
