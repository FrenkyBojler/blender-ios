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
#include "SEQ_modifiertypes.hh"
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
    case SOCK_OBJECT:
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

static bool ensure_linear_float_buffer(ImBuf *ibuf)
{
  if (!ibuf) {
    return false;
  }

  /* Already have scene linear float pixels, nothing to do. */
  if (is_linear_float_buffer(ibuf)) {
    return true;
  }

  if (ibuf->float_buffer.data == nullptr) {
    IMB_float_from_byte(ibuf);
  }
  else {
    const char *from_colorspace = IMB_colormanagement_get_float_colorspace(ibuf);
    const char *to_colorspace = IMB_colormanagement_role_colorspace_name_get(
        COLOR_ROLE_SCENE_LINEAR);
    IMB_colormanagement_transform_float(ibuf->float_buffer.data,
                                        ibuf->x,
                                        ibuf->y,
                                        ibuf->channels,
                                        from_colorspace,
                                        to_colorspace,
                                        true);
    IMB_colormanagement_assign_float_colorspace(ibuf, to_colorspace);
  }
  return false;
}

class CompositorModifierContext : public CompositorContext {
 private:
  const SequencerCompositorModifierData *modifier_data_;
  SeqRenderState &render_state_;

  ImBuf *image_buffer_;
  ImBuf *mask_buffer_;
  float3x3 xform_;
  float2 result_translation_ = float2(0, 0);
  const float timeline_frame_;

  PointerRNA properties_ptr_;

  /* Identified if the output of the viewer was written. */
  bool viewer_was_written_ = false;

