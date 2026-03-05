/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_OPENVDB

#  include "DNA_packedFile_types.h"

#  include "BKE_volume_grid_file_cache.hh"
#  include "BKE_volume_openvdb.hh"

#  include "BLI_map.hh"
#  include "BLI_memory_cache.hh"
#  include "BLI_memory_counter.hh"

#  include <openvdb/io/Stream.h>
#  include <openvdb/openvdb.h>

namespace blender::bke::volume_grid::file_cache {

class PackedFileStreamBuf final : public std::streambuf {
  const uint8_t *begin_ = nullptr;
  const uint8_t *cursor_ = nullptr;
  const uint8_t *end_ = nullptr;

 public:
  explicit PackedFileStreamBuf(const PackedFile *packed_file)
      : begin_(static_cast<const uint8_t *>(packed_file->data)),
        cursor_(begin_),
        end_(begin_ + packed_file->size)
  {
  }

 protected:
  int_type underflow() override
  {
    if (cursor_ == end_) {
      return traits_type::eof();
    }
    return traits_type::to_int_type(*cursor_);
  }

  int_type uflow() override
  {
    if (cursor_ == end_) {
      return traits_type::eof();
    }
    return traits_type::to_int_type(*cursor_++);
  }

  std::streamsize xsgetn(char *out_ptr, std::streamsize count) override
  {
    count = std::min(count, std::streamsize(end_ - cursor_));
    memcpy(out_ptr, cursor_, size_t(count));
    cursor_ += count;
    return count;
  }

  int_type pbackfail(int_type ch) override
  {
    if (cursor_ == begin_ || (ch != traits_type::eof() && ch != cursor_[-1])) {
      return traits_type::eof();
    }
    return traits_type::to_int_type(*--cursor_);
  }

  std::streamsize showmanyc() override
  {
    return end_ - cursor_;
  }

  std::streampos seekoff(std::streamoff off,
                         std::ios_base::seekdir way,
                         std::ios_base::openmode /*which*/) override
  {
    if (way == std::ios_base::beg) {
      cursor_ = begin_ + off;
    }
    else if (way == std::ios_base::cur) {
      cursor_ += off;
    }
    else if (way == std::ios_base::end) {
      cursor_ = end_;
    }

    if (cursor_ < begin_ || cursor_ > end_) {
      return -1;
    }

    return cursor_ - begin_;
  }

  std::streampos seekpos(std::streampos sp, std::ios_base::openmode /*which*/) override
  {
    cursor_ = begin_ + int(sp);

    if (cursor_ < begin_ || cursor_ > end_) {
      return -1;
    }

    return cursor_ - begin_;
  }
};

/**
 * Cache for a single grid stored in a file.
 */
struct GridCache {
  /**
   * Grid returned by #readAllGridMetadata. This only contains the meta-data and transform of
   * the grid, but not the tree.
   */
  openvdb::GridBase::Ptr meta_data_grid;
  /**
   * Cached simplify levels.
   */
  Map<int, GVolumeGrid> grid_by_simplify_level;
};

/**
 * Cache for a file that contains potentially multiple grids.
 */
struct FileCache {
  /**
   * Empty on success, otherwise an error message that was generated when trying to load the file.
   */
  std::string error_message;
  /**
   * Meta data of the file (not of an individual grid).
   */
  openvdb::MetaMap meta_data;
  /**
   * Caches for grids in the same order they are stored in the file.
   */
  Vector<GridCache> grids;

