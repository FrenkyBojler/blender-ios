/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>

#include "IO_midi.hh"

#include "BLI_fileops.h"
#include "BLI_map.hh"
#include "BLI_memory_utils.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

namespace blender::io::midi {

namespace {

struct RawNoteEvent {
  uint64_t tick;
  uint64_t order;
  uint8_t velocity;
  bool is_note_on;
};

struct TempoEvent {
  uint64_t tick;
  uint64_t order;
  uint32_t microseconds_per_quarter_note;
};

struct TempoPoint {
  uint64_t tick;
  double time;
  uint32_t microseconds_per_quarter_note;
};

struct NoteStateEvent {
  double time;
  double note_start_time;
  double last_hit_time;
  float velocity;
  float last_hit_velocity;
  bool is_playing;
  bool has_last_hit;
};

struct ActiveNote {
  double start_time = 0.0;
  float velocity = 0.0f;
};

class Reader {
 private:
  Span<uint8_t> data_;
  int64_t position_ = 0;

 public:
  explicit Reader(const Span<uint8_t> data) : data_(data) {}

  int64_t remaining() const
  {
    return data_.size() - position_;
  }

  bool read_byte(uint8_t &r_value)
  {
    if (this->remaining() < 1) {
      return false;
    }
    r_value = data_[position_++];
    return true;
  }

  bool read_u16(uint16_t &r_value)
  {
    uint8_t a, b;
    if (!this->read_byte(a) || !this->read_byte(b)) {
      return false;
    }
    r_value = (uint16_t(a) << 8) | uint16_t(b);
    return true;
  }

