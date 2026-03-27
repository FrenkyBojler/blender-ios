/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "BLT_translation.hh"

#include "COM_domain.hh"
#include "COM_realize_on_domain_operation.hh"
#include "COM_result.hh"

#include "DNA_node_types.h"
#include "DNA_sequence_types.h"

#include "BKE_anim_data.hh"
#include "BKE_animsys.h"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_mask.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "DEG_depsgraph_query.hh"

#include "IMB_colormanagement.hh"

#include "SEQ_modifier.hh"
#include "SEQ_select.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_transform.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "cache/compositor_cache.hh"
#include "compositor.hh"
#include "modifier.hh"
#include "render.hh"

namespace blender::seq {

class CompositorModifierContext : public CompositorContext {
 private:
  const SequencerCompositorModifierData *modifier_data_;

  ImBuf *image_buffer_;
  compositor::Result mask_;
  float3x3 mask_transform_;
  ImBuf *mask_buffer_ = nullptr;

 public:
  CompositorModifierContext(const ModifierApplyContext &mod_context,
                            int timeline_frame,
                            compositor::StaticCacheManager &cache_manager,
                            const SequencerCompositorModifierData *modifier_data)
      : CompositorContext(cache_manager, mod_context.render_data, mod_context.strip),
        modifier_data_(modifier_data),
        image_buffer_(mod_context.image),
        mask_(*this, compositor::ResultType::Color, compositor::ResultPrecision::Full)
  {
    render_mask_input(mod_context, timeline_frame);

    /* Masks are in screen space, whereas modifier executes in strip space. */
    mask_transform_ = math::invert(
        image_transform_matrix_get(mod_context.render_data.scene, &mod_context.strip));
  }
  ~CompositorModifierContext()
  {
    if (this->mask_buffer_ != nullptr) {
      IMB_freeImBuf(this->mask_buffer_);
    }
    if (mask_.is_allocated()) {
      mask_.release();
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

    /* Map the inputs to the operation. */
    Vector<std::unique_ptr<Result>> inputs;
    for (const bNodeTreeInterfaceSocket *input_socket : node_group.interface_inputs()) {
      Result *input_result = new Result(
          this->create_result(ResultType::Color, ResultPrecision::Full));
      if (input_socket == node_group.interface_inputs()[0]) {
        /* First socket is the image input. */
        create_result_from_input(*input_result, *image_buffer_);
      }
      else if (mask_.is_allocated() && input_socket == node_group.interface_inputs()[1]) {
        /* Second socket is the mask input. */
        input_result->set_type(this->mask_.type());
        input_result->set_precision(this->mask_.precision());
        input_result->wrap_external(this->mask_);
        input_result->set_transformation(this->mask_transform_);
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
      }
    }
    else if (smd.mask_input_type == STRIP_MASK_INPUT_ID && smd.mask_id) {
      int frame_offset = 0;
      if (smd.mask_time == STRIP_MASK_TIME_RELATIVE) {
        frame_offset = context.strip.start;
      }
      else if (smd.mask_time == STRIP_MASK_TIME_ABSOLUTE) {
        frame_offset = smd.mask_id->sfra;
      }
      int frame_index = timeline_frame - frame_offset;

      /* Copy mask and evaluate it at the needed frame. */
      Mask *mask_temp = id_cast<Mask *>(BKE_id_copy_ex(
          nullptr, &smd.mask_id->id, nullptr, LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_NO_ANIMDATA));
      BKE_mask_evaluate(mask_temp, smd.mask_id->sfra + frame_index, true);
      AnimData *adt = BKE_animdata_from_id(&smd.mask_id->id);
      const AnimationEvalContext anim_eval_context = BKE_animsys_eval_context_construct(
          context.render_data.depsgraph, smd.mask_id->sfra + frame_index);
      BKE_animsys_evaluate_animdata(
          &mask_temp->id, adt, &anim_eval_context, ADT_RECALC_ANIM, false);

      /* Rasterize the mask. */
      const int width = context.render_data.rectx;
      const int height = context.render_data.recty;
      MaskRasterHandle *mr_handle = BKE_maskrasterize_handle_new();
      BKE_maskrasterize_handle_init(mr_handle, mask_temp, width, height, true, true, true);
      BKE_id_free(nullptr, &mask_temp->id);

      this->mask_ = this->create_result(compositor::ResultType::Float);
      this->mask_.allocate_texture(
          compositor::Domain(int2(width, height)), false, compositor::ResultStorageType::CPU);

      const bool seq_space_is_srgb = IMB_colormanagement_space_name_is_srgb(
          context.render_data.scene->sequencer_colorspace_settings.name);

      const float x_inv = 1.0f / float(width);
      const float y_inv = 1.0f / float(height);
      const float x_px_ofs = x_inv * 0.5f;
      const float y_px_ofs = y_inv * 0.5f;
      float *dst_float = this->mask_.cpu_data().typed<float>().data();
      threading::parallel_for(IndexRange(height), 16, [&](const IndexRange y_range) {
        const int64_t pixel_offset = y_range.first() * width;
        float *ptr_float = dst_float + pixel_offset;
        for (int64_t y : y_range) {
          float2 coord;
          coord.y = y * y_inv + y_px_ofs;
          for (int x = 0; x < width; x++) {
            coord.x = x * x_inv + x_px_ofs;
            float value = BKE_maskrasterize_handle_sample(mr_handle, coord);

            /* Mask is a grayscale value, similar to alpha, so conceptually it is already a
             * "linear" quantity. However, masks used to be turned into grayscale images and
             * interpreted as being in "sequencer working space" (default: sRGB), so keep at least
             * that behavior working as before -- if sequencer space is sRGB, convert value to
             * linear for the compositor. */
            if (seq_space_is_srgb) {
              value = srgb_to_linearrgb(value);
            }
            *ptr_float = value;
            ptr_float++;
          }
        }
      });

      BKE_maskrasterize_handle_free(mr_handle);
      if (this->use_gpu()) {
        const compositor::Result gpu_mask = this->mask_.upload_to_gpu(false);
        this->mask_.release();
        this->mask_ = gpu_mask;
      }
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
  const SequencerCompositorModifierData *modifier_data =
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

static void compositor_modifier_panel_draw(const bContext *C, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = ui::panel_custom_data_get(panel);

  layout.use_property_split_set(true);

  Scene *scene = CTX_data_sequencer_scene(C);
  Strip *strip = seq::select_active_get(scene);
  bool has_existing_group = false;
  if (strip != nullptr) {
    StripModifierData *smd = seq::modifier_get_active(strip);

    if (smd && smd->type == eSeqModifierType_Compositor) {
      SequencerCompositorModifierData *nmd = reinterpret_cast<SequencerCompositorModifierData *>(
          smd);
      if (nmd->node_group != nullptr) {
        template_id(&layout,
                    C,
                    ptr,
                    "node_group",
                    "NODE_OT_duplicate_compositing_modifier_node_group",
                    nullptr,
                    nullptr);
        has_existing_group = true;
      }
    }
  }

  if (!has_existing_group) {
    template_id(&layout,
                C,
                ptr,
                "node_group",
                "NODE_OT_new_compositor_sequencer_node_group",
                nullptr,
                nullptr);
  }

  if (ui::Layout *mask_input_layout = layout.panel_prop(
          C, ptr, "open_mask_input_panel", IFACE_("Mask Input")))
  {
    draw_mask_input_type_settings(C, *mask_input_layout, ptr);
  }
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
};

};  // namespace blender::seq
