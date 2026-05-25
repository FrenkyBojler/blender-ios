/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <array>
#include <optional>

#include "BKE_sound_sample.hh"

#include "BLI_multi_value_map.hh"

#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_sound_wavelet_energy_cc {

enum class WindowSize {
  _256 = 256,
  _512 = 512,
  _1024 = 1024,
  _2048 = 2048,
  _4096 = 4096,
  _8192 = 8192,
};

static const EnumPropertyItem window_size_items[] = {
    {int(WindowSize::_256), "256", 0, "256", ""},
    {int(WindowSize::_512), "512", 0, "512", ""},
    {int(WindowSize::_1024), "1024", 0, "1024", ""},
    {int(WindowSize::_2048), "2048", 0, "2048", ""},
    {int(WindowSize::_4096), "4096", 0, "4096", ""},
    {int(WindowSize::_8192), "8192", 0, "8192", ""},
    {},
};

static const EnumPropertyItem band_items[] = {
    {int(bke::WaveletBand::FullRange),
     "FULL_RANGE",
     0,
     "Full Range",
     "Average energy across all useful wavelet detail levels"},
    {int(bke::WaveletBand::High),
     "HIGH",
     0,
     "High",
     "Sample the highest-frequency wavelet detail level"},
    {int(bke::WaveletBand::HighMid),
     "HIGH_MID",
     0,
     "High-Mid",
     "Sample an early wavelet detail level"},
    {int(bke::WaveletBand::Mid), "MID", 0, "Mid", "Sample a middle wavelet detail level"},
    {int(bke::WaveletBand::LowMid),
     "LOW_MID",
     0,
     "Low-Mid",
     "Sample a later wavelet detail level"},
    {int(bke::WaveletBand::Low),
     "LOW",
     0,
     "Low",
     "Sample the deepest useful wavelet detail level"},
    {},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Float>("Energy"_ustr)
      .propagate_references()
      .description("Average wavelet detail energy at the given time in the selected band. "
                   "Values scale with source loudness; louder audio produces larger values")
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Bool>("Above Threshold"_ustr)
      .propagate_references()
      .description("True when the wavelet energy exceeds the threshold")
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Sound>("Sound"_ustr).optional_label().description("Sound to analyze");
  b.add_input<decl::Float>("Time"_ustr)
      .subtype(PROP_TIME_ABSOLUTE)
      .structure_type(StructureType::Dynamic)
      .description("Time in seconds of the sound to sample at");
  b.add_input<decl::Float>("Threshold"_ustr)
      .default_value(0.01f)
      .min(0.0f)
      .structure_type(StructureType::Dynamic)
      .description("Wavelet energy level above which the threshold output is true");
  b.add_input<decl::Bool>("All Channels"_ustr)
      .default_value(true)
      .structure_type(StructureType::Dynamic)
      .description("Mix all channels before analyzing (e.g. stereo to mono)");
  b.add_input<decl::Int>("Channel"_ustr)
      .min(0)
      .structure_type(StructureType::Dynamic)
      .description("The channel to analyze unless 'All Channels' is checked");

  {
    auto &p = b.add_panel("Analysis"_ustr)
                  .default_closed(true)
                  .description("Configure the wavelet energy analysis");
    p.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr) {
      layout.prop(ptr, "band", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    });
    p.add_input<decl::Menu>("Window Size"_ustr)
        .static_items(window_size_items)
        .default_value(WindowSize::_2048)
        .optional_label()
        .description(
            "Number of samples per DWT window. Larger values capture lower-frequency detail "
            "with less precise time localization. Changing window size also shifts which "
            "frequency range each named band covers");
  }
}

class SampleSoundWaveletEnergyFunction : public mf::MultiFunction {
 private:
  const bSound &sound_;
  const bke::WaveletBand band_;
  const int window_size_;

 public:
  SampleSoundWaveletEnergyFunction(bSound &sound,
                                   const bke::WaveletBand band,
                                   const int window_size)
      : sound_(sound), band_(band), window_size_(window_size)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder("Sample Sound Wavelet Energy", signature);
      builder.single_input<float>("Time");
      builder.single_input<float>("Threshold");
      builder.single_input<bool>("All Channels");
      builder.single_input<int>("Channel");
      builder.single_output<float>("Energy");
      builder.single_output<bool>("Above Threshold");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float> &times = params.readonly_single_input<float>(0, "Time");
    const VArray<float> &thresholds = params.readonly_single_input<float>(1, "Threshold");
    const VArray<bool> &all_channels_varray = params.readonly_single_input<bool>(2,
                                                                                 "All Channels");
    const VArray<int> &channels = params.readonly_single_input<int>(3, "Channel");
    MutableSpan<float> energies = params.uninitialized_single_output<float>(4, "Energy");
    MutableSpan<bool> above_threshold = params.uninitialized_single_output<bool>(
        5, "Above Threshold");

    const std::optional<bool> all_channels_value = all_channels_varray.get_if_single();
    const std::optional<int> channel_value = channels.get_if_single();
    /* True when every element uses the same sampler key, so one cache lookup covers all. */
    const bool single_sampler_key = all_channels_value.has_value() &&
                                    (*all_channels_value || channel_value.has_value());

