/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"

namespace blender::io::midi {

struct NoteSample {
  bool is_playing = false;
  float velocity = 0.0f;
  float last_hit_velocity = 0.0f;
  float time_since_last_hit = 0.0f;
  float time_since_note_start = 0.0f;
};

class MidiFile {
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit MidiFile(std::unique_ptr<Impl> impl);

 public:
  ~MidiFile();

  MidiFile(MidiFile &&);
  MidiFile &operator=(MidiFile &&);

  MidiFile(const MidiFile &) = delete;
  MidiFile &operator=(const MidiFile &) = delete;

  static std::unique_ptr<MidiFile> parse(Span<uint8_t> data, std::string &r_error);
  static std::unique_ptr<MidiFile> load(StringRefNull path, std::string &r_error);

  NoteSample sample(double time, int channel, int note) const;
  int64_t memory_usage() const;
};

}  // namespace blender::io::midi
