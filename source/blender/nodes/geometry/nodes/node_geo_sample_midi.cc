/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_midi_cc {

/* -------------------------------------------------------------------- */
/** \name MIDI file parser
 * \{ */

static uint16_t midi_u16(const uint8_t *p)
{
  return uint16_t(p[0]) << 8 | p[1];
}

static uint32_t midi_u32(const uint8_t *p)
{
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

static uint32_t midi_vlq(const uint8_t *data, const size_t size, size_t &pos)
{
  uint32_t value = 0;
  for (int i = 0; i < 4 && pos < size; i++) {
    const uint8_t byte = data[pos++];
    value = (value << 7) | (byte & 0x7Fu);
    if (!(byte & 0x80u)) {
      break;
    }
  }
  return value;
}

struct TempoChange {
  uint32_t tick;
  uint32_t us_per_beat;
};

struct RawNoteEvent {
  uint32_t tick;
  uint8_t channel;
  uint8_t note;
  bool is_on;
  uint8_t velocity;
};

static void parse_midi_track(const uint8_t *data,
                             const size_t size,
                             Vector<RawNoteEvent> &note_events,
                             Vector<TempoChange> &tempo_changes,
                             uint32_t &end_tick)
{
  size_t pos = 0;
  uint32_t tick = 0;
  uint8_t running_status = 0;

  while (pos < size) {
    tick += midi_vlq(data, size, pos);
    if (pos >= size) {
      break;
    }

    const uint8_t byte = data[pos];
    uint8_t status;
    if (byte & 0x80u) {
      status = byte;
      pos++;
      if (status < 0xF0u) {
        running_status = status;
      }
      else if (status == 0xF0u || status == 0xF7u) {
        running_status = 0;
      }
    }
    else {
      if (!running_status) {
        break;
      }
      status = running_status;
    }

    if (status == 0xFFu) {
      /* Meta event. */
      if (pos >= size) {
        break;
      }
      const uint8_t meta_type = data[pos++];
      const uint32_t meta_len = midi_vlq(data, size, pos);
      if (meta_type == 0x2Fu) {
        /* End of track. */
        break;
      }
      if (meta_type == 0x51u && meta_len == 3 && pos + 3 <= size) {
        /* Set Tempo. */
        const uint32_t us = uint32_t(data[pos]) << 16 | uint32_t(data[pos + 1]) << 8 |
                            data[pos + 2];
        tempo_changes.append({tick, us ? us : 500000u});
      }
      if (pos + meta_len > size) {
        break;
      }
      pos += meta_len;
      continue;
    }

    if (status == 0xF0u || status == 0xF7u) {
      /* SysEx: skip data bytes. */
      const uint32_t len = midi_vlq(data, size, pos);
      if (pos + len > size) {
        break;
      }
      pos += len;
      continue;
    }

    const uint8_t type = status & 0xF0u;
    const uint8_t channel = status & 0x0Fu;

    if (type == 0x90u) {
      /* Note On. */
      if (pos + 2 > size) {
        break;
      }
      const uint8_t note = data[pos++];
      const uint8_t vel = data[pos++];
      note_events.append({tick, channel, note, vel > 0, vel});
    }
    else if (type == 0x80u) {
      /* Note Off. */
      if (pos + 2 > size) {
        break;
      }
      const uint8_t note = data[pos++];
      pos++; /* release velocity */
      note_events.append({tick, channel, note, false, 0});
    }
    else if (type == 0xA0u || type == 0xB0u || type == 0xE0u) {
      /* Aftertouch / Control Change / Pitch Bend: 2 data bytes. */
      if (pos + 2 > size) {
        break;
      }
      pos += 2;
    }
    else if (type == 0xC0u || type == 0xD0u) {
      /* Program Change / Channel Pressure: 1 data byte. */
      if (pos + 1 > size) {
        break;
      }
      pos++;
    }
  }

  end_tick = std::max(end_tick, tick);
}

struct NoteInterval {
  float start;
  float end;
  uint8_t velocity;
};

/* Map key: channel << 8 | note. */
using NoteKey = uint16_t;

struct MidiData {
  Map<NoteKey, Vector<NoteInterval>> intervals;
  /* Sorted list of note-on times per key, used for "time since last hit". */
  Map<NoteKey, Vector<float>> hit_times;
  bool valid = false;
};

static float ticks_to_seconds(const uint32_t tick,
                              const Vector<TempoChange> &tempo_changes,
                              const uint32_t ticks_per_beat)
{
  if (ticks_per_beat == 0) {
    return 0.0f;
  }
  double t = 0.0;
  uint32_t prev_tick = 0;
  uint32_t us_per_beat = 500000u; /* Default: 120 BPM. */
  for (const TempoChange &tc : tempo_changes) {
    if (tc.tick >= tick) {
      break;
    }
    t += double(tc.tick - prev_tick) * us_per_beat / ticks_per_beat / 1e6;
    prev_tick = tc.tick;
    us_per_beat = tc.us_per_beat;
  }
  t += double(tick - prev_tick) * us_per_beat / ticks_per_beat / 1e6;
  return float(t);
}

static MidiData load_midi_file(const std::string &path)
{
  MidiData result;

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return result;
  }
  const size_t file_size = size_t(file.tellg());
  file.seekg(0);
  if (file_size < 14) {
    return result;
  }

  Vector<uint8_t> data(file_size);
  if (!file.read(reinterpret_cast<char *>(data.data()), std::streamsize(file_size))) {
    return result;
  }

  if (std::memcmp(data.data(), "MThd", 4) != 0) {
    return result;
  }
  const uint32_t header_len = midi_u32(data.data() + 4);
  if (header_len < 6 || 8 + size_t(header_len) > file_size) {
    return result;
  }
  const uint16_t num_tracks = midi_u16(data.data() + 10);
  const uint16_t timing = midi_u16(data.data() + 12);
  if (timing & 0x8000u) {
    /* SMPTE time code not supported. */
    return result;
  }
  const uint32_t ticks_per_beat = timing;

  Vector<RawNoteEvent> note_events;
  Vector<TempoChange> tempo_changes;
  tempo_changes.append({0u, 500000u}); /* Default tempo: 120 BPM. */
  uint32_t end_tick = 0;

  size_t pos = 8 + header_len;
  for (uint16_t t = 0; t < num_tracks && pos + 8 <= file_size; t++) {
    if (std::memcmp(data.data() + pos, "MTrk", 4) != 0) {
      break;
    }
    const uint32_t track_len = midi_u32(data.data() + pos + 4);
    const size_t track_start = pos + 8;
    if (track_start + track_len > file_size) {
      break;
    }
    parse_midi_track(
        data.data() + track_start, track_len, note_events, tempo_changes, end_tick);
    pos = track_start + track_len;
  }

  std::sort(tempo_changes.begin(), tempo_changes.end(), [](const TempoChange &a, const TempoChange &b) {
    return a.tick < b.tick;
  });
  std::sort(note_events.begin(), note_events.end(), [](const RawNoteEvent &a, const RawNoteEvent &b) {
    return a.tick < b.tick;
  });

  /* Build held-note intervals from note-on/off event pairs.
   * pending maps NoteKey -> (start_tick, velocity). */
  Map<NoteKey, std::pair<uint32_t, uint8_t>> pending;

  for (const RawNoteEvent &ev : note_events) {
    const NoteKey key = NoteKey(ev.channel) << 8 | ev.note;
    if (ev.is_on) {
      const float hit_time = ticks_to_seconds(ev.tick, tempo_changes, ticks_per_beat);
      result.hit_times.lookup_or_add_default(key).append(hit_time);
      if (pending.contains(key)) {
        /* Retrigger without release: close the previous interval now. */
        const auto [start_tick, vel] = pending.lookup(key);
        const float start = ticks_to_seconds(start_tick, tempo_changes, ticks_per_beat);
        const float end = hit_time;
        if (end > start) {
          result.intervals.lookup_or_add_default(key).append({start, end, vel});
        }
      }
      pending.add_overwrite(key, {ev.tick, ev.velocity});
    }
    else {
      if (pending.contains(key)) {
        const auto [start_tick, vel] = pending.lookup(key);
        pending.remove(key);
        const float start = ticks_to_seconds(start_tick, tempo_changes, ticks_per_beat);
        const float end = ticks_to_seconds(ev.tick, tempo_changes, ticks_per_beat);
        if (end > start) {
          result.intervals.lookup_or_add_default(key).append({start, end, vel});
        }
      }
    }
  }

  /* Close any notes still held at the end of the file. */
  const float end_seconds = ticks_to_seconds(end_tick, tempo_changes, ticks_per_beat);
  for (auto &&[key, start_and_vel] : pending.items()) {
    const float start = ticks_to_seconds(start_and_vel.first, tempo_changes, ticks_per_beat);
    if (end_seconds > start) {
      result.intervals.lookup_or_add_default(key).append(
          {start, end_seconds, start_and_vel.second});
    }
  }

  result.valid = true;
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cache
 * \{ */

struct MidiFileCache {
  std::mutex mutex;
  Map<std::string, std::shared_ptr<const MidiData>> by_path;
};

static MidiFileCache &midi_file_cache()
{
  static MidiFileCache cache;
  return cache;
}

static std::shared_ptr<const MidiData> get_or_load_midi(const std::string &path)
{
  MidiFileCache &cache = midi_file_cache();
  std::lock_guard lock(cache.mutex);
  if (std::shared_ptr<const MidiData> *existing = cache.by_path.lookup_ptr(path)) {
    return *existing;
  }
  auto data = std::make_shared<MidiData>(load_midi_file(path));
  cache.by_path.add(path, data);
  return data;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name MultiFunction
 * \{ */

class SampleMidiFunction : public mf::MultiFunction {
 private:
  std::shared_ptr<const MidiData> midi_;

 public:
  SampleMidiFunction(std::shared_ptr<const MidiData> midi) : midi_(std::move(midi))
  {
    static const mf::Signature signature = []() {
      mf::Signature sig;
      mf::SignatureBuilder builder("Sample MIDI", sig);
      builder.single_input<float>("Time");
      builder.single_input<int>("Channel");
      builder.single_input<int>("Note");
      builder.single_output<bool>("Is Playing");
      builder.single_output<float>("Velocity");
      builder.single_output<float>("Time Since Last Hit");
      return sig;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float> &times = params.readonly_single_input<float>(0, "Time");
    const VArray<int> &channels = params.readonly_single_input<int>(1, "Channel");
    const VArray<int> &notes = params.readonly_single_input<int>(2, "Note");
    MutableSpan<bool> is_playing = params.uninitialized_single_output<bool>(3, "Is Playing");
    MutableSpan<float> velocities = params.uninitialized_single_output<float>(4, "Velocity");
    MutableSpan<float> time_since_hit = params.uninitialized_single_output<float>(
        5, "Time Since Last Hit");

    mask.foreach_index([&](const int i) {
      const int channel = channels[i];
      const int note = notes[i];
      const float time = times[i];
      if (channel < 0 || channel > 15 || note < 0 || note > 127) {
        is_playing[i] = false;
        velocities[i] = 0.0f;
        time_since_hit[i] = time;
        return;
      }
      const NoteKey key = NoteKey(channel) << 8 | uint8_t(note);

      /* --- Is Playing / Velocity --- */
      const Vector<NoteInterval> *ivs = midi_->intervals.lookup_ptr(key);
      if (!ivs || ivs->is_empty()) {
        is_playing[i] = false;
        velocities[i] = 0.0f;
      }
      else {
        /* Binary search for the last interval starting at or before `time`. */
        int lo = 0, hi = int(ivs->size()) - 1, found = -1;
        while (lo <= hi) {
          const int mid = (lo + hi) / 2;
          if ((*ivs)[mid].start <= time) {
            found = mid;
            lo = mid + 1;
          }
          else {
            hi = mid - 1;
          }
        }
        if (found >= 0 && time < (*ivs)[found].end) {
          is_playing[i] = true;
          velocities[i] = (*ivs)[found].velocity / 127.0f;
        }
        else {
          is_playing[i] = false;
          velocities[i] = 0.0f;
        }
      }

      /* --- Time Since Last Hit --- */
      const Vector<float> *hits = midi_->hit_times.lookup_ptr(key);
      if (!hits || hits->is_empty()) {
        time_since_hit[i] = time;
      }
      else {
        const float *it = std::upper_bound(hits->data(), hits->data() + hits->size(), time);
        if (it == hits->data()) {
          /* No hit before current time. */
          time_since_hit[i] = time;
        }
        else {
          --it;
          time_since_hit[i] = time - *it;
        }
      }
    });
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node
 * \{ */

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Bool>("Is Playing"_ustr)
      .structure_type(StructureType::Field)
      .propagate_references()
      .description("Whether the note is currently held at the given time");
  b.add_output<decl::Float>("Velocity"_ustr)
      .propagate_references()
      .description("Note velocity normalized to 0-1 (0 when not playing)")
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Time Since Last Hit"_ustr)
      .propagate_references()
      .description("Seconds since the last note-on event for this note")
      .structure_type(StructureType::Dynamic);

  b.add_input<decl::String>("File"_ustr)
      .subtype(PROP_FILEPATH)
      .path_filter("*.mid")
      .optional_label()
      .description("Path to a MIDI file");
  b.add_input<decl::Float>("Time"_ustr)
      .subtype(PROP_TIME_ABSOLUTE)
      .structure_type(StructureType::Dynamic)
      .description("Time in seconds to sample the MIDI file at");
  b.add_input<decl::Int>("Channel"_ustr)
      .min(0)
      .max(15)
      .default_value(0)
      .structure_type(StructureType::Dynamic)
      .description("MIDI channel (0-15)");
  b.add_input<decl::Int>("Note"_ustr)
      .min(0)
      .max(127)
      .default_value(60)
      .structure_type(StructureType::Dynamic)
      .description("MIDI note number (60 = Middle C)");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const std::optional<std::string> path = params.ensure_absolute_path(
      params.extract_input<std::string>("File"_ustr));
  if (!path) {
    params.set_default_remaining_outputs();
    return;
  }

  std::shared_ptr<const MidiData> midi = get_or_load_midi(*path);
  if (!midi->valid) {
    params.error_message_add(NodeWarningType::Warning, TIP_("Could not load MIDI file"));
    params.set_default_remaining_outputs();
    return;
  }

  SocketValueVariant times = params.extract_input<SocketValueVariant>("Time"_ustr);
  SocketValueVariant channels = params.extract_input<SocketValueVariant>("Channel"_ustr);
  SocketValueVariant notes = params.extract_input<SocketValueVariant>("Note"_ustr);

  auto fn = std::make_shared<SampleMidiFunction>(std::move(midi));

  SocketValueVariant is_playing_out;
  SocketValueVariant velocity_out;
  SocketValueVariant time_since_hit_out;
  std::string error_message;
  if (!execute_multi_function_on_value_variant(
          std::move(fn),
          {&times, &channels, &notes},
          {&is_playing_out, &velocity_out, &time_since_hit_out},
          params.user_data(),
          error_message))
  {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, std::move(error_message));
    return;
  }

  params.set_output("Is Playing"_ustr, std::move(is_playing_out));
  params.set_output("Velocity"_ustr, std::move(velocity_out));
  params.set_output("Time Since Last Hit"_ustr, std::move(time_since_hit_out));
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSampleMidi"_ustr);
  ntype.ui_name = "Sample MIDI";
  ntype.ui_description = "Sample a note state from a MIDI file at a given time";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.default_width = bke::NodeWidth::_180;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

/** \} */

}  // namespace blender::nodes::node_geo_sample_midi_cc