  bool read_u32(uint32_t &r_value)
  {
    uint8_t a, b, c, d;
    if (!this->read_byte(a) || !this->read_byte(b) || !this->read_byte(c) || !this->read_byte(d)) {
      return false;
    }
    r_value = (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | uint32_t(d);
    return true;
  }

  bool read_variable_length(uint32_t &r_value)
  {
    r_value = 0;
    for (int i = 0; i < 4; i++) {
      uint8_t byte;
      if (!this->read_byte(byte)) {
        return false;
      }
      r_value = (r_value << 7) | (byte & 0x7f);
      if ((byte & 0x80) == 0) {
        return true;
      }
    }
    return false;
  }

  bool skip(const int64_t size)
  {
    if (size < 0 || this->remaining() < size) {
      return false;
    }
    position_ += size;
    return true;
  }

  std::optional<Span<uint8_t>> read_span(const int64_t size)
  {
    if (size < 0 || this->remaining() < size) {
      return std::nullopt;
    }
    const Span<uint8_t> result = data_.slice(position_, size);
    position_ += size;
    return result;
  }
};

static bool read_chunk_id(Reader &reader, const char *expected)
{
  const std::optional<Span<uint8_t>> id = reader.read_span(4);
  return id && (*id)[0] == expected[0] && (*id)[1] == expected[1] && (*id)[2] == expected[2] &&
         (*id)[3] == expected[3];
}

static bool read_data_byte(Reader &reader, uint8_t &r_value)
{
  return reader.read_byte(r_value) && r_value < 0x80;
}

static bool parse_track(const Span<uint8_t> data,
                        Map<int, Vector<RawNoteEvent>> &r_note_events,
                        Vector<TempoEvent> &r_tempo_events,
                        uint64_t &r_order,
                        std::string &r_error)
{
  Reader reader(data);
  uint64_t tick = 0;
  uint8_t running_status = 0;

  while (reader.remaining() > 0) {
    uint32_t delta_time;
    if (!reader.read_variable_length(delta_time) ||
        tick > std::numeric_limits<uint64_t>::max() - delta_time)
    {
      r_error = "Invalid MIDI event delta time";
      return false;
    }
    tick += delta_time;

    uint8_t first_byte;
    if (!reader.read_byte(first_byte)) {
      r_error = "Unexpected end of MIDI track";
      return false;
    }

    uint8_t status;
    std::optional<uint8_t> first_data_byte;
    if (first_byte < 0x80) {
      if (running_status == 0) {
        r_error = "MIDI running status has no preceding channel event";
        return false;
      }
      status = running_status;
      first_data_byte = first_byte;
    }
    else {
      status = first_byte;
      if (status < 0xf0) {
        running_status = status;
      }
      else {
        running_status = 0;
      }
    }

    if (status == 0xff) {
      uint8_t type;
      uint32_t length;
      if (!reader.read_byte(type) || !reader.read_variable_length(length)) {
        r_error = "Invalid MIDI meta event";
        return false;
      }
      const std::optional<Span<uint8_t>> payload = reader.read_span(length);
      if (!payload) {
        r_error = "MIDI meta event exceeds the track length";
        return false;
      }
      if (type == 0x2f) {
        if (!payload->is_empty()) {
          r_error = "Invalid MIDI End of Track event";
          return false;
        }
        if (reader.remaining() != 0) {
          r_error = "MIDI End of Track event is not the last event";
          return false;
        }
        return true;
      }
      if (type == 0x51) {
        if (payload->size() != 3) {
          r_error = "Invalid MIDI Set Tempo event";
          return false;
        }
        const uint32_t tempo = (uint32_t((*payload)[0]) << 16) | (uint32_t((*payload)[1]) << 8) |
                               uint32_t((*payload)[2]);
        if (tempo == 0) {
          r_error = "MIDI tempo must be greater than zero";
          return false;
        }
        r_tempo_events.append({tick, r_order++, tempo});
      }
      else {
        r_order++;
      }
      continue;
    }

    if (ELEM(status, 0xf0, 0xf7)) {
      uint32_t length;
      if (!reader.read_variable_length(length) || !reader.skip(length)) {
        r_error = "MIDI SysEx event exceeds the track length";
        return false;
      }
      r_order++;
      continue;
    }

    if (status >= 0xf0) {
      r_error = "Unsupported MIDI system event";
      return false;
    }

    const uint8_t message_type = status & 0xf0;
    const int data_size = ELEM(message_type, 0xc0, 0xd0) ? 1 : 2;
    uint8_t message_data[2];
    int data_index = 0;
    if (first_data_byte) {
      message_data[data_index++] = *first_data_byte;
    }
    while (data_index < data_size) {
      if (!read_data_byte(reader, message_data[data_index++])) {
        r_error = "Invalid MIDI channel event";
        return false;
      }
    }

    if (ELEM(message_type, 0x80, 0x90)) {
      const int channel = status & 0x0f;
      const int note = message_data[0];
      const uint8_t velocity = message_data[1];
      const bool is_note_on = message_type == 0x90 && velocity != 0;
      r_note_events.lookup_or_add_default(channel * 128 + note)
          .append({tick, r_order, velocity, is_note_on});
    }
    r_order++;
  }

  r_error = "MIDI track is missing an End of Track event";
  return false;
}

static double tick_to_time(const uint64_t tick,
                           const Span<TempoPoint> tempo_points,
                           const uint16_t ticks_per_quarter_note)
{
  const auto point_after_tick = std::upper_bound(
      tempo_points.begin(),
      tempo_points.end(),
      tick,
      [](const uint64_t tick, const TempoPoint &point) { return tick < point.tick; });
  const TempoPoint &point = *(point_after_tick - 1);
  const double seconds_per_tick = double(point.microseconds_per_quarter_note) /
                                  (1'000'000.0 * ticks_per_quarter_note);
  return point.time + double(tick - point.tick) * seconds_per_tick;
}

}  // namespace

struct MidiFile::Impl {
  Map<int, Vector<NoteStateEvent>> events_by_note;
  int64_t memory_usage = sizeof(Impl);
};

MidiFile::MidiFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MidiFile::~MidiFile() = default;
MidiFile::MidiFile(MidiFile &&) = default;
MidiFile &MidiFile::operator=(MidiFile &&) = default;

std::unique_ptr<MidiFile> MidiFile::parse(const Span<uint8_t> data, std::string &r_error)
{
  r_error.clear();
  Reader reader(data);
  if (!read_chunk_id(reader, "MThd")) {
    r_error = "Missing MIDI header";
    return nullptr;
  }

  uint32_t header_length;
  if (!reader.read_u32(header_length) || header_length < 6) {
    r_error = "Invalid MIDI header";
    return nullptr;
  }
  const std::optional<Span<uint8_t>> header_data = reader.read_span(header_length);
  if (!header_data) {
    r_error = "MIDI header exceeds the file length";
    return nullptr;
  }

  Reader header_reader(*header_data);
  uint16_t format, tracks_num, time_division;
  if (!header_reader.read_u16(format) || !header_reader.read_u16(tracks_num) ||
      !header_reader.read_u16(time_division))
  {
    r_error = "Invalid MIDI header";
    return nullptr;
  }
  if (format > 1) {
    r_error = "MIDI format 2 is not supported";
    return nullptr;
  }
  if (tracks_num == 0 || (format == 0 && tracks_num != 1)) {
    r_error = "Invalid number of MIDI tracks";
    return nullptr;
  }
  if ((time_division & 0x8000) != 0) {
    r_error = "SMPTE MIDI time division is not supported";
    return nullptr;
  }
  if (time_division == 0) {
    r_error = "MIDI time division must be greater than zero";
    return nullptr;
  }

  Map<int, Vector<RawNoteEvent>> raw_note_events;
  Vector<TempoEvent> tempo_events;
  uint64_t order = 0;
  for (int track_i = 0; track_i < tracks_num; track_i++) {
    if (!read_chunk_id(reader, "MTrk")) {
      r_error = "Missing MIDI track";
      return nullptr;
    }
    uint32_t track_length;
    if (!reader.read_u32(track_length)) {
      r_error = "Invalid MIDI track header";
      return nullptr;
    }
    const std::optional<Span<uint8_t>> track_data = reader.read_span(track_length);
    if (!track_data) {
      r_error = "MIDI track exceeds the file length";
      return nullptr;
    }
    if (!parse_track(*track_data, raw_note_events, tempo_events, order, r_error)) {
      return nullptr;
    }
  }

  std::sort(
      tempo_events.begin(), tempo_events.end(), [](const TempoEvent &a, const TempoEvent &b) {
        return std::tie(a.tick, a.order) < std::tie(b.tick, b.order);
      });

  Vector<TempoPoint> tempo_points;
  tempo_points.reserve(tempo_events.size() + 1);
  tempo_points.append({0, 0.0, 500'000});
  uint64_t previous_tick = 0;
  double previous_time = 0.0;
  uint32_t previous_tempo = 500'000;
  for (const TempoEvent &event : tempo_events) {
    const double seconds_per_tick = double(previous_tempo) / (1'000'000.0 * time_division);
    previous_time += double(event.tick - previous_tick) * seconds_per_tick;
    previous_tick = event.tick;
    previous_tempo = event.microseconds_per_quarter_note;
    tempo_points.append({event.tick, previous_time, previous_tempo});
  }

  auto impl = std::make_unique<Impl>();
  for (auto item : raw_note_events.items()) {
    Vector<RawNoteEvent> &events = item.value;
    std::sort(events.begin(), events.end(), [](const RawNoteEvent &a, const RawNoteEvent &b) {
      return std::tie(a.tick, a.order) < std::tie(b.tick, b.order);
    });

    Vector<NoteStateEvent> state_events;
    state_events.reserve(events.size());
    Vector<ActiveNote> active_notes;
    int first_active_note = 0;
    bool has_last_hit = false;
    double last_hit_time = 0.0;
    float last_hit_velocity = 0.0f;

    for (const RawNoteEvent &event : events) {
      const double time = tick_to_time(event.tick, tempo_points, time_division);
      if (event.is_note_on) {
        const float velocity = event.velocity / 127.0f;
        active_notes.append({time, velocity});
        has_last_hit = true;
        last_hit_time = time;
        last_hit_velocity = velocity;
      }
      else if (first_active_note < active_notes.size()) {
        /* Match note-offs to the oldest active hit. The newest remaining hit is sampled below. */
        first_active_note++;
        if (first_active_note == active_notes.size()) {
          active_notes.clear();
          first_active_note = 0;
        }
      }

      const bool is_playing = first_active_note < active_notes.size();
      const ActiveNote active_note = is_playing ? active_notes.last() : ActiveNote{};
      state_events.append({time,
                           active_note.start_time,
                           last_hit_time,
                           active_note.velocity,
                           last_hit_velocity,
                           is_playing,
                           has_last_hit});
    }

    impl->memory_usage += int64_t(state_events.capacity()) * sizeof(NoteStateEvent);
    impl->events_by_note.add(item.key, std::move(state_events));
  }
  impl->memory_usage += impl->events_by_note.size_in_bytes();

  return std::unique_ptr<MidiFile>(new MidiFile(std::move(impl)));
}

std::unique_ptr<MidiFile> MidiFile::load(const StringRefNull path, std::string &r_error)
{
  size_t size;
  void *data = BLI_file_read_binary_as_mem(path.c_str(), 0, &size);
  if (!data) {
    r_error = "Unable to read MIDI file";
    return nullptr;
  }
  BLI_SCOPED_DEFER([&]() { MEM_delete_void(data); });
  return MidiFile::parse(Span<uint8_t>(static_cast<const uint8_t *>(data), int64_t(size)),
                         r_error);
}

NoteSample MidiFile::sample(const double time, const int channel, const int note) const
{
  NoteSample sample;
  if (!std::isfinite(time) || channel < 0 || channel >= 16 || note < 0 || note >= 128) {
    return sample;
  }

  const Vector<NoteStateEvent> *events = impl_->events_by_note.lookup_ptr(channel * 128 + note);
  if (!events) {
    return sample;
  }
  const auto event_after_time = std::upper_bound(
      events->begin(),
      events->end(),
      time,
      [](const double sample_time, const NoteStateEvent &event) {
        return sample_time < event.time;
      });
  if (event_after_time == events->begin()) {
    return sample;
  }

  const NoteStateEvent &event = *(event_after_time - 1);
  sample.is_playing = event.is_playing;
  sample.velocity = event.is_playing ? event.velocity : 0.0f;
  sample.last_hit_velocity = event.has_last_hit ? event.last_hit_velocity : 0.0f;
  sample.time_since_last_hit = event.has_last_hit ?
                                   float(std::max(0.0, time - event.last_hit_time)) :
                                   0.0f;
  sample.time_since_note_start = event.is_playing ?
                                     float(std::max(0.0, time - event.note_start_time)) :
                                     0.0f;
  return sample;
}

int64_t MidiFile::memory_usage() const
{
  return impl_->memory_usage;
}

}  // namespace blender::io::midi
