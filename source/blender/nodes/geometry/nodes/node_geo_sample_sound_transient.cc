/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_sound_sample.hh"

#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_sound_transient_cc {

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
    {int(bke::TransientBand::FullRange),
     "FULL_RANGE",
     0,
     "Full Range",
     "Sum transient energy across all useful wavelet detail levels"},
    {int(bke::TransientBand::High),
     "HIGH",
     0,
     "High",
     "Sample the highest-frequency wavelet detail level"},
    {int(bke::TransientBand::HighMid),
     "HIGH_MID",
     0,
     "High-Mid",
     "Sample an early wavelet detail level"},
    {int(bke::TransientBand::Mid), "MID", 0, "Mid", "Sample a middle wavelet detail level"},
    {int(bke::TransientBand::LowMid),
     "LOW_MID",
     0,
     "Low-Mid",
     "Sample a later wavelet detail level"},
    {int(bke::TransientBand::Low),
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
      .description("Wavelet-based transient energy at the given time in the selected band")
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Bool>("Is Transient"_ustr)
      .propagate_references()
      .description("True when the transient energy exceeds the threshold")
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Sound>("Sound"_ustr).optional_label().description("Sound to analyze");
  b.add_input<decl::Float>("Time"_ustr)
      .subtype(PROP_TIME_ABSOLUTE)
      .structure_type(StructureType::Dynamic)
      .description("Time in seconds of the sound to sample at");
  b.add_input<decl::Float>("Threshold"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .structure_type(StructureType::Dynamic)
      .description("Energy level above which a transient is detected");
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
                  .description("Configure the transient analysis");
    p.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr) {
      layout.prop(ptr, "band", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    });
    p.add_input<decl::Menu>("Window Size"_ustr)
        .static_items(window_size_items)
        .default_value(WindowSize::_2048)
        .optional_label()
        .description(
            "Number of samples per DWT window. Larger values capture lower-frequency transients "
            "but smear the time localization");
  }
}

class SampleSoundTransientFunction : public mf::MultiFunction {
 private:
  const bSound &sound_;
  const bke::TransientBand band_;
  const int window_size_;

 public:
  SampleSoundTransientFunction(bSound &sound, const bke::TransientBand band, const int window_size)
      : sound_(sound), band_(band), window_size_(window_size)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder("Sample Sound Transient", signature);
      builder.single_input<float>("Time");
      builder.single_input<float>("Threshold");
      builder.single_input<bool>("All Channels");
      builder.single_input<int>("Channel");
      builder.single_output<float>("Energy");
      builder.single_output<bool>("Is Transient");
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
    MutableSpan<bool> is_transient = params.uninitialized_single_output<bool>(5, "Is Transient");

    const std::optional<bool> all_channels_value = all_channels_varray.get_if_single();
    const std::optional<int> channel_value = channels.get_if_single();
    const bool constant_channel = all_channels_value == true ||
                                  (all_channels_value.has_value() && channel_value.has_value());

    /* Fast path: every element samples the same channel, so one sampler lookup suffices. */
    if (constant_channel) {
      bke::bSoundTransientSampler::Key key;
      key.band = band_;
      key.window_size = window_size_;
      key.channel = *all_channels_value ? std::nullopt : channel_value;
      const bke::bSoundTransientSampler *sampler = bke::bSoundTransientSampler::get_cached(sound_,
                                                                                           key);
      if (!sampler) {
        index_mask::masked_fill(energies, 0.0f, mask);
        index_mask::masked_fill(is_transient, false, mask);
        return;
      }
      mask.foreach_index([&](const int i) {
        const float energy = sampler->sample(times[i]);
        energies[i] = energy;
        is_transient[i] = energy > thresholds[i];
      });
      return;
    }