  GridCache *grid_cache_by_name(const StringRef name)
  {
    for (GridCache &grid_cache : this->grids) {
      if (grid_cache.meta_data_grid->getName() == name) {
        return &grid_cache;
      }
    }
    return nullptr;
  }
};

/**
 * Singleton cache that's shared throughout the application.
 */
struct GlobalCache {
  Mutex mutex;
  Map<std::string, FileCache> file_map;
};

/**
 * Uses the "construct on first use" idiom to get the cache.
 */
static GlobalCache &get_global_cache()
{
  static GlobalCache global_cache;
  return global_cache;
}

/**
 * Tries to load the file at the given path and creates a cache for it. This only reads meta-data,
 * but not the actual trees, which will be loaded on-demand.
 */
static FileCache create_file_cache(const PackedFile *packed_file, const StringRef file_path)
{
  FileCache file_cache;

  openvdb::io::File file(file_path);
  openvdb::GridPtrVec vdb_grids;
  try {
    /* Disable delay loading and file copying, this has poor performance
     * on network drives. */
    const bool delay_load = false;

    if (packed_file) {
      PackedFileStreamBuf packed_file_stream_buf(packed_file);
      std::istream packed_file_stream(&packed_file_stream_buf);

      openvdb::io::Stream stream(packed_file_stream, delay_load);

      openvdb::GridPtrVecPtr grids = stream.getGrids();

      file_cache.meta_data = *stream.getMetadata();

      for (openvdb::GridBase::Ptr &vdb_grid : *grids) {
        if (!vdb_grid) {
          continue;
        }
        GridCache grid_cache;
        grid_cache.meta_data_grid = vdb_grid;
        file_cache.grids.append(std::move(grid_cache));
      }

      return file_cache;
    }
#  ifdef OPENVDB_USE_DELAYED_LOADING
    file.setCopyMaxBytes(0);
#  endif
    file.open(delay_load);
    vdb_grids = *(file.readAllGridMetadata());
    file_cache.meta_data = *file.getMetadata();
  }
  catch (const openvdb::IoError &e) {
    file_cache.error_message = e.what();
  }
  catch (...) {
    file_cache.error_message = "Unknown error reading VDB file";
  }
  if (!file_cache.error_message.empty()) {
    return file_cache;
  }

  for (openvdb::GridBase::Ptr &vdb_grid : vdb_grids) {
    if (!vdb_grid) {
      continue;
    }
    GridCache grid_cache;
    grid_cache.meta_data_grid = vdb_grid;
    file_cache.grids.append(std::move(grid_cache));
  }

  return file_cache;
}

static FileCache &get_file_cache(const PackedFile *packed_file, const StringRef file_path)
{
  GlobalCache &global_cache = get_global_cache();
  /* Assumes that the cache is locked already. */
  BLI_assert(!global_cache.mutex.try_lock());
  return global_cache.file_map.lookup_or_add_cb_as(
      file_path, [&]() { return create_file_cache(packed_file, file_path); });
}

/**
 * Identifies a grid in the global memory cache.
 */
class GridReadKey : public GenericKey {
 public:
  const PackedFile *packed_file;
  std::string file_path;
  std::string grid_name;
  int simplify_level;

  uint64_t hash() const override
  {
    return get_default_hash(this->file_path, this->grid_name, this->simplify_level);
  }

  bool equal_to(const GenericKey &other) const override
  {
    if (const auto *other_typed = dynamic_cast<const GridReadKey *>(&other)) {
      const GridReadKey &a = *this;
      const GridReadKey &b = *other_typed;
      return a.file_path == b.file_path && a.grid_name == b.grid_name &&
             a.simplify_level == b.simplify_level;
    }
    return false;
  }

  std::unique_ptr<GenericKey> to_storable() const override
  {
    return std::make_unique<GridReadKey>(*this);
  }
};

class GridReadValue : public memory_cache::CachedValue {
 private:
  mutable std::atomic<int64_t> bytes_ = 0;

 public:
  ImplicitSharingPtr<> tree_sharing_info;
  openvdb::GridBase::Ptr grid;

