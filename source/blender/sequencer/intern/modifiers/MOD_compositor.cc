/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "BLI_math_rotation.hh"

#include "BLT_translation.hh"

#include "COM_domain.hh"
#include "COM_realize_on_domain_operation.hh"
#include "COM_result.hh"

#include "DNA_node_types.h"
#include "DNA_sequence_types.h"

#include "BKE_anim_data.hh"
#include "BKE_animsys.h"
#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "DEG_depsgraph_query.hh"

#include "IMB_colormanagement.hh"

#include "NOD_composite.hh"
#include "NOD_compositor_nodes_caller_ui.hh"
#include "NOD_compositor_nodes_srna.hh"

#include "SEQ_modifier.hh"
#include "SEQ_select.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_transform.hh"

#include "UI_interface.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "cache/compositor_cache.hh"
#include "compositor.hh"
#include "modifier.hh"
#include "render.hh"

namespace blender::seq {

void compositor_nodes_update_interface(Scene &sequencer_scene,
                                       SequencerCompositorModifierData &cmd)
{
  if (!cmd.modifier.system_properties) {
    cmd.modifier.system_properties =
        bke::idprop::create_group("SequencerCompositorModifierProperties").release();
  }
  PointerRNA properties_ptr = RNA_pointer_create_discrete(
      &sequencer_scene.id, RNA_SequencerCompositorModifierProperties, &cmd);
  RNA_sync_system_properties(properties_ptr, *cmd.modifier.system_properties);

  DEG_id_tag_update(&sequencer_scene.id, ID_RECALC_SEQUENCER_STRIPS);
}

static void set_single_input_from_rna_value(PointerRNA *input_props_ptr,
                                            const eNodeSocketDatatype socket_type,
                                            compositor::Result &result)
{
  using namespace nodes;
  switch (socket_type) {
    case SOCK_FLOAT: {
      const auto type = CompositorNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == CompositorNodesInputType::Value) {
        const float value = RNA_float_get(input_props_ptr, "value");
        result.set_single_value(value);
      }
      break;
    }
    case SOCK_VECTOR: {
      const auto type = CompositorNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == CompositorNodesInputType::Value) {
        float3 value;
        RNA_float_get_array(input_props_ptr, "value", value);
        result.set_single_value(value);
      }
      break;
    }
    case SOCK_RGBA: {
      const auto type = CompositorNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == CompositorNodesInputType::Value) {
        ColorGeometry4f value;
        RNA_float_get_array(input_props_ptr, "value", value);
        result.set_single_value(value);
      }
      break;
    }
    case SOCK_BOOLEAN: {
      const auto type = CompositorNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == CompositorNodesInputType::Value) {
        const bool value = RNA_boolean_get(input_props_ptr, "value");
        result.set_single_value(value);
      }
      break;
    }
    case SOCK_INT: {
      const auto type = CompositorNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == CompositorNodesInputType::Value) {
        const int value = RNA_int_get(input_props_ptr, "value");
        result.set_single_value(value);
      }
      break;
    }
    case SOCK_ROTATION: {
      const auto type = CompositorNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == CompositorNodesInputType::Value) {
        float3 value_euler;
        RNA_float_get_array(input_props_ptr, "value", value_euler);
        math::Quaternion value_rotation = math::to_quaternion(math::EulerXYZ(value_euler));
        result.set_single_value(
            float4(value_rotation.x, value_rotation.y, value_rotation.z, value_rotation.w));
      }
      break;
    }
    case SOCK_MENU: {
      const auto type = CompositorNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == CompositorNodesInputType::Value) {
        const MenuValue value = MenuValue(RNA_enum_get(input_props_ptr, "value"));
        result.set_single_value(value);
      }
      break;
    }
    case SOCK_STRING: {
      const auto type = CompositorNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == CompositorNodesInputType::Value) {
        const std::string value = RNA_string_get(input_props_ptr, "value");
        result.set_single_value(value);
      }
      break;
    }
    case SOCK_INT_VECTOR: {
      const auto type = CompositorNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == CompositorNodesInputType::Value) {
        int3 value;
        RNA_int_get_array(input_props_ptr, "value", value);
        result.set_single_value(value);
      }
      break;
    }
    case SOCK_OBJECT: {
      const auto type = CompositorNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == CompositorNodesInputType::Value) {
        Object *value = RNA_pointer_get(input_props_ptr, "value").data_as<Object>();
        result.set_single_value(value);
      }
      break;
    }
    case SOCK_IMAGE:
    case SOCK_COLLECTION:
    case SOCK_TEXTURE:
    case SOCK_MATERIAL:
    case SOCK_FONT:
    case SOCK_SCENE:
    case SOCK_TEXT_ID:
    case SOCK_MASK:
    case SOCK_SOUND:
    case SOCK_GEOMETRY:
    case SOCK_MATRIX:
    case SOCK_BUNDLE:
    case SOCK_CLOSURE:
    case SOCK_SHADER:
    case SOCK_CUSTOM:
      break;
  }
}

