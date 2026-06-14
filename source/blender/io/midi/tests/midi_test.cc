/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "IO_midi.hh"

#include "BLI_vector.hh"

#include "testing/testing.h"

namespace blender::io::midi::tests {

static void append_u16(Vector<uint8_t> &data, const uint16_t value)
{
  data.append(uint8_t(value >> 8));
  data.append(uint8_t(value));
}

static void append_u32(Vector<uint8_t> &data, const uint32_t value)
{
  data.append(uint8_t(value >> 24));
  data.append(uint8_t(value >> 16));
  data.append(uint8_t(value >> 8));
  data.append(uint8_t(value));
}

static void append_chunk(Vector<uint8_t> &data, const char *id, const Span<uint8_t> contents)
{
  for (int i = 0; i < 4; i++) {
    data.append(id[i]);
  }
  append_u32(data, contents.size());
  data.extend(contents);
}

static Vector<uint8_t> midi_file(const uint16_t format,
                                 const uint16_t time_division,
                                 const Span<Vector<uint8_t>> tracks)
{
  Vector<uint8_t> data;
  Vector<uint8_t> header;
  append_u16(header, format);
  append_u16(header, tracks.size());
  append_u16(header, time_division);
  append_chunk(data, "MThd", header);
  for (const Vector<uint8_t> &track : tracks) {
    append_chunk(data, "MTrk", track);
  }
  return data;
}

TEST(midi, NoteStateAndRunningStatus)
{
  const Vector<uint8_t> track = {0x00,
                                 0x90,
                                 0x3c,
                                 0x40,
                                 0x00,
                                 0xff,
                                 0x01,
                                 0x00,
                                 0x81,
                                 0x70,
                                 0x3c,
                                 0x00,
                                 0x00,
                                 0xff,
                                 0x2f,
                                 0x00};
  const Vector<Vector<uint8_t>> tracks = {track};
  const Vector<uint8_t> data = midi_file(0, 480, tracks);

  std::string error;
  const std::unique_ptr<MidiFile> midi = MidiFile::parse(data, error);
  ASSERT_NE(midi, nullptr) << error;

  NoteSample sample = midi->sample(0.125, 0, 60);
  EXPECT_TRUE(sample.is_playing);
  EXPECT_NEAR(sample.velocity, 64.0f / 127.0f, 1e-6f);
  EXPECT_NEAR(sample.time_since_note_start, 0.125f, 1e-6f);

  sample = midi->sample(0.25, 0, 60);
  EXPECT_FALSE(sample.is_playing);
  EXPECT_FLOAT_EQ(sample.velocity, 0.0f);
  EXPECT_NEAR(sample.last_hit_velocity, 64.0f / 127.0f, 1e-6f);
  EXPECT_NEAR(sample.time_since_last_hit, 0.25f, 1e-6f);
}

TEST(midi, FormatOneTempoTrack)
{
  const Vector<uint8_t> tempo_track = {
      0x83, 0x60, 0xff, 0x51, 0x03, 0x0f, 0x42, 0x40, 0x00, 0xff, 0x2f, 0x00};
  const Vector<uint8_t> note_track = {
      0x87, 0x40, 0x92, 0x40, 0x7f, 0x83, 0x60, 0x82, 0x40, 0x00, 0x00, 0xff, 0x2f, 0x00};
  const Vector<Vector<uint8_t>> tracks = {tempo_track, note_track};
  const Vector<uint8_t> data = midi_file(1, 480, tracks);

  std::string error;
  const std::unique_ptr<MidiFile> midi = MidiFile::parse(data, error);
  ASSERT_NE(midi, nullptr) << error;

  NoteSample sample = midi->sample(1.75, 2, 64);
  EXPECT_TRUE(sample.is_playing);
  EXPECT_NEAR(sample.time_since_note_start, 0.25f, 1e-6f);

  sample = midi->sample(2.5, 2, 64);
  EXPECT_FALSE(sample.is_playing);
  EXPECT_NEAR(sample.time_since_last_hit, 1.0f, 1e-6f);
}

TEST(midi, RetriggerKeepsNewestActiveNote)
{
  const Vector<uint8_t> track = {0x00, 0x90, 0x3c, 0x20, 0x60, 0x90, 0x3c, 0x60, 0x60, 0x80,
                                 0x3c, 0x00, 0x60, 0x80, 0x3c, 0x00, 0x00, 0xff, 0x2f, 0x00};
  const Vector<Vector<uint8_t>> tracks = {track};
  const Vector<uint8_t> data = midi_file(0, 480, tracks);

  std::string error;
  const std::unique_ptr<MidiFile> midi = MidiFile::parse(data, error);
  ASSERT_NE(midi, nullptr) << error;

  NoteSample sample = midi->sample(0.15, 0, 60);
  EXPECT_TRUE(sample.is_playing);
  EXPECT_NEAR(sample.velocity, 96.0f / 127.0f, 1e-6f);
  EXPECT_NEAR(sample.time_since_note_start, 0.05f, 1e-6f);

  sample = midi->sample(0.25, 0, 60);
  EXPECT_TRUE(sample.is_playing);
  EXPECT_NEAR(sample.velocity, 96.0f / 127.0f, 1e-6f);
  EXPECT_NEAR(sample.time_since_note_start, 0.15f, 1e-6f);

  sample = midi->sample(0.31, 0, 60);
  EXPECT_FALSE(sample.is_playing);
  EXPECT_NEAR(sample.last_hit_velocity, 96.0f / 127.0f, 1e-6f);
}

TEST(midi, SkipsSystemExclusiveEvents)
{
  const Vector<uint8_t> track = {0x00,
                                 0x90,
                                 0x3c,
                                 0x40,
                                 0x00,
                                 0xf0,
                                 0x03,
                                 0x01,
                                 0x02,
                                 0xf7,
                                 0x81,
                                 0x70,
                                 0x3c,
                                 0x00,
                                 0x00,
                                 0xff,
                                 0x2f,
                                 0x00};
  const Vector<Vector<uint8_t>> tracks = {track};
  const Vector<uint8_t> data = midi_file(0, 480, tracks);

  std::string error;
  const std::unique_ptr<MidiFile> midi = MidiFile::parse(data, error);
  ASSERT_NE(midi, nullptr) << error;
  EXPECT_TRUE(midi->sample(0.125, 0, 60).is_playing);
  EXPECT_FALSE(midi->sample(0.25, 0, 60).is_playing);
}

TEST(midi, RejectsMalformedAndUnsupportedFiles)
{
  std::string error;
  EXPECT_EQ(MidiFile::parse({}, error), nullptr);
  EXPECT_FALSE(error.empty());

  const Vector<uint8_t> invalid_running_status_track = {0x00, 0x3c, 0x40, 0x00, 0xff, 0x2f, 0x00};
  const Vector<Vector<uint8_t>> invalid_running_status_tracks = {invalid_running_status_track};
  const Vector<uint8_t> invalid_running_status_data = midi_file(
      0, 480, invalid_running_status_tracks);
  EXPECT_EQ(MidiFile::parse(invalid_running_status_data, error), nullptr);
  EXPECT_EQ(error, "MIDI running status has no preceding channel event");

  const Vector<uint8_t> end_track = {0x00, 0xff, 0x2f, 0x00};
  const Vector<Vector<uint8_t>> end_tracks = {end_track};
  const Vector<uint8_t> smpte_data = midi_file(0, 0xe728, end_tracks);
  EXPECT_EQ(MidiFile::parse(smpte_data, error), nullptr);
  EXPECT_EQ(error, "SMPTE MIDI time division is not supported");

  const Vector<uint8_t> unsupported_system_track = {0x00, 0xf1, 0x00};
  const Vector<Vector<uint8_t>> unsupported_system_tracks = {unsupported_system_track};
  const Vector<uint8_t> unsupported_system_data = midi_file(0, 480, unsupported_system_tracks);
  EXPECT_EQ(MidiFile::parse(unsupported_system_data, error), nullptr);
  EXPECT_EQ(error, "Unsupported MIDI system event");

  const Vector<uint8_t> missing_end_track = {0x00, 0x90, 0x3c, 0x40};
  const Vector<Vector<uint8_t>> missing_end_tracks = {missing_end_track};
  const Vector<uint8_t> missing_end_data = midi_file(0, 480, missing_end_tracks);
  EXPECT_EQ(MidiFile::parse(missing_end_data, error), nullptr);
  EXPECT_EQ(error, "MIDI track is missing an End of Track event");

  const Vector<uint8_t> trailing_event_track = {0x00, 0xff, 0x2f, 0x00, 0x00, 0x90, 0x3c, 0x40};
  const Vector<Vector<uint8_t>> trailing_event_tracks = {trailing_event_track};
  const Vector<uint8_t> trailing_event_data = midi_file(0, 480, trailing_event_tracks);
  EXPECT_EQ(MidiFile::parse(trailing_event_data, error), nullptr);
  EXPECT_EQ(error, "MIDI End of Track event is not the last event");
}

}  // namespace blender::io::midi::tests
