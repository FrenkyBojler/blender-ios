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

/**
 * A key used to identify data loaded from one or more files.
 */
class LoadFileKey : public GenericKey {
 private:
  /** The files to load from. */
  Vector<std::string> file_paths_;
  /**
   * The key used to identify the loader. The same files might be loaded with different loaders
   * which can result in different data that needs to be cached separately.
   */
  std::shared_ptr<const GenericKey> loader_key_;

 public:
  LoadFileKey(Vector<std::string> file_paths, std::shared_ptr<const GenericKey> loader_key)
      : file_paths_(std::move(file_paths)), loader_key_(std::move(loader_key))
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
    /* Currently, #LoadFileKey is always storable, i.e. it owns all the data it references. A
     * potential future optimization could be to support just referencing the paths and loader key,
     * but that causes some boilerplate now that is not worth it. */
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

struct FileStatMap {
  std::mutex mutex;
  Map<std::string, std::optional<int64_t>> map;
};

static FileStatMap &get_file_stat_map()
{
  static FileStatMap file_stat_map;
  return file_stat_map;
}

std::shared_ptr<CachedValue> get_loaded_base(const GenericKey &loader_key,
                                             Span<StringRefNull> file_paths,
                                             FunctionRef<std::unique_ptr<CachedValue>()> load_fn)
{
  FileStatMap &file_stat_map = get_file_stat_map();

  bool found_outdated = false;
  {
    // TODO: Properly handle case when there are multiple loaders for the same file. Currently,
    // some of these loaders may miss a file modification.
    Vector<std::optional<int64_t>> new_times;
    for (const StringRefNull path : file_paths) {
      new_times.append(get_file_modification_time(path));
    }
    std::lock_guard lock{file_stat_map.mutex};
    for (const int i : file_paths.index_range()) {
      const StringRefNull path = file_paths[i];
      const std::optional<int64_t> new_time = new_times[i];
      const std::optional<int64_t> old_time = file_stat_map.map.lookup_or_add_as(path, new_time);
      if (old_time != new_time) {
        found_outdated = true;
        file_stat_map.map.add_overwrite_as(path, new_time);
      }
    }
  }

  const LoadFileKey key{file_paths, loader_key.to_storable()};
  if (found_outdated) {
    memory_cache::invalidate(key);
  }
  return memory_cache::get_base(key, load_fn);
}

}  // namespace blender::memory_cache