  void count_memory(MemoryCounter &memory) const override
  {
    /* Avoid computing the amount of memory from scratch every time. */
    if (bytes_ == 0) {
      this->bytes_ = grid->baseTree().memUsage();
    }
    memory.add(bytes_);
  }
};

/**
 * Load a single grid by name from a file. This loads the full grid including meta-data, transforms
 * and the tree.
 */
static openvdb::GridBase::Ptr load_single_grid_from_disk(const PackedFile *packed_file,
                                                         const StringRef file_path,
                                                         const StringRef grid_name)
{
  /* Disable delay loading and file copying, this has poor performance
   * on network drivers. */
  const bool delay_load = false;

  if (packed_file) {
    PackedFileStreamBuf packed_file_stream_buf(packed_file);
    std::istream packed_file_stream(&packed_file_stream_buf);

    openvdb::io::Stream stream(packed_file_stream, delay_load);

    openvdb::GridPtrVecPtr grids = stream.getGrids();

    for (size_t i = 0; i < grids->size(); i++) {
      openvdb::GridBase::Ptr grid = (*grids)[i];
      if (grid->getName() == grid_name) {
        return grid;
      }
    }

    return {};
  }

  openvdb::io::File file(file_path);
#  ifdef OPENVDB_USE_DELAYED_LOADING
  file.setCopyMaxBytes(0);
#  endif
  file.open(delay_load);
  return file.readGrid(grid_name);
}

/**
 * Load a single grid by name from a file. This loads the full grid including meta-data, transforms
 * and the tree.
 */
static LazyLoadedGrid load_single_grid_from_disk_cached(const PackedFile *packed_file,
                                                        const StringRef file_path,
                                                        const StringRef grid_name,
                                                        const int simplify_level)
{
  GridReadKey key;
  key.packed_file = packed_file;
  key.file_path = file_path;
  key.grid_name = grid_name;
  key.simplify_level = simplify_level;

  std::shared_ptr<const GridReadValue> value = memory_cache::get<GridReadValue>(key, [&key]() {
    openvdb::GridBase::Ptr grid;
    if (key.simplify_level == 0) {
      grid = load_single_grid_from_disk(key.packed_file, key.file_path, key.grid_name);
    }
    else {
      /* Build the simplified grid from the main grid. */
      const GVolumeGrid main_grid = get_grid_from_file(
          key.packed_file, key.file_path, key.grid_name, 0);
      const VolumeGridType grid_type = main_grid->grid_type();
      const float resolution_factor = 1.0f / (1 << key.simplify_level);
      VolumeTreeAccessToken tree_token;
      grid = BKE_volume_grid_create_with_changed_resolution(
          grid_type, main_grid->grid(tree_token), resolution_factor);
    }
    auto value = std::make_unique<GridReadValue>();
    value->grid = std::move(grid);
    value->tree_sharing_info = OpenvdbTreeSharingInfo::make(value->grid->baseTreePtr());
    return value;
  });
  if (!value) {
    return {};
  }

  /* Copy the grid so that it has a single owner. Note that the tree is still shared. */
  openvdb::GridBase::Ptr grid = value->grid->copyGrid();
  grid->setTransform(grid->transform().copy());
  return {grid, value->tree_sharing_info};
}

/**
 * Checks if there is already a cached grid for the parameters and creates it otherwise. This does
 * not load the tree, because that is done on-demand.
 */
static GVolumeGrid get_cached_grid(const PackedFile *packed_file,
                                   const StringRef file_path,
                                   GridCache &grid_cache,
                                   const int simplify_level)
{
  if (GVolumeGrid *grid = grid_cache.grid_by_simplify_level.lookup_ptr(simplify_level)) {
    return *grid;
  }
  /* A callback that actually loads the full grid including the tree when it's accessed. */
  auto load_grid_fn = [file_path = std::string(file_path),
                       grid_name = std::string(grid_cache.meta_data_grid->getName()),
                       simplify_level,
                       packed_file]() -> LazyLoadedGrid {
    return load_single_grid_from_disk_cached(packed_file, file_path, grid_name, simplify_level);
  };
  /* This allows the returned grid to already contain meta-data and transforms, even if the tree is
   * not loaded yet. */
  openvdb::GridBase::Ptr meta_data_and_transform_grid;
  if (simplify_level == 0) {
    /* Only pass the meta-data grid when there is no simplification for now. For simplified grids,
     * the transform would have to be updated here already. */
    meta_data_and_transform_grid = grid_cache.meta_data_grid->copyGrid();
  }
  VolumeGridData *grid_data = MEM_new<VolumeGridData>(
      __func__, load_grid_fn, meta_data_and_transform_grid);
  GVolumeGrid grid{grid_data};
  grid_cache.grid_by_simplify_level.add(simplify_level, grid);
  return grid;
}

GVolumeGrid get_grid_from_file(const PackedFile *packed_file,
                               const StringRef file_path,
                               const StringRef grid_name,
                               const int simplify_level)
{
  GlobalCache &global_cache = get_global_cache();
  std::lock_guard lock{global_cache.mutex};
  FileCache &file_cache = get_file_cache(packed_file, file_path);
  if (GridCache *grid_cache = file_cache.grid_cache_by_name(grid_name)) {
    return get_cached_grid(packed_file, file_path, *grid_cache, simplify_level);
  }
  return {};
}

GridsFromFile get_all_grids_from_file(const PackedFile *packed_file,
                                      const StringRef file_path,
                                      const int simplify_level)
{
  GridsFromFile result;
  GlobalCache &global_cache = get_global_cache();
  std::lock_guard lock{global_cache.mutex};
  FileCache &file_cache = get_file_cache(packed_file, file_path);

  if (!file_cache.error_message.empty()) {
    result.error_message = file_cache.error_message;
    return result;
  }
  result.file_meta_data = std::make_shared<openvdb::MetaMap>(file_cache.meta_data);
  for (GridCache &grid_cache : file_cache.grids) {
    result.grids.append(get_cached_grid(packed_file, file_path, grid_cache, simplify_level));
  }
  return result;
}

void unload_unused()
{
  GlobalCache &global_cache = get_global_cache();
  std::lock_guard lock{global_cache.mutex};
  for (FileCache &file_cache : global_cache.file_map.values()) {
    for (GridCache &grid_cache : file_cache.grids) {
      grid_cache.grid_by_simplify_level.remove_if(
          [&](const auto &item) { return item.value->is_mutable(); });
    }
  }
}

}  // namespace blender::bke::volume_grid::file_cache

#endif /* WITH_OPENVDB */
