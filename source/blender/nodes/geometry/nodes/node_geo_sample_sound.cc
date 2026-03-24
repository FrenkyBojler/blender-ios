/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_sound.hh"

#include "NOD_socket_usage_inference.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_sound_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Sound>("Sound").optional_label();
  b.add_input<decl::Float>("Time")
      .subtype(PROP_TIME_ABSOLUTE)
      .supports_field()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Bool>("All Channels")
      .default_value(true)
      .supports_field()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Int>("Channel")
      .min(0)
      .usage_inference(
          [](const socket_usage_inference::SocketUsageParams &params) -> std::optional<bool> {
            if (const std::optional<bool> any_output_used = params.any_output_is_used()) {
              if (!*any_output_used) {
                return false;
              }
            }
            else {
              return std::nullopt;
            }
            const std::optional<bool> all_channels =
                params.get_input("All Channels").get_if_primitive<bool>();
            if (!all_channels.has_value()) {
              return true;
            }
            return !*all_channels;
          })
      .supports_field()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Low")
      .subtype(PROP_FREQUENCY)
      .default_value(0.0f)
      .min(0.0f)
      .supports_field()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("High")
      .subtype(PROP_FREQUENCY)
      .default_value(10'000.0f)
      .min(0.0f)
      .supports_field()
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Amplitude").reference_pass_all();
}

class SampleSoundFunction : public mf::MultiFunction {
 private:
  const bSound &sound_;

 public:
  SampleSoundFunction(bSound &sound) : sound_(sound)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder("Sample Sound", signature);
      builder.single_input<float>("Time");
      builder.single_input<bool>("All Channels");
      builder.single_input<int>("Channel");
      builder.single_input<float>("Low");
      builder.single_input<float>("High");
      builder.single_output<float>("Amplitude");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float> &times = params.readonly_single_input<float>(0, "Time");
    const VArray<bool> &all_channels_varray = params.readonly_single_input<bool>(1,
                                                                                 "All Channels");
    const VArray<int> &channels = params.readonly_single_input<int>(2, "Channel");
    const VArray<float> &lows = params.readonly_single_input<float>(3, "Low");
    const VArray<float> &highs = params.readonly_single_input<float>(4, "High");
    MutableSpan<float> amplitudes = params.uninitialized_single_output<float>(5, "Amplitude");

    const std::optional<bool> all_channels_value = all_channels_varray.get_if_single();
    const std::optional<float> channel_value = channels.get_if_single();
    const bool constant_channel = all_channels_value == true ||
                                  (all_channels_value.has_value() && channel_value.has_value());
    if (constant_channel) {
      bke::SampleSoundKey key;
      key.window = bke::SampleSoundWindow::Rectangular;
      key.fft_size = 2048;
      key.channel = *all_channels_value ? std::nullopt : channel_value;

      bke::SoundSampler *sampler = bke::sound_sampler_get(sound_, key);
      if (!sampler) {
        index_mask::masked_fill(amplitudes, 0.0f, mask);
        return;
      }
      mask.foreach_index([&](const int i) {
        const float time = times[i];
        const float low = lows[i];
        const float high = highs[i];
        const float amplitude = sampler->sample_single(time, low, high);
        amplitudes[i] = amplitude;
      });
      delete sampler;
      return;
    }

    mask.foreach_index([&](const int i) {
      const float time = times[i];
      const bool all_channels = all_channels_varray[i];
      const int channel = channels[i];
      const float low = lows[i];
      const float high = highs[i];

      bke::SampleSoundKey key;
      key.window = bke::SampleSoundWindow::Rectangular;
      key.fft_size = 2048;
      key.channel = all_channels ? std::nullopt : std::make_optional(channel);

      bke::SoundSampler *sampler = bke::sound_sampler_get(sound_, key);
      if (!sampler) {
        amplitudes[i] = 0.0f;
        return;
      }
      const float amplitude = sampler->sample_single(time, low, high);
      amplitudes[i] = amplitude;
      delete sampler;
    });
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  bSound *sound = params.extract_input<bSound *>("Sound");
  if (!sound) {
    params.set_default_remaining_outputs();
    return;
  }

  SocketValueVariant times = params.extract_input<SocketValueVariant>("Time");
  SocketValueVariant all_channels = params.extract_input<SocketValueVariant>("All Channels");
  SocketValueVariant channels = params.extract_input<SocketValueVariant>("Channel");
  SocketValueVariant lows = params.extract_input<SocketValueVariant>("Low");
  SocketValueVariant highs = params.extract_input<SocketValueVariant>("High");

  auto sample_fn = std::make_shared<SampleSoundFunction>(*sound);

  SocketValueVariant amplitudes;
  std::string error_message;
  if (!execute_multi_function_on_value_variant(std::move(sample_fn),
                                               {&times, &all_channels, &channels, &lows, &highs},
                                               {&amplitudes},
                                               params.user_data(),
                                               error_message))
  {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, std::move(error_message));
    return;
  }

  params.set_output("Amplitude", std::move(amplitudes));
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSampleSound");
  ntype.ui_name = "Sample Sound";
  ntype.ui_description = "";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sample_sound_cc