class CompositorModifierContext : public CompositorContext {
 private:
  const ModifierApplyContext &mod_context_;
  SequencerCompositorModifierData *modifier_data_;

  ImBuf *image_buffer_;
  compositor::Result mask_;
  float3x3 mask_transform_;
  ImBuf *mask_buffer_ = nullptr;
  int timeline_frame_;
  bool owns_mask_ = false;
  PointerRNA properties_ptr_;

 public:
  CompositorModifierContext(const ModifierApplyContext &mod_context,
                            int timeline_frame,
                            compositor::StaticCacheManager &cache_manager,
                            SequencerCompositorModifierData *modifier_data)
      : CompositorContext(cache_manager, mod_context.render_data, mod_context.strip),
        mod_context_(mod_context),
        modifier_data_(modifier_data),
        image_buffer_(mod_context.image),
        mask_(*this, compositor::ResultType::Color, compositor::ResultPrecision::Full),
        timeline_frame_(timeline_frame)
  {
    /* Masks are in screen space, whereas modifier executes in strip space. */
    mask_transform_ = math::invert(
        image_transform_matrix_get(mod_context.render_data.scene, &mod_context.strip));

    PointerRNA ptr = RNA_pointer_create_discrete(
        const_cast<ID *>(&mod_context.render_data.scene->id),
        RNA_SequencerCompositorModifierData,
        modifier_data);
    properties_ptr_ = RNA_pointer_get(&ptr, "properties");
  }

  ~CompositorModifierContext()
  {
    if (this->mask_buffer_ != nullptr) {
      IMB_freeImBuf(this->mask_buffer_);
    }
    if (this->owns_mask_) {
      this->mask_.release();
    }
  }

  compositor::Domain get_compositing_domain() const override
  {
    return compositor::Domain(int2(image_buffer_->x, image_buffer_->y));
  }

  void write_viewer(compositor::Result &viewer_result) override
  {
    using namespace compositor;

    /* Realize the transforms if needed. */
    const InputDescriptor input_descriptor = {ResultType::Color,
                                              InputRealizationMode::OperationDomain};
    SimpleOperation *realization_operation = RealizeOnDomainOperation::construct_if_needed(
        *this, viewer_result, input_descriptor, viewer_result.domain());

    if (realization_operation) {
      Result realize_input = this->create_result(ResultType::Color, viewer_result.precision());
      realize_input.wrap_external(viewer_result);
      realization_operation->map_input_to_result(&realize_input);
      realization_operation->evaluate();

      Result &realized_viewer_result = realization_operation->get_result();
      this->write_output(realized_viewer_result, *image_buffer_);
      realized_viewer_result.release();
      viewer_was_written_ = true;
      delete realization_operation;
      return;
    }

    this->write_output(viewer_result, *image_buffer_);
    viewer_was_written_ = true;
  }

