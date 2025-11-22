/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase_wrapper.hh"

#include "BKE_armature_deform_fields.hh"
#include "BKE_curves.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_mesh.hh"

#include "NOD_geometry_nodes_closure.hh"
#include "NOD_geometry_nodes_closure_eval.hh"
#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_armature_deform_cc {

using bke::armature_deform::PoseChannelDeformGroup;
using bke::armature_deform::SkinningMode;

/* Source of deformation weights. */
enum class WeightSource {
  /* Use vertex groups weights defined in the geometry context. */
  VertexGroups,
  /* Use distance from bone envelopes as weights. */
  Envelope,
  /* Use custom weight and selection fields for each bone. */
  CustomWeights,
};

static const EnumPropertyItem weight_source_items[] = {
    {int(WeightSource::VertexGroups),
     "VERTEX_GROUPS",
     0,
     "Vertex Groups",
     "Use vertex groups weights defined in the geometry context"},
    {int(WeightSource::Envelope),
     "ENVELOPE",
     0,
     "Envelope",
     "Use distance from bone envelopes as weights"},
    {int(WeightSource::CustomWeights),
     "CUSTOM_WEIGHTS",
     0,
     "Custom Weights",
     "Use custom weight and selection fields for each bone"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Object>("Armature Object").optional_label();

  b.add_input<decl::Bool>("Preserve Volume")
      .default_value(false)
      .description(
          "Use dual quaternion deformation to better preserve the initial volume of the geometry");
  b.add_input<decl::Menu>("Weight Source")
      .static_items(weight_source_items)
      .description("Source of deformation weights");
  b.add_input<decl::Closure>("Custom Weights")
      .usage_by_single_menu(int(WeightSource::CustomWeights));

  b.add_input<decl::Float>("Mask").default_value(1.0f).hide_value().field_on_all().description(
      "Influence of the deformation for each point");

  const bNode *node = b.node_or_null();
  if (node == nullptr) {
    return;
  }
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);

  b.add_default_layout();

  switch (data_type) {
    case SOCK_VECTOR:
      b.add_input<decl::Vector>("Position", "Value")
          .implicit_field_on_all(NodeDefaultInputType::NODE_DEFAULT_INPUT_POSITION_FIELD);
      b.add_output<decl::Vector>("Position", "Value").field_on_all().align_with_previous();
      break;
    case SOCK_MATRIX:
      b.add_input<decl::Matrix>("Transform", "Value")
          .field_on_all()
          .description("Local deformation gradient for each point");
      b.add_output<decl::Matrix>("Transform", "Value")
          .field_on_all()
          .description("Local deformation gradient for each point")
          .align_with_previous();
      break;
    default:
      BLI_assert_unreachable();
      break;
  }
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SOCK_VECTOR;
}

/**
 * A multifunction that computes normalized deformation for all bones of an armature.
 *
 * The function has a variable number of inputs based on armature bones:
 *   - Main value input (float3 or float4x4)
 *   - Global influence factor
 *   - Bone 1 weights
 *   - Bone 1 selection
 *   - Bone 2 weights
 *   - Bone 2 selection
 *     [...]
 *   - Bone n weights
 *   - Bone n selection
 *
 *   - Main value output
 *   - Weight output (total weight sum)
 *   - Selection output (all deformed points)
 */
class ArmatureDeformFunction : public mf::MultiFunction {
 private:
  struct DeformGroupInput {
    std::string pose_channel_name;
    std::string weight_input_name;
    std::string selection_input_name;
    int weight_input;
    int selection_input;
  };

  const Object &armature_object_;
  float4x4 target_to_armature_;
  SkinningMode skinning_mode_;
  Array<DeformGroupInput> deform_groups_inputs_;
  int value_input_;
  int value_output_;
  int mask_input_;
  int weight_output_;
  int selection_output_;

  mf::Signature signature_;

