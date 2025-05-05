/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <mutex>
#include <optional>

#include "BLI_fileops.hh"
#include "BLI_hash.hh"
#include "BLI_map.hh"
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

static std::optional<int64_t> get_file_modification_time(const StringRefNull path)
{
  BLI_stat_t stat;
  if (BLI_stat(path.c_str(), &stat) == -1) {
    return std::nullopt;
  }
  return stat.st_mtim.tv_sec;
}

struct FileModificationTimesMap {
  std::mutex mutex;
  Map<std::string, std::optional<int64_t>> map;
};

static FileModificationTimesMap &get_file_modification_times_map()
{
  static FileModificationTimesMap file_modification_times_map;
  return file_modification_times_map;
}

std::shared_ptr<CachedValue> get_loaded_base(const GenericKey &loader_key,
                                             Span<StringRefNull> file_paths,
                                             FunctionRef<std::unique_ptr<CachedValue>()> load_fn)
{
  const LoadFileKey key{file_paths, loader_key.to_storable()};

  FileModificationTimesMap &file_modification_times_map = get_file_modification_times_map();
  bool found_outdated = false;
  {
    std::lock_guard lock{file_modification_times_map.mutex};
    for (const StringRefNull path : file_paths) {
      const std::optional<int64_t> new_time = get_file_modification_time(path);
      const std::optional<int64_t> old_time = file_modification_times_map.map.lookup_or_add_as(
          path, new_time);
      if (old_time != new_time) {
        found_outdated = true;
        file_modification_times_map.map.add_overwrite_as(path, new_time);
      }
    }
  }
  if (found_outdated) {
    memory_cache::invalidate(key);
  }
  return memory_cache::get_base(key, load_fn);
}

}  // namespace blender::memory_cache
