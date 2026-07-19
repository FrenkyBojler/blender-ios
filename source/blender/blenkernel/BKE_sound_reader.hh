/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#if defined(WITH_AUDASPACE)

#  include <memory>
#  include <optional>

namespace aud {
class IReader;
}  // namespace aud

namespace blender {
struct bSound;
}

namespace blender::bke {

class SoundReaderCache;

/**
 * Exclusive access to a sound reader. Readers are discarded unless #mark_reusable is called.
 */
class SoundReaderLease {
 private:
  std::shared_ptr<SoundReaderCache> owner_;
  std::shared_ptr<aud::IReader> reader_;
  int slot_ = -1;
  bool is_reusable_ = false;

  SoundReaderLease(std::shared_ptr<SoundReaderCache> owner,
                   std::shared_ptr<aud::IReader> reader,
                   int slot);
  void release() noexcept;

  friend SoundReaderCache;

 public:
  SoundReaderLease() = default;
  ~SoundReaderLease();

  SoundReaderLease(const SoundReaderLease &) = delete;
  SoundReaderLease &operator=(const SoundReaderLease &) = delete;
  SoundReaderLease(SoundReaderLease &&other) noexcept;
  SoundReaderLease &operator=(SoundReaderLease &&other) noexcept;

  explicit operator bool() const;
  aud::IReader *operator->() const;

  /** Keep a retained reader for reuse when this lease is destroyed. */
  void mark_reusable();
};

/** Acquire exclusive access to a reusable reader owned by the sound's runtime. */
std::optional<SoundReaderLease> sound_reader_acquire(const bSound &sound);

}  // namespace blender::bke

#endif
