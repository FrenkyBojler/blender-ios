/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_hash.hh"
#include "BLI_memory_cache_file_load.hh"
#include "BLI_vector.hh"

namespace blender::memory_cache {

class LoadFileKey : public GenericKey {
 private:
  Vector<std::string> file_paths_;
  std::unique_ptr<GenericKey> loader_key_;

 public:
  LoadFileKey(Vector<std::string> file_paths, std::unique_ptr<GenericKey> loader_key)
      : file_paths_(std::move(file_paths)), loader_key_(std::move(loader_key))
  {
  }

  LoadFileKey(const LoadFileKey &other)
      : file_paths_(other.file_paths_), loader_key_(other.loader_key_->to_storable())
  {
  }

  uint64_t hash() const override
  {
    return get_default_hash(this->file_paths_, *this->loader_key_);
  }

  friend bool operator==(const LoadFileKey &a, const LoadFileKey &b)
  {
    return a.file_paths_ == b.file_paths_ && *a.loader_key_ == *b.loader_key_;
  }

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
  const LoadFileKey key{file_paths, loader_key.to_storable()};
  return memory_cache::get_base(key, load_fn);
}

}  // namespace blender::memory_cache
