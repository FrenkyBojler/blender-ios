/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <memory>
#include <optional>
#include <string>

#include "IO_midi.hh"

#include "BLI_generic_key_string.hh"
#include "BLI_memory_cache_file_load.hh"
#include "BLI_memory_counter.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_midi_note_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Path"_ustr)
      .subtype(PROP_FILEPATH)
      .path_filter("*.mid;*.midi")
      .optional_label()
      .description("Path to a MIDI file (.mid or .midi)");
  b.add_input<decl::Float>("Time"_ustr)
      .subtype(PROP_TIME_ABSOLUTE)
      .structure_type(StructureType::Dynamic)
      .description("Time in seconds at which to sample the note");
  b.add_input<decl::Int>("Channel"_ustr)
      .default_value(1)
      .min(1)
      .max(16)
      .structure_type(StructureType::Dynamic)
      .description("MIDI channel from 1 to 16");
  b.add_input<decl::Int>("Note"_ustr)
      .default_value(60)
      .min(0)
      .max(127)
      .structure_type(StructureType::Dynamic)
      .description("MIDI note number from 0 to 127");

  b.add_output<decl::Bool>("Is Playing"_ustr)
      .propagate_references()
      .structure_type(StructureType::Dynamic)
      .description("Whether at least one matching note-on event is active at the sampled time");
  b.add_output<decl::Float>("Velocity"_ustr)
      .propagate_references()
      .structure_type(StructureType::Dynamic)
      .description("Velocity of the newest active note-on event, or zero when inactive");
  b.add_output<decl::Float>("Last Hit Velocity"_ustr)
      .propagate_references()
      .structure_type(StructureType::Dynamic)
      .description("Velocity of the most recent note-on event, including when inactive");
  b.add_output<decl::Float>("Time Since Last Hit"_ustr)
      .propagate_references()
      .structure_type(StructureType::Dynamic)
      .description("Seconds since the most recent note-on event, or zero before the first event");
  b.add_output<decl::Float>("Time Since Note Start"_ustr)
      .propagate_references()
      .structure_type(StructureType::Dynamic)
      .description("Seconds since the newest active note-on event, or zero when inactive");
}

class LoadMidiCache : public memory_cache::CachedValue {
 public:
  std::unique_ptr<io::midi::MidiFile> midi;
  std::string error;

  void count_memory(MemoryCounter &counter) const override
  {
    counter.add(this->error.size());
    if (this->midi) {
      counter.add(this->midi->memory_usage());
    }
  }
};

class SampleMidiNoteFunction : public mf::MultiFunction {
 private:
  std::shared_ptr<const LoadMidiCache> cache_;

 public:
  explicit SampleMidiNoteFunction(std::shared_ptr<const LoadMidiCache> cache)
      : cache_(std::move(cache))
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Sample MIDI Note", signature};
      builder.single_input<float>("Time");
      builder.single_input<int>("Channel");
      builder.single_input<int>("Note");
      builder.single_output<bool>("Is Playing", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Velocity", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Last Hit Velocity", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Time Since Last Hit", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Time Since Note Start", mf::ParamFlag::SupportsUnusedOutput);
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float> &times = params.readonly_single_input<float>(0, "Time");
    const VArray<int> &channels = params.readonly_single_input<int>(1, "Channel");
    const VArray<int> &notes = params.readonly_single_input<int>(2, "Note");
    MutableSpan<bool> is_playing = params.uninitialized_single_output_if_required<bool>(
        3, "Is Playing");
    MutableSpan<float> velocities = params.uninitialized_single_output_if_required<float>(
        4, "Velocity");
    MutableSpan<float> last_hit_velocities = params.uninitialized_single_output_if_required<float>(
        5, "Last Hit Velocity");
    MutableSpan<float> times_since_last_hit =
        params.uninitialized_single_output_if_required<float>(6, "Time Since Last Hit");
    MutableSpan<float> times_since_note_start =
        params.uninitialized_single_output_if_required<float>(7, "Time Since Note Start");

    mask.foreach_index([&](const int i) {
      const io::midi::NoteSample sample = cache_->midi->sample(
          times[i], channels[i] - 1, notes[i]);
      if (!is_playing.is_empty()) {
        is_playing[i] = sample.is_playing;
      }
      if (!velocities.is_empty()) {
        velocities[i] = sample.velocity;
      }
      if (!last_hit_velocities.is_empty()) {
        last_hit_velocities[i] = sample.last_hit_velocity;
      }
      if (!times_since_last_hit.is_empty()) {
        times_since_last_hit[i] = sample.time_since_last_hit;
      }
      if (!times_since_note_start.is_empty()) {
        times_since_note_start[i] = sample.time_since_note_start;
      }
    });
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const std::optional<std::string> path = params.ensure_absolute_path(
      params.extract_input<std::string>("Path"_ustr));
  if (!path) {
    params.set_default_remaining_outputs();
    return;
  }

  const std::shared_ptr<const LoadMidiCache> cache = memory_cache::get_loaded<LoadMidiCache>(
      GenericStringKey{"sample_midi_note_node"}, {StringRefNull(*path)}, [&]() {
        auto value = std::make_unique<LoadMidiCache>();
        value->midi = io::midi::MidiFile::load(*path, value->error);
        return value;
      });
  if (!cache->midi) {
    params.error_message_add(NodeWarningType::Error, cache->error);
    params.set_default_remaining_outputs();
    return;
  }

  SocketValueVariant times = params.extract_input<SocketValueVariant>("Time"_ustr);
  SocketValueVariant channels = params.extract_input<SocketValueVariant>("Channel"_ustr);
  SocketValueVariant notes = params.extract_input<SocketValueVariant>("Note"_ustr);
  SocketValueVariant is_playing;
  SocketValueVariant velocity;
  SocketValueVariant last_hit_velocity;
  SocketValueVariant time_since_last_hit;
  SocketValueVariant time_since_note_start;

  std::string error_message;
  if (!execute_multi_function_on_value_variant(std::make_shared<SampleMidiNoteFunction>(cache),
                                               {&times, &channels, &notes},
                                               {&is_playing,
                                                &velocity,
                                                &last_hit_velocity,
                                                &time_since_last_hit,
                                                &time_since_note_start},
                                               params.user_data(),
                                               error_message))
  {
    params.error_message_add(NodeWarningType::Error, std::move(error_message));
    params.set_default_remaining_outputs();
    return;
  }

  params.set_output("Is Playing"_ustr, std::move(is_playing));
  params.set_output("Velocity"_ustr, std::move(velocity));
  params.set_output("Last Hit Velocity"_ustr, std::move(last_hit_velocity));
  params.set_output("Time Since Last Hit"_ustr, std::move(time_since_last_hit));
  params.set_output("Time Since Note Start"_ustr, std::move(time_since_note_start));
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSampleMIDINote"_ustr);
  ntype.ui_name = "Sample MIDI Note";
  ntype.ui_description = "Sample the state of a MIDI note at a given time";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.default_width = bke::NodeWidth::_180;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sample_midi_note_cc