    /* Slow path: per-element channel varies. Bucket by channel to reuse samplers. */
    constexpr int channel_array_size = 6;
    std::array<Vector<int>, channel_array_size> indices_by_channel;
    MultiValueMap<int, int> indices_with_different_channel;
    Vector<int> indices_with_all_channels;
    mask.foreach_index([&](const int i) {
      if (all_channels_varray[i]) {
        indices_with_all_channels.append(i);
      }
      else {
        const int channel = channels[i];
        if (channel >= 0 && channel < channel_array_size) {
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
      bke::bSoundTransientSampler::Key key;
      key.band = band_;
      key.window_size = window_size_;
      key.channel = channel;
      const bke::bSoundTransientSampler *sampler = bke::bSoundTransientSampler::get_cached(sound_,
                                                                                           key);
      if (!sampler) {
        energies.fill_indices(indices, 0.0f);
        is_transient.fill_indices(indices, false);
        return;
      }
      for (const int i : indices) {
        const float energy = sampler->sample(times[i]);
        energies[i] = energy;
        is_transient[i] = energy > thresholds[i];
      }
    };

    sample_indices_in_channel(indices_with_all_channels, std::nullopt);
    for (const int channel : IndexRange(channel_array_size)) {
      sample_indices_in_channel(indices_by_channel[channel], channel);
    }
    for (const auto item : indices_with_different_channel.items()) {
      sample_indices_in_channel(item.value, item.key);
    }
  }
};

static int to_window_size_int(const WindowSize window_size)
{
  switch (window_size) {
    case WindowSize::_256:
      return 256;
    case WindowSize::_512:
      return 512;
    case WindowSize::_1024:
      return 1024;
    case WindowSize::_2048:
      return 2048;
    case WindowSize::_4096:
      return 4096;
    case WindowSize::_8192:
      return 8192;
  }
  return 2048;
}

static bke::TransientBand node_band(const bNode &node)
{
  switch (bke::TransientBand(node.custom1)) {
    case bke::TransientBand::FullRange:
    case bke::TransientBand::High:
    case bke::TransientBand::HighMid:
    case bke::TransientBand::Mid:
    case bke::TransientBand::LowMid:
    case bke::TransientBand::Low:
      return bke::TransientBand(node.custom1);
  }
  return bke::TransientBand::FullRange;
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(bke::TransientBand::FullRange);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  bSound *sound = params.extract_input<bSound *>("Sound"_ustr);
  if (!sound) {
    params.set_default_remaining_outputs();
    return;
  }

  const WindowSize window_size = params.extract_input<WindowSize>("Window Size"_ustr);

  SocketValueVariant times = params.extract_input<SocketValueVariant>("Time"_ustr);
  SocketValueVariant thresholds = params.extract_input<SocketValueVariant>("Threshold"_ustr);
  SocketValueVariant all_channels = params.extract_input<SocketValueVariant>("All Channels"_ustr);
  SocketValueVariant channels = params.extract_input<SocketValueVariant>("Channel"_ustr);

  auto sample_fn = std::make_shared<SampleSoundTransientFunction>(
      *sound, node_band(params.node()), to_window_size_int(window_size));

  SocketValueVariant energies;
  SocketValueVariant transients;
  std::string error_message;
  if (!execute_multi_function_on_value_variant(std::move(sample_fn),
                                               {&times, &thresholds, &all_channels, &channels},
                                               {&energies, &transients},
                                               params.user_data(),
                                               error_message))
  {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, std::move(error_message));
    return;
  }

  params.set_output("Energy"_ustr, std::move(energies));
  params.set_output("Is Transient"_ustr, std::move(transients));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "band",
                    "Band",
                    "Approximate frequency band used for wavelet-based transient energy",
                    band_items,
                    NOD_inline_enum_accessors(custom1),
                    int(bke::TransientBand::FullRange));
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSampleSoundTransient"_ustr);
  ntype.ui_name = "Sample Sound Transient";
  ntype.ui_description = "Sample wavelet-based transient energy in an approximate frequency band";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.default_width = bke::NodeWidth::_180;
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sample_sound_transient_cc