 public:
  mf::Signature build_signature(const CPPType &value_type) const
  {
    mf::Signature signature;
    mf::SignatureBuilder builder{"Armature Deform", signature};
    builder.single_input("Value", value_type);
    builder.single_input<float>("Mask");
    for (const DeformGroupInput &group : deform_groups_inputs_) {
      builder.single_input<float>(group.weight_input_name.c_str());
      builder.single_input<bool>(group.selection_input_name.c_str());
    }
    builder.single_output("Value Output", value_type, mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<float>("Weight Output", mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<bool>("Selection Output", mf::ParamFlag::SupportsUnusedOutput);
    return signature;
  }

  ArmatureDeformFunction(const Object &armature_object,
                         const float4x4 &target_to_armature,
                         const SkinningMode skinning_mode,
                         const CPPType &value_type,
                         const Span<StringRef> deform_groups)
      : armature_object_(armature_object),
        target_to_armature_(target_to_armature),
        skinning_mode_(skinning_mode)
  {
    const int groups_num = deform_groups.size();
    value_input_ = 0;
    value_output_ = 2 + 2 * groups_num;
    weight_output_ = 3 + 2 * groups_num;
    selection_output_ = 4 + 2 * groups_num;
    mask_input_ = 1;
    deform_groups_inputs_.reinitialize(groups_num);
    for (const int i : deform_groups.index_range()) {
      DeformGroupInput &input = deform_groups_inputs_[i];
      input.pose_channel_name = deform_groups[i];
      input.weight_input_name = "Group " + std::to_string(i) + " Weight";
      input.selection_input_name = "Group " + std::to_string(i) + " Selection";
      input.weight_input = 2 + 2 * i;
      input.selection_input = 3 + 2 * i;
    }

    signature_ = build_signature(value_type);
    this->set_signature(&signature_);
  }

  const bPoseChannel *find_pose_channel(const StringRefNull name) const
  {
    return static_cast<const bPoseChannel *>(BLI_findstring(
        &armature_object_.pose->chanbase, name.c_str(), offsetof(bPoseChannel, name)));
  }

  PoseChannelDeformGroup get_deform_group_input(const IndexMask &mask,
                                                mf::Params &params,
                                                const int group_index,
                                                IndexMaskMemory &memory) const
  {
    BLI_assert(deform_groups_inputs_.index_range().contains(group_index));
    const DeformGroupInput &group_input = deform_groups_inputs_[group_index];

    VArray<bool> selection = params.readonly_single_input<bool>(group_input.selection_input);
    VArray<float> weights = params.readonly_single_input<float>(group_input.weight_input);
    IndexMask group_mask = IndexMask::from_bools(mask, selection, memory);
    Array<float> weights_buffer(group_mask.size());
    weights.materialize_compressed_to_uninitialized(group_mask, weights_buffer);

    PoseChannelDeformGroup pchan_group;
    pchan_group.pose_channel = find_pose_channel(group_input.pose_channel_name);
    pchan_group.deform_group.mask = std::move(group_mask);
    pchan_group.deform_group.weights = std::move(weights_buffer);
    return pchan_group;
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context context) const override
  {
    const auto *user_data = dynamic_cast<const GeoNodesUserData *>(context.user_data);
    const Object *self_object = (user_data != nullptr) ? user_data->call_data->self_object() :
                                                         nullptr;
    const float4x4 target_to_armature = self_object ?
                                            target_to_armature_ * self_object->object_to_world() :
                                            target_to_armature_;
    IndexMaskMemory memory;

    if (!params.single_output_is_required(value_output_)) {
      return;
    }

    const GVArray &value = params.readonly_single_input(0, "Value");
    const VArraySpan<float> vert_influence = params.readonly_single_input<float>(1, "Mask");
    Array<PoseChannelDeformGroup> deform_groups(deform_groups_inputs_.size());
    for (const int i : deform_groups.index_range()) {
      deform_groups[i] = get_deform_group_input(mask, params, i, memory);
    }

    if (&value.type() == &CPPType::get<float3>()) {
      MutableSpan<float3> value_output = params.uninitialized_single_output_if_required<float3>(
          value_output_);
      if (!value_output.is_empty()) {
        value.typed<float3>().materialize_to_uninitialized(mask, value_output);
        bke::armature_deform::deform_positions(
            target_to_armature, mask, vert_influence, deform_groups, skinning_mode_, value_output);
      }
    }
    else if (&value.type() == &CPPType::get<float4x4>()) {
      MutableSpan<float4x4> value_output =
          params.uninitialized_single_output_if_required<float4x4>(value_output_);
      if (!value_output.is_empty()) {
        value.typed<float4x4>().materialize_to_uninitialized(mask, value_output);
        bke::armature_deform::deform_matrices(
            target_to_armature, mask, vert_influence, deform_groups, skinning_mode_, value_output);
      }
    }
    else {
      /* Unsupported field type for armature deformation. */
      BLI_assert_unreachable();
    }

    if (params.single_output_is_required(weight_output_)) {
      MutableSpan<float> weights_output = params.uninitialized_single_output_if_required<float>(
          weight_output_);
      mask.foreach_index(GrainSize(4096), [&](const int index) { weights_output[index] = 0.0f; });
      for (const PoseChannelDeformGroup &group : deform_groups) {
        group.deform_group.mask.foreach_index(
            GrainSize(4096), [&](const int index, const int pos) {
              weights_output[index] += group.deform_group.weights[pos];
            });
      }
    }

    if (params.single_output_is_required(selection_output_)) {
      MutableSpan<bool> bools_output = params.uninitialized_single_output_if_required<bool>(
          selection_output_);
      mask.foreach_index(GrainSize(4096), [&](const int index) { bools_output[index] = false; });
      for (const PoseChannelDeformGroup &group : deform_groups) {
        group.deform_group.mask.foreach_index(
            GrainSize(4096), [&](const int index) { bools_output[index] = true; });
      }
    }
  }

  uint64_t hash() const override
  {
    uint64_t hash = get_default_hash(&armature_object_, target_to_armature_, skinning_mode_);
    for (const DeformGroupInput &group : deform_groups_inputs_) {
      hash = get_default_hash(hash, group.pose_channel_name);
    }
    return hash;
  }

  bool equals(const MultiFunction &other) const override
  {
    const auto *other_deform = dynamic_cast<const ArmatureDeformFunction *>(&other);
    if (!other_deform) {
      return false;
    }
    if (deform_groups_inputs_.size() != other_deform->deform_groups_inputs_.size()) {
      return false;
    }
    for (const int i : deform_groups_inputs_.index_range()) {
      const DeformGroupInput &group = deform_groups_inputs_[i];
      const DeformGroupInput &other_group = other_deform->deform_groups_inputs_[i];
      if (group.pose_channel_name != other_group.pose_channel_name) {
        return false;
      }
    }
    if (&armature_object_ != &other_deform->armature_object_ ||
        target_to_armature_ != other_deform->target_to_armature_ ||
        skinning_mode_ != other_deform->skinning_mode_)
    {
      return false;
    }

    return true;
  }
};

struct DeformGroupFields {
  const bPoseChannel *pose_channel;
  Field<float> weights_field;
  Field<bool> selection_field;
};

static Vector<DeformGroupFields> build_vertex_group_deform_fields(
    const Object &armature_object,
    const float4x4 &target_to_world,
    const std::optional<float> threshold_weight,
    const bool use_envelope_multiply,
    Field<float3> position_field)
{
  const ConstListBaseWrapper<bPoseChannel> pose_channels(armature_object.pose->chanbase);
  const int pose_channels_num = BLI_listbase_count(&armature_object.pose->chanbase);
  const float4x4 target_to_armature = armature_object.world_to_object() * target_to_world;

  Vector<DeformGroupFields> vertex_group_fields;
  vertex_group_fields.reserve(pose_channels_num);

  for (const bPoseChannel *pose_channel : pose_channels) {
    if (!pose_channel->bone || bool(pose_channel->bone->flag & BONE_NO_DEFORM)) {
      continue;
    }

    const Bone &bone = *pose_channel->bone;
    const float threshold = threshold_weight ? std::max(*threshold_weight, 0.0f) : 0.0f;
    Field<float> weight_field(
        std::make_shared<bke::armature_deform::VertexGroupWeightInput>(bone));

    /* Bone feature: multiply with envelope weight. */
    if (use_envelope_multiply && (bone.flag & BONE_MULT_VG_ENV)) {
      const Field<float> envelope_field = Field<float>(
          FieldOperation::from(std::make_shared<bke::armature_deform::BoneEnvelopeMultiFunction>(
                                   target_to_armature, bone, threshold),
                               {position_field}),
          0);
      static auto multiply_envelope_fn = mf::build::SI2_SO<float, float, float>(
          "Multiply Bone Envelope",
          [&](const float weight, const float envelope) -> float { return weight * envelope; });
      weight_field = Field<float>(
          FieldOperation::from(multiply_envelope_fn, {weight_field, envelope_field}));
    }

    Field<bool> selection_field(
        std::make_shared<bke::armature_deform::VertexGroupSelectionInput>(bone, threshold));
    vertex_group_fields.append_unchecked(
        {pose_channel, std::move(weight_field), std::move(selection_field)});
  }

  return vertex_group_fields;
}

static Vector<DeformGroupFields> build_envelope_deform_fields(
    const Object &armature_object,
    const float4x4 &target_to_world,
    const std::optional<float> threshold_weight,
    Field<float3> position_field)
{
  const ConstListBaseWrapper<bPoseChannel> pose_channels(armature_object.pose->chanbase);
  const int pose_channels_num = BLI_listbase_count(&armature_object.pose->chanbase);
  const float4x4 target_to_armature = armature_object.world_to_object() * target_to_world;

  Vector<DeformGroupFields> envelope_group_fields;
  envelope_group_fields.reserve(pose_channels_num);

  for (const bPoseChannel *pose_channel : pose_channels) {
    if (!pose_channel->bone || bool(pose_channel->bone->flag & BONE_NO_DEFORM)) {
      continue;
    }

    const Bone &bone = *pose_channel->bone;
    const float threshold = threshold_weight ? std::max(*threshold_weight, 0.0f) : 0.0f;
    auto bone_envelope_op = FieldOperation::from(
        std::make_shared<bke::armature_deform::BoneEnvelopeMultiFunction>(
            target_to_armature, bone, threshold),
        {position_field});
    Field<float> weight_field(bone_envelope_op, 0);
    Field<bool> selection_field(bone_envelope_op, 1);
    envelope_group_fields.append_unchecked(
        {pose_channel, std::move(weight_field), std::move(selection_field)});
  }

  return envelope_group_fields;
}

static Vector<DeformGroupFields> build_custom_deform_fields(
    const Object &armature_object,
    GeoNodesUserData *user_data,
    const ClosurePtr &custom_groups_closure)
{
  if (!custom_groups_closure) {
    return {};
  }

  const ConstListBaseWrapper<bPoseChannel> pose_channels(armature_object.pose->chanbase);
  const int pose_channels_num = BLI_listbase_count(&armature_object.pose->chanbase);
  const bke::bNodeSocketType *stype_string = bke::node_socket_type_find_static(SOCK_STRING);
  const bke::bNodeSocketType *stype_float = bke::node_socket_type_find_static(SOCK_FLOAT);
  const bke::bNodeSocketType *stype_bool = bke::node_socket_type_find_static(SOCK_BOOLEAN);

  Vector<DeformGroupFields> custom_group_fields;
  custom_group_fields.reserve(pose_channels_num);

  for (const bPoseChannel *pose_channel : pose_channels) {
    if (!pose_channel->bone || bool(pose_channel->bone->flag & BONE_NO_DEFORM)) {
      continue;
    }

    SocketValueVariant bone_variant(std::string(pose_channel->name));
    SocketValueVariant weights_variant;
    SocketValueVariant selection_variant;
    ClosureEagerEvalParams eval_params = {
        {{"Bone", stype_string, bone_variant}},
        {{"Weight", stype_float, &weights_variant}, {"Selection", stype_bool, &selection_variant}},
        user_data};
    evaluate_closure_eagerly(*custom_groups_closure, eval_params);

    Field<float> weights_field = weights_variant.extract<Field<float>>();
    Field<bool> selection_field = selection_variant.extract<Field<bool>>();
    custom_group_fields.append_unchecked(
        {pose_channel, std::move(weights_field), std::move(selection_field)});
  }
  return custom_group_fields;
}

static Field<float3> position_from_value_field(const GField &value_field)
{
  Field<float3> position_field;
  if (value_field.cpp_type() == CPPType::get<float3>()) {
    position_field = value_field;
  }
  else if (value_field.cpp_type() == CPPType::get<float4x4>()) {
    static const auto matrix_location_fn = mf::build::SI1_SO<float4x4, float3>(
        "matrix_location", [](const float4x4 &matrix) -> float3 { return matrix.location(); });
    position_field = Field<float3>(FieldOperation::from(matrix_location_fn, {value_field}));
  }
  else {
    BLI_assert_unreachable();
  }
  return position_field;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GField value_field = params.extract_input<GField>("Value");

  const Object *armature_object = params.extract_input<Object *>("Armature Object");
  if (!armature_object || armature_object->type != OB_ARMATURE) {
    params.set_output("Value", value_field);
    return;
  }
  const Object *self_object = params.self_object();
  BLI_assert(self_object != nullptr);
  const float4x4 &target_to_world = self_object->object_to_world();

  const SkinningMode skinning_mode = params.extract_input<bool>("Preserve Volume") ?
                                         SkinningMode::DualQuaternion :
                                         SkinningMode::Linear;
  const WeightSource weight_source = params.extract_input<WeightSource>("Weight Source");
  /* TODO could be an input option (always enabled in legacy armature deform). */
  const bool use_envelope_multiply = true;
  const std::optional<float> threshold_weight = std::nullopt;

  Vector<DeformGroupFields> deform_group_fields;
  switch (weight_source) {
    case WeightSource::VertexGroups: {
      Field<float3> position_field = position_from_value_field(value_field);
      deform_group_fields = build_vertex_group_deform_fields(*armature_object,
                                                             target_to_world,
                                                             threshold_weight,
                                                             use_envelope_multiply,
                                                             position_field);
      break;
    }
    case WeightSource::Envelope: {
      Field<float3> position_field = position_from_value_field(value_field);
      deform_group_fields = build_envelope_deform_fields(
          *armature_object, target_to_world, threshold_weight, position_field);
      break;
    }
    case WeightSource::CustomWeights: {
      const ClosurePtr custom_weights = params.extract_input<ClosurePtr>("Custom Weights");
      deform_group_fields = build_custom_deform_fields(
          *armature_object, params.user_data(), custom_weights);
      break;
    }
  }

  Field<float> mask_field = params.extract_input<Field<float>>("Mask");

  Array<StringRef> deform_groups_bone_names(deform_group_fields.size());
  for (const int i : deform_group_fields.index_range()) {
    deform_groups_bone_names[i] = deform_group_fields[i].pose_channel->name;
  }
  Vector<GField> input_fields;
  input_fields.reserve(3 + 2 * deform_group_fields.size());
  input_fields.append_unchecked(value_field);
  input_fields.append_unchecked(mask_field);
  for (const int i : deform_group_fields.index_range()) {
    input_fields.append_unchecked(deform_group_fields[i].weights_field);
    input_fields.append_unchecked(deform_group_fields[i].selection_field);
  }
  auto deform_op = FieldOperation::from(
      std::make_unique<ArmatureDeformFunction>(*armature_object,
                                               target_to_world,
                                               skinning_mode,
                                               value_field.cpp_type(),
                                               deform_groups_bone_names),
      input_fields);
  params.set_output<GField>("Value", GField(deform_op, 0));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "data_type",
      "Data Type",
      "Type of grid data",
      rna_enum_node_socket_data_type_items,
      NOD_inline_enum_accessors(custom1),
      SOCK_FLOAT,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(rna_enum_node_socket_data_type_items,
                                 [](const EnumPropertyItem &item) -> bool {
                                   return ELEM(item.value, SOCK_VECTOR, SOCK_MATRIX);
                                 });
      });
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeArmatureDeform");
  ntype.ui_name = "Armature Deform";
  ntype.ui_description = "Deformation using armature bones";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_armature_deform_cc