 public:
  CompositorModifierContext(compositor::StaticCacheManager &cache_manager,
                            const RenderData &render_data,
                            SequencerCompositorModifierData *modifier_data,
                            SeqRenderState &render_state,
                            ImBuf *image_buffer,
                            ImBuf *mask_buffer,
                            const Strip &strip,
                            float timeline_frame)
      : CompositorContext(cache_manager, render_data, strip),
        modifier_data_(modifier_data),
        render_state_(render_state),
        image_buffer_(image_buffer),
        mask_buffer_(mask_buffer),
        xform_(float3x3::identity()),
        timeline_frame_(timeline_frame)
  {
    if (mask_buffer) {
      /* Note: do not use passed transform matrix since compositor coordinate
       * space is not from the image corner, but rather centered on the image. */
      xform_ = math::invert(image_transform_matrix_get(render_data.scene, &strip));
    }

    PointerRNA ptr = RNA_pointer_create_discrete(const_cast<ID *>(&render_data.scene->id),
                                                 RNA_SequencerCompositorModifierData,
                                                 modifier_data);
    properties_ptr_ = RNA_pointer_get(&ptr, "properties");
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
      const bool is_strip = RNA_enum_get(&input_props_ptr, "type") ==
                            int(nodes::CompositorNodesInputType::Strip);
      const std::optional<ResultType> result_type = Result::from_socket_data_type(socket_type);
      Result *input_result = new Result(
          this->create_result(result_type.value_or(ResultType::Color), ResultPrecision::Full));
      if (result_type) {
        if (!found_image_input && socket_type == SOCK_RGBA) {
          /* First color socket is the image input. */
          create_result_from_input(*input_result, *image_buffer_);
          found_image_input = true;
        }
        else if (mask_buffer_ && !found_mask_input && socket_type == SOCK_RGBA) {
          if (mask_buffer_) {
            /* Second color socket is the mask input. */
            create_result_from_input(*input_result, *mask_buffer_);
            input_result->set_transformation(xform_);
            found_mask_input = true;
          }
          else {
            input_result->allocate_invalid();
          }
        }
        else if (is_strip) {
          const std::string strip_name = RNA_string_get(&input_props_ptr, "strip_name");
          Strip *input_strip = seq::lookup_strip_by_name(seq::editing_get(&get_scene()),
                                                         strip_name.c_str());
          if (!input_strip) {
            /* Set to black. */
            input_result->allocate_single_value();
            input_result->set_single_value(ColorGeometry4f(0.0f, 0.0f, 0.0f, 1.0f));
          }
          else {
            ImBuf *image_buffer = seq::seq_render_strip(
                &render_data_, &render_state_, input_strip, timeline_frame_);
            if (!image_buffer) {
              /* Set to black. */
              input_result->allocate_single_value();
              input_result->set_single_value(ColorGeometry4f(0.0f, 0.0f, 0.0f, 1.0f));
            }
            ImBuf *linear_image_buffer = image_buffer;
            if (!is_linear_float_buffer(image_buffer)) {
              linear_image_buffer = IMB_dupImBuf(image_buffer);
              ensure_linear_float_buffer(linear_image_buffer);
            }
            create_result_from_input(*input_result, *linear_image_buffer);
          }
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
};

static void compositor_modifier_init_data(StripModifierData *strip_modifier_data)
{
  SequencerCompositorModifierData *modifier_data =
      reinterpret_cast<SequencerCompositorModifierData *>(strip_modifier_data);
  modifier_data->node_group = nullptr;

  modifier_data->runtime = MEM_new<SequencerCompositorModifierRuntime>(__func__);
}

static void compositor_modifier_free_data(StripModifierData *strip_modifier_data)
{
  SequencerCompositorModifierData *modifier_data =
      reinterpret_cast<SequencerCompositorModifierData *>(strip_modifier_data);
  if (modifier_data->runtime) {
    MEM_delete(modifier_data->runtime);
  }
  modifier_data->runtime = nullptr;
}

static void compositor_modifier_copy_data(StripModifierData * /*src*/, StripModifierData *dst)
{
  SequencerCompositorModifierData *dst_data = reinterpret_cast<SequencerCompositorModifierData *>(
      dst);
  dst_data->runtime = MEM_new<SequencerCompositorModifierRuntime>(__func__);
}

static void compositor_modifier_read(BlendDataReader * /*reader*/, StripModifierData *smd)
{
  SequencerCompositorModifierData *modifier_data =
      reinterpret_cast<SequencerCompositorModifierData *>(smd);
  modifier_data->runtime = MEM_new<SequencerCompositorModifierRuntime>(__func__);
}

static void compositor_modifier_apply(ModifierApplyContext &context,
                                      StripModifierData *strip_modifier_data,
                                      ImBuf *mask)
{
  SequencerCompositorModifierData *modifier_data =
      reinterpret_cast<SequencerCompositorModifierData *>(strip_modifier_data);
  if (!modifier_data->node_group) {
    return;
  }

  ImBuf *linear_mask = mask;
  if (mask && !is_linear_float_buffer(mask)) {
    linear_mask = IMB_dupImBuf(mask);
    ensure_linear_float_buffer(linear_mask);
  }

  const bool was_float_linear = ensure_linear_float_buffer(context.image);
  const bool was_byte = context.image->float_buffer.data == nullptr;

  CompositorCache &com_cache = context.render_data.scene->ed->runtime->ensure_compositor_cache();
  CompositorModifierContext com_mod_context(com_cache.get_cache_manager(),
                                            context.render_data,
                                            modifier_data,
                                            context.render_state,
                                            context.image,
                                            linear_mask,
                                            context.strip,
                                            context.timeline_frame);

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

  if (mask != linear_mask) {
    IMB_freeImBuf(linear_mask);
  }

  if (was_float_linear) {
    return;
  }

  if (was_byte) {
    IMB_byte_from_float(context.image);
    IMB_free_float_pixels(context.image);
  }
  else {
    seq_imbuf_to_sequencer_space(context.render_data.scene, context.image, true);
  }
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
    /*free_data*/ compositor_modifier_free_data,
    /*copy_data*/ compositor_modifier_copy_data,
    /*apply*/ compositor_modifier_apply,
    /*panel_register*/ compositor_modifier_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ compositor_modifier_read,
};

};  // namespace blender::seq