  void evaluate()
  {
    using namespace compositor;
    const bNodeTree &node_group = *DEG_get_evaluated<bNodeTree>(render_data_.depsgraph,
                                                                modifier_data_->node_group);
    NodeGroupOperation node_group_operation(*this,
                                            node_group,
                                            this->needed_outputs(),
                                            nullptr,
                                            node_group.active_viewer_key,
                                            bke::NODE_INSTANCE_KEY_BASE);
    set_output_refcount(node_group, node_group_operation);

    node_group.ensure_topology_cache();
    PointerRNA inputs_ptr = RNA_pointer_get(&properties_ptr_, "inputs");
    BLI_assert(inputs_ptr.data != nullptr);

    /* Map the inputs to the operation. */
    Vector<std::unique_ptr<Result>> inputs;
    bool found_image_input = false;
    bool found_mask_input = false;
    for (const bNodeTreeInterfaceSocket *input_socket : node_group.interface_inputs()) {
      const bke::bNodeSocketType *typeinfo = input_socket->socket_typeinfo();
      const eNodeSocketDatatype socket_type = typeinfo ? typeinfo->type : SOCK_CUSTOM;

      PointerRNA input_props_ptr = RNA_pointer_get(&inputs_ptr, input_socket->identifier);
      const std::optional<ResultType> result_type = Result::from_socket_data_type(socket_type);
      Result *input_result = new Result(
          this->create_result(result_type.value_or(ResultType::Color), ResultPrecision::Full));
      if (result_type) {
        if (!found_image_input && socket_type == SOCK_RGBA) {
          /* First color socket is the image input. */
          create_result_from_input(*input_result, *image_buffer_);
          found_image_input = true;
        }
        else if (!found_mask_input && socket_type == SOCK_RGBA) {
          /* Second socket is the mask input. */
          render_mask_input(this->mod_context_, this->timeline_frame_);
          if (this->mask_.is_allocated()) {
            input_result->set_type(this->mask_.type());
            input_result->set_precision(this->mask_.precision());
            input_result->wrap_external(this->mask_);
            input_result->set_transformation(this->mask_transform_);
          }
          else {
            input_result->allocate_invalid();
          }
          found_mask_input = true;
        }
        else {
          PointerRNA input_props_ptr = RNA_pointer_get(&inputs_ptr, input_socket->identifier);
          input_result->allocate_single_value();
          set_single_input_from_rna_value(&input_props_ptr, socket_type, *input_result);
        }
      }
      else {
        /* The rest of the sockets are not supported. */
        input_result->allocate_invalid();
      }

      node_group_operation.map_input_to_result(input_socket->identifier, input_result);
      inputs.append(std::unique_ptr<Result>(input_result));
    }

    node_group_operation.evaluate();
    this->write_outputs(node_group, node_group_operation, *this->image_buffer_);
  }

  /* Render mask - similar to #modifier_render_mask_input except for the Mask ID
   * path we do a more efficient approach than rendering into a full ImBuf. */
  void render_mask_input(const ModifierApplyContext &context, int timeline_frame)
  {
    const StripModifierData &smd = this->modifier_data_->modifier;
    if (smd.mask_input_type == STRIP_MASK_INPUT_STRIP && smd.mask_strip) {
      this->mask_buffer_ = seq_render_strip(
          &context.render_data, &context.render_state, smd.mask_strip, timeline_frame);
      if (this->mask_buffer_ != nullptr) {
        ensure_ibuf_is_linear_space(this->mask_buffer_, true);
        this->create_result_from_input(this->mask_, *this->mask_buffer_);
        this->owns_mask_ = true;
      }
    }
    else if (smd.mask_input_type == STRIP_MASK_INPUT_ID && smd.mask_id) {
      int frame_index = 0;
      if (smd.mask_time == STRIP_MASK_TIME_RELATIVE) {
        frame_index = smd.mask_id->sfra + timeline_frame - context.strip.start;
      }
      else if (smd.mask_time == STRIP_MASK_TIME_ABSOLUTE) {
        frame_index = timeline_frame;
      }

      /* Mask is a grayscale value, similar to alpha, so conceptually it is already a
       * "linear" quantity. However, masks used to be turned into grayscale images and
       * interpreted as being in "sequencer working space" (default: sRGB), so keep at least
       * that behavior working as before -- if sequencer space is sRGB, convert value to
       * linear for the compositor. */
      const bool seq_space_is_srgb = IMB_colormanagement_space_name_is_srgb(
          context.render_data.scene->sequencer_colorspace_settings.name);

      const int width = context.render_data.rectx;
      const int height = context.render_data.recty;
      this->mask_ = this->cache_manager().cached_masks.get(*this,
                                                           smd.mask_id,
                                                           compositor::Domain(int2(width, height)),
                                                           1.0f,
                                                           true,
                                                           frame_index,
                                                           1,
                                                           0.0f,
                                                           seq_space_is_srgb);
      this->owns_mask_ = false;
    }
  }
};

