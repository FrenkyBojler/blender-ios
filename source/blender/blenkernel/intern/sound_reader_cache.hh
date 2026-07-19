/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#if defined(WITH_AUDASPACE)

#  include <memory>
#  include <optional>

#  include "BKE_sound_reader.hh"

namespace aud {
class ISound;
}

namespace blender::bke {

class bSoundFrequencySampler;
struct SoundReaderCacheTestAccess;

/** Runtime-only reusable reader state owned by a #bSound runtime. */
class SoundReaderCache : public std::enable_shared_from_this<SoundReaderCache> {
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit SoundReaderCache(std::shared_ptr<aud::ISound> sound);
  std::optional<SoundReaderLease> acquire();
  void release(int slot, std::shared_ptr<aud::IReader> reader, bool is_reusable) noexcept;

  friend bSoundFrequencySampler;
  friend SoundReaderLease;
  friend SoundReaderCacheTestAccess;
  friend std::optional<SoundReaderLease> sound_reader_acquire(const bSound &sound);

 public:
  static std::shared_ptr<SoundReaderCache> create(std::shared_ptr<aud::ISound> sound);
  ~SoundReaderCache();

  SoundReaderCache(const SoundReaderCache &) = delete;
  SoundReaderCache &operator=(const SoundReaderCache &) = delete;
};

/** Internal access for focused reader-cache tests. */
struct SoundReaderCacheTestAccess {
  static std::shared_ptr<SoundReaderCache> create(std::shared_ptr<aud::ISound> sound);
  static std::optional<SoundReaderLease> acquire(const std::shared_ptr<SoundReaderCache> &cache);
  static void initialize_sound(bSound &sound, std::shared_ptr<aud::ISound> reader_sound);
  static void clear_sound(bSound &sound);
};

}  // namespace blender::bke

#endif