    /* Fast path: every element samples the same channel, so one sampler lookup suffices. */
    if (single_sampler_key) {
      bke::bSoundWaveletEnergySampler::Key key;
      key.band = band_;
      key.window_size = window_size_;
      key.channel = *all_channels_value ? std::nullopt : channel_value;
      const bke::bSoundWaveletEnergySampler *sampler = bke::bSoundWaveletEnergySampler::get_cached(
          sound_, key);
      if (!sampler) {
        index_mask::masked_fill(energies, 0.0f, mask);
        index_mask::masked_fill(above_threshold, false, mask);
        return;
      }
      mask.foreach_index([&](const int i) {
        const float energy = sampler->sample(times[i]);
        energies[i] = energy;
        above_threshold[i] = energy > thresholds[i];
      });
      return;
    }

    /* Slow path: per-element channel varies. Bucket by channel to reuse samplers. */
    constexpr int inline_channel_buckets_num = 6; /* covers mono through 5.1 surround */
    std::array<Vector<int>, inline_channel_buckets_num> indices_by_channel;
    MultiValueMap<int, int> indices_with_different_channel;
    Vector<int> indices_with_all_channels;
    mask.foreach_index([&](const int i) {
      if (all_channels_varray[i]) {
        indices_with_all_channels.append(i);
      }
      else {
        const int channel = channels[i];
        if (channel >= 0 && channel < inline_channel_buckets_num) {
          indices_by_channel[channel].append(i);
        }
        else {
          indices_with_different_channel.add(channel, i);
        }
      }
    });

    auto sample_indices_in_channel = [&](const Span<int> indices,
                                         const std::optional<int> channel) {
      if (indices.is_empty()) {
        return;
      }
      bke::bSoundWaveletEnergySampler::Key key;
      key.band = band_;
      key.window_size = window_size_;
      key.channel = channel;
      const bke::bSoundWaveletEnergySampler *sampler = bke::bSoundWaveletEnergySampler::get_cached(
          sound_, key);
      if (!sampler) {
        energies.fill_indices(indices, 0.0f);
        above_threshold.fill_indices(indices, false);
        return;
      }
      for (const int i : indices) {
        const float energy = sampler->sample(times[i]);
        energies[i] = energy;
        above_threshold[i] = energy > thresholds[i];
      }
    };

    sample_indices_in_channel(indices_with_all_channels, std::nullopt);
    for (const int channel : IndexRange(inline_channel_buckets_num)) {
      sample_indices_in_channel(indices_by_channel[channel], channel);
    }
    for (const auto &item : indices_with_different_channel.items()) {
      sample_indices_in_channel(item.value, item.key);
    }
  }
};

static bke::WaveletBand node_band(const bNode &node)
{
  switch (bke::WaveletBand(node.custom1)) {
    case bke::WaveletBand::FullRange:
      return bke::WaveletBand::FullRange;
    case bke::WaveletBand::High:
      return bke::WaveletBand::High;
    case bke::WaveletBand::HighMid:
      return bke::WaveletBand::HighMid;
    case bke::WaveletBand::Mid:
      return bke::WaveletBand::Mid;
    case bke::WaveletBand::LowMid:
      return bke::WaveletBand::LowMid;
    case bke::WaveletBand::Low:
      return bke::WaveletBand::Low;
    default:
      BLI_assert_unreachable();
      return bke::WaveletBand::FullRange;
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(bke::WaveletBand::FullRange);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  bSound *sound = params.extract_input<bSound *>("Sound"_ustr);
  if (!sound) {
    params.set_default_remaining_outputs();
    return;
  }

  const int window_size = int(params.extract_input<WindowSize>("Window Size"_ustr));

  SocketValueVariant times = params.extract_input<SocketValueVariant>("Time"_ustr);
  SocketValueVariant thresholds = params.extract_input<SocketValueVariant>("Threshold"_ustr);
  SocketValueVariant all_channels = params.extract_input<SocketValueVariant>("All Channels"_ustr);
  SocketValueVariant channels = params.extract_input<SocketValueVariant>("Channel"_ustr);

  auto sample_fn = std::make_shared<SampleSoundWaveletEnergyFunction>(
      *sound, node_band(params.node()), window_size);

  SocketValueVariant energies;
  SocketValueVariant above_threshold;
  std::string error_message;
  if (!execute_multi_function_on_value_variant(std::move(sample_fn),
                                               {&times, &thresholds, &all_channels, &channels},
                                               {&energies, &above_threshold},
                                               params.user_data(),
                                               error_message))
  {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, std::move(error_message));
    return;
  }

  params.set_output("Energy"_ustr, std::move(energies));
  params.set_output("Above Threshold"_ustr, std::move(above_threshold));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "band",
                    "Band",
                    "Approximate frequency band for wavelet detail energy. "
                    "Bands depend on sample rate and window size and are not fixed instrument "
                    "detectors",
                    band_items,
                    NOD_inline_enum_accessors(custom1),
                    int(bke::WaveletBand::FullRange));
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSampleSoundWaveletEnergy"_ustr);
  ntype.ui_name = "Sample Sound Wavelet";
  ntype.ui_description = "Sample wavelet detail energy in an approximate frequency band";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.default_width = bke::NodeWidth::_180;
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sample_sound_wavelet_energy_cc