static void compositor_modifier_init_data(StripModifierData *strip_modifier_data)
{
  SequencerCompositorModifierData *modifier_data =
      reinterpret_cast<SequencerCompositorModifierData *>(strip_modifier_data);
  modifier_data->node_group = nullptr;
}

static void compositor_modifier_apply(ModifierApplyContext &context,
                                      StripModifierData *strip_modifier_data,
                                      int timeline_frame)
{
  SequencerCompositorModifierData *modifier_data =
      reinterpret_cast<SequencerCompositorModifierData *>(strip_modifier_data);
  if (!modifier_data->node_group) {
    return;
  }

  /* Note: compositor always operates in linear space, float pixels. */
  ensure_ibuf_is_linear_space(context.image, true);
  CompositorCache &com_cache = context.render_data.scene->ed->runtime->ensure_compositor_cache();
  CompositorModifierContext com_mod_context(
      context, timeline_frame, com_cache.get_cache_manager(), modifier_data);

  const bool use_gpu = com_mod_context.use_gpu();
  if (use_gpu) {
    render_begin_gpu(context.render_data);
  }

  com_cache.recreate_if_needed(
      com_mod_context.use_gpu(), com_mod_context.get_precision(), context.render_data.gpu_context);
  com_mod_context.evaluate();
  com_mod_context.cache_manager().reset();
  if (use_gpu) {
    render_end_gpu(context.render_data);
  }

  context.result_translation += com_mod_context.get_result_translation();
}

static PointerRNA *modifier_panel_get_property_pointers(Panel *panel)
{
  PointerRNA *ptr = ui::panel_custom_data_get(panel);
  BLI_assert(!RNA_pointer_is_null(ptr));
  BLI_assert(RNA_struct_is_a(ptr->type, RNA_StripModifier));
  ui::panel_context_pointer_set(panel, "modifier", ptr);
  return ptr;
}

static void compositor_modifier_panel_draw(const bContext *C, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *modifier_ptr = modifier_panel_get_property_pointers(panel);
  nodes::draw_compositor_nodes_modifier_ui(*C, modifier_ptr, layout);
}

static void compositor_modifier_register(ARegionType *region_type)
{
  modifier_panel_register(
      region_type, eSeqModifierType_Compositor, compositor_modifier_panel_draw);
}

StripModifierTypeInfo seqModifierType_Compositor = {
    /*idname*/ "Compositor",
    /*name*/ CTX_N_(BLT_I18NCONTEXT_ID_SEQUENCE, "Compositor"),
    /*struct_name*/ "SequencerCompositorModifierData",
    /*struct_size*/ sizeof(SequencerCompositorModifierData),
    /*init_data*/ compositor_modifier_init_data,
    /*free_data*/ nullptr,
    /*copy_data*/ nullptr,
    /*apply*/ compositor_modifier_apply,
    /*panel_register*/ compositor_modifier_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
};

};  // namespace blender::seq
