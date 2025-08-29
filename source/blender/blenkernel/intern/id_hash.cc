/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>
#include <mutex>
#include <xxhash.h>

#include "BKE_id_hash.hh"
#include "BKE_lib_query.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"

#include "BLI_fileops.hh"
#include "BLI_mutex.hh"
#include "BLI_set.hh"

namespace blender::bke::id_hash {

static std::optional<Vector<char>> read_file(const StringRefNull path)
{
  blender::fstream stream{path.c_str(), std::ios_base::in | std::ios_base::binary};
  stream.seekg(0, std::ios_base::end);
  const int64_t size = stream.tellg();
  stream.seekg(0, std::ios_base::beg);

  blender::Vector<char> buffer(size);
  stream.read(buffer.data(), size);
  if (stream.bad()) {
    return std::nullopt;
  }

  return buffer;
}

struct CachedFileHash {
  int64_t last_modified = 0;
  XXH128_hash_t hash;
};

static std::optional<XXH128_hash_t> get_file_hash(const StringRefNull path)
{
  static Map<std::string, CachedFileHash> cache;
  static Mutex mutex;

  BLI_stat_t stat;
  if (BLI_stat(path.c_str(), &stat) == -1) {
    return std::nullopt;
  }

  std::lock_guard lock(mutex);
  if (const CachedFileHash *cached_hash = cache.lookup_ptr_as(path)) {
    if (cached_hash->last_modified == stat.st_mtime) {
      return cached_hash->hash;
    }
  }
  const std::optional<Vector<char>> buffer = read_file(path);
  if (!buffer) {
    return std::nullopt;
  }
  const XXH128_hash_t hash = XXH3_128bits(buffer->data(), buffer->size());
  cache.add(path, CachedFileHash{stat.st_mtime, hash});
  return hash;
}

static std::optional<XXH128_hash_t> get_id_shallow_hash(const ID &id,
                                                        Set<std::string> &r_missing_files)
{
  BLI_assert(ID_IS_LINKED(&id));
  const StringRefNull id_name = id.name;
  const StringRefNull path = id.lib->runtime->filepath_abs;
  const std::optional<XXH128_hash_t> file_hash = get_file_hash(path);
  if (!file_hash) {
    r_missing_files.add_as(path);
    return std::nullopt;
  }

  XXH3_state_t *hash_state = XXH3_createState();
  XXH3_128bits_reset(hash_state);
  XXH3_128bits_update(hash_state, id_name.data(), id_name.size());
  XXH3_128bits_update(hash_state, &*file_hash, sizeof(XXH128_hash_t));
  XXH128_hash_t shallow_hash = XXH3_128bits_digest(hash_state);
  XXH3_freeState(hash_state);
  return shallow_hash;
}

static void compute_deep_hash_recursive(const Main &bmain,
                                        const ID &id,
                                        Set<const ID *> &current_stack,
                                        Map<const ID *, IDHash> &r_hashes,
                                        Set<std::string> &r_missing_files)
{
  if (r_hashes.contains(&id)) {
    return;
  }
  if (!id.deep_hash.is_null()) {
    r_hashes.add(&id, id.deep_hash);
    return;
  }
  current_stack.add(&id);
  const std::optional<XXH128_hash_t> id_shallow_hash = get_id_shallow_hash(id, r_missing_files);
  if (!id_shallow_hash) {
    return;
  }

  XXH3_state_t *hash_state = XXH3_createState();
  XXH3_128bits_reset(hash_state);
  XXH3_128bits_update(hash_state, &*id_shallow_hash, sizeof(XXH128_hash_t));

  bool success = true;
  BKE_library_foreach_ID_link(
      const_cast<Main *>(&bmain),
      const_cast<ID *>(&id),
      [&](LibraryIDLinkCallbackData *cb_data) {
        if (cb_data->cb_flag & IDWALK_CB_LOOPBACK) {
          /* Loopback pointer (e.g. from a shapekey to its owner geometry ID, or from a collection
           * to its parents) should always be ignored, as they do not represent an actual
           * dependency. The dependency relationship should already have been processed from the
           * owner to its dependency anyway (if applicable). */
          return IDWALK_RET_NOP;
        }
        if (cb_data->cb_flag & (IDWALK_CB_EMBEDDED | IDWALK_CB_EMBEDDED_NOT_OWNING)) {
          /* Embedded data are part of their owner's internal data, and as such already computed as
           * part of the owner's shallow hash. */
          return IDWALK_RET_NOP;
        }
        ID *referenced_id = *cb_data->id_pointer;
        if (!referenced_id) {
          /* Need to update the hash even if there is no id. There is a difference between the case
           * where there is no id and the case where this callback is not called at all.*/
          const int random_data = 452942579;
          XXH3_128bits_update(hash_state, &random_data, sizeof(int));
          return IDWALK_RET_NOP;
        }
        /* All embedded ID usages should already have been excluded above. */
        BLI_assert((referenced_id->flag & ID_FLAG_EMBEDDED_DATA) == 0);
        if (current_stack.contains(referenced_id)) {
          /* Somehow encode that we had a circular reference here. */
          const int random_data = 234632342;
          XXH3_128bits_update(hash_state, &random_data, sizeof(int));
          return IDWALK_RET_NOP;
        }
        compute_deep_hash_recursive(
            bmain, *referenced_id, current_stack, r_hashes, r_missing_files);
        const IDHash *referenced_id_hash = r_hashes.lookup_ptr(referenced_id);
        if (!referenced_id_hash) {
          success = false;
          return IDWALK_RET_STOP_ITER;
        }
        XXH3_128bits_update(hash_state, referenced_id_hash->data, sizeof(IDHash));
        return IDWALK_RET_NOP;
      },
      nullptr,
      IDWALK_READONLY);

  if (!success) {
    return;
  }
  IDHash new_deep_hash;
  const XXH128_hash_t new_deep_hash_xxh128 = XXH3_128bits_digest(hash_state);
  XXH3_freeState(hash_state);
  static_assert(sizeof(IDHash) == sizeof(XXH128_hash_t));
  memcpy(new_deep_hash.data, &new_deep_hash_xxh128, sizeof(IDHash));
  r_hashes.add(&id, new_deep_hash);
}

IDHashResult compute_linked_id_deep_hashes(const Main &bmain, Span<const ID *> ids)
{
#ifndef NDEBUG
  for (const ID *id : ids) {
    BLI_assert(ID_IS_LINKED(id));
  }
#endif

  if (ids.is_empty()) {
    return ValidDeepHashes{};
  }

  Map<const ID *, IDHash> hashes;
  Set<const ID *> current_stack;
  Set<std::string> missing_files;
  for (const ID *id : ids) {
    compute_deep_hash_recursive(bmain, *id, current_stack, hashes, missing_files);
  }
  if (!missing_files.is_empty()) {
    Vector<std::string> missing_files_vec;
    missing_files_vec.extend(missing_files.begin(), missing_files.end());
    return MissingBlendFiles{missing_files_vec};
  }
  return ValidDeepHashes{hashes};
}

std::string id_hash_to_hex(const IDHash &hash)
{
  std::string hex_str;
  for (const uint8_t byte : hash.data) {
    hex_str += fmt::format("{:02x}", byte);
  }
  return hex_str;
}

}  // namespace blender::bke::id_hash
