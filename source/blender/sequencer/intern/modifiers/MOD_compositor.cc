/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "BLI_rect.h"
#include "BLT_translation.hh"
#include "BLI_listbase.h"

#include "BLO_read_write.hh"

#include "BKE_context.hh"
#include "BKE_idprop.hh"

#include "COM_context.hh"
#include "COM_domain.hh"
#include "COM_evaluator.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_sequence_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "NOD_draw_group_inputs.hh"
#include "NOD_geometry_nodes_execute.hh"
#include "NOD_socket_usage_inference.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "SEQ_modifier.hh"
#include "SEQ_render.hh"

#include "modifier.hh"

namespace blender::seq {

class CompositorContext : public compositor::Context {
 private:
  const RenderData &render_data_;
  const SequencerCompositorModifierData *modifier_data_;

  ImBuf *image_buffer_;

  nodes::PropertiesVectorSet properties_;
  VectorSet<StringRef> socket_identifiers_;

 public:
  CompositorContext(const RenderData &render_data,
                    const SequencerCompositorModifierData *modifier_data,
                    ImBuf *image_buffer)
      : compositor::Context(),
        render_data_(render_data),
        modifier_data_(modifier_data),
        image_buffer_(image_buffer)
  {
    properties_ = nodes::build_properties_vector_set(modifier_data_->settings.properties);

    const bNodeTree &node_tree = this->get_node_tree();
    node_tree.ensure_interface_cache();
    for (const bNodeTreeInterfaceSocket *socket : node_tree.interface_inputs()) {
      socket_identifiers_.add(socket->identifier);
    }
  }

  const Scene &get_scene() const override
  {
    return *render_data_.scene;
  }

  const bNodeTree &get_node_tree() const override
  {
    return *DEG_get_evaluated<bNodeTree>(render_data_.depsgraph, modifier_data_->node_group);
  }

  compositor::OutputTypes needed_outputs() const override
  {
    compositor::OutputTypes needed_outputs = compositor::OutputTypes::Composite;
    if (!render_data_.for_render) {
      needed_outputs |= compositor::OutputTypes::Viewer;
    }
    return needed_outputs;
  }

  bool treat_viewer_as_compositor_output() const override
  {
    return true;
  }

  Bounds<int2> get_compositing_region() const override
  {
    return Bounds<int2>(int2(0), int2(image_buffer_->x, image_buffer_->y));
  }

  compositor::Result get_output() override
  {
    compositor::Result result = this->create_result(compositor::ResultType::Color);
    result.wrap_external(image_buffer_->float_buffer.data,
                         int2(image_buffer_->x, image_buffer_->y));
    return result;
  }

  compositor::Result get_viewer_output(compositor::Domain /*domain*/,
                                       bool /*is_data*/,
                                       compositor::ResultPrecision /*precision*/) override
  {
    compositor::Result result = this->create_result(compositor::ResultType::Color);
    result.wrap_external(image_buffer_->float_buffer.data,
                         int2(image_buffer_->x, image_buffer_->y));
    return result;
  }

  compositor::Result get_input(const Scene * /*scene*/,
                               int /*view_layer_id*/,
                               const char * /*pass_name*/,
                               const char *identifier) override
  {
    if (this->get_node_tree().interface_inputs().is_empty()) {
      return this->create_result(compositor::ResultType::Color);
    }
    /* The first color input is assumed to be image buffer coming from the strip. */
    const bNodeTreeInterfaceSocket *first_socket =
        this->get_node_tree().interface_inputs().first();
    if (first_socket->identifier == StringRef(identifier)) {
      const bke::bNodeSocketType *typeinfo = first_socket->socket_typeinfo();
      const eNodeSocketDatatype type = typeinfo ? typeinfo->type : SOCK_CUSTOM;
      if (type == SOCK_RGBA) {
        compositor::Result result = this->create_result(compositor::ResultType::Color);
        result.wrap_external(image_buffer_->float_buffer.data,
                             int2(image_buffer_->x, image_buffer_->y));
        return result;
      }
    }

    const int socket_i = socket_identifiers_.index_of_try(identifier);
    if (socket_i == -1) {
      /* TODO. */
      return this->create_result(compositor::ResultType::Color);
    }
    const bNodeTreeInterfaceSocket &input = *this->get_node_tree().interface_inputs()[socket_i];
    const bke::bNodeSocketType *typeinfo = input.socket_typeinfo();
    const CPPType *socket_data_type = typeinfo ? typeinfo->base_cpp_type : nullptr;
    if (!socket_data_type) {
      /* TODO. */
      return this->create_result(compositor::ResultType::Color);
    }

    compositor::Result result = this->create_result(
        compositor::Result::from_cpp_type(*socket_data_type));

    const IDProperty *property = properties_.lookup_key_default_as(identifier, nullptr);
    if (property == nullptr) {
      /* TODO. */
      return result;
    }
    result.allocate_single_value();
    result.set_single_value_from_property(*property);
    /* TODO: check with #old_id_property_type_matches_socket_convert_to_new */
    return result;
  }

  bool use_gpu() const override
  {
    return false;
  }
};

static void compositor_modifier_init_data(StripModifierData *strip_modifier_data)
{
  SequencerCompositorModifierData *modifier_data =
      reinterpret_cast<SequencerCompositorModifierData *>(strip_modifier_data);
  modifier_data->node_group = nullptr;
}

static ImBuf *compute_linear_float_buffer(ImBuf *image_buffer)
{
  if (image_buffer->float_buffer.data &&
      IMB_colormanagement_space_is_scene_linear(image_buffer->float_buffer.colorspace))
  {
    return image_buffer;
  }

  ImBuf *linear_float_buffer = IMB_dupImBuf(image_buffer);
  if (image_buffer->float_buffer.data == nullptr) {
    IMB_float_from_byte(linear_float_buffer);
  }
  else {
    IMB_colormanagement_colorspace_to_scene_linear(linear_float_buffer->float_buffer.data,
                                                   linear_float_buffer->x,
                                                   linear_float_buffer->y,
                                                   4,
                                                   image_buffer->float_buffer.colorspace,
                                                   false);
  }

  return linear_float_buffer;
}

static void compositor_modifier_free_data(StripModifierData *smd)
{
  SequencerCompositorModifierData *cmd = reinterpret_cast<SequencerCompositorModifierData *>(smd);
  if (cmd->settings.properties != nullptr) {
    IDP_FreeProperty_ex(cmd->settings.properties, false);
    cmd->settings.properties = nullptr;
  }
}

static void compositor_modifier_copy_data(StripModifierData *target, StripModifierData *smd)
{
  const SequencerCompositorModifierData *cmd =
      reinterpret_cast<const SequencerCompositorModifierData *>(smd);
  SequencerCompositorModifierData *tcmd = reinterpret_cast<SequencerCompositorModifierData *>(
      target);
  if (cmd->settings.properties != nullptr) {
    tcmd->settings.properties = IDP_CopyProperty_ex(cmd->settings.properties, 0);
  }
}

static void compositor_modifier_apply(const RenderData *render_data,
                                      const StripScreenQuad & /*quad*/,
                                      StripModifierData *strip_modifier_data,
                                      ImBuf *image_buffer,
                                      ImBuf * /*mask*/)
{
  const SequencerCompositorModifierData *modifier_data =
      reinterpret_cast<SequencerCompositorModifierData *>(strip_modifier_data);
  if (!modifier_data->node_group) {
    return;
  }

  ImBuf *linear_float_buffer = compute_linear_float_buffer(image_buffer);

  CompositorContext context(*render_data, modifier_data, linear_float_buffer);
  compositor::Evaluator evaluator(context);
  evaluator.evaluate();

  if (image_buffer == linear_float_buffer) {
    return;
  }

  const bool is_byte_buffer = image_buffer->float_buffer.data == nullptr;
  IMB_assign_float_buffer(
      image_buffer, IMB_steal_float_buffer(linear_float_buffer), IB_TAKE_OWNERSHIP);
  if (is_byte_buffer) {
    IMB_byte_from_float(image_buffer);
  }
  else {
    IMB_colormanagement_scene_linear_to_colorspace(linear_float_buffer->float_buffer.data,
                                                   linear_float_buffer->x,
                                                   linear_float_buffer->y,
                                                   4,
                                                   image_buffer->float_buffer.colorspace);
  }

  IMB_freeImBuf(linear_float_buffer);
}

static void compositor_modifier_draw(const bContext *C, Panel *panel)
{
  Main *bmain = CTX_data_main(C);
  PointerRNA bmain_ptr = RNA_main_pointer_create(bmain);

  uiLayout *layout = panel->layout;
  PointerRNA *modifier_ptr = UI_panel_custom_data_get(panel);
  const SequencerCompositorModifierData &modifier_data =
      *modifier_ptr->data_as<SequencerCompositorModifierData>();

  nodes::DrawGroupInputsContext ctx{
      *C,
      modifier_data.node_group,
      nullptr,
      nodes::build_properties_vector_set(modifier_data.settings.properties),
      modifier_ptr,
      &bmain_ptr};

  layout->use_property_split_set(true);

  uiTemplateID(layout,
               C,
               modifier_ptr,
               "node_group",
               "NODE_OT_new_compositor_sequencer_strip_modifier_node_group",
               nullptr,
               nullptr);

  if (modifier_data.node_group != nullptr && modifier_data.settings.properties != nullptr) {
    modifier_data.node_group->ensure_interface_cache();
    ctx.input_usages.reinitialize(modifier_data.node_group->interface_inputs().size());
    nodes::socket_usage_inference::infer_group_interface_inputs_usage(
        *modifier_data.node_group, ctx.properties, ctx.input_usages);
    nodes::draw_interface_panel_content(
        ctx, layout, modifier_data.node_group->tree_interface.root_panel, true);
  }

  if (uiLayout *mask_input_layout = layout->panel_prop(
          C, modifier_ptr, "open_mask_input_panel", IFACE_("Mask Input")))
  {
    draw_mask_input_type_settings(C, mask_input_layout, modifier_ptr);
  }
}

static void compositor_modifier_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eSeqModifierType_Compositor, compositor_modifier_draw);
}

static void compositor_modifier_blend_write(BlendWriter *writer, const StripModifierData *smd)
{
  const SequencerCompositorModifierData *cmd =
      reinterpret_cast<const SequencerCompositorModifierData *>(smd);

  Map<IDProperty *, IDPropertyUIDataBool *> boolean_props;
  if (cmd->settings.properties != nullptr) {
    if (!BLO_write_is_undo(writer)) {
      /* Boolean properties are added automatically for boolean node group inputs. Integer
       * properties are automatically converted to boolean sockets where applicable as well.
       * However, boolean properties will crash old versions of Blender, so convert them to integer
       * properties for writing. The actual value is stored in the same variable for both types */
      LISTBASE_FOREACH (IDProperty *, prop, &cmd->settings.properties->data.group) {
        if (prop->type == IDP_BOOLEAN) {
          boolean_props.add_new(prop, reinterpret_cast<IDPropertyUIDataBool *>(prop->ui_data));
          prop->type = IDP_INT;
          prop->ui_data = nullptr;
        }
      }
    }

    /* Note that the property settings are based on the socket type info
     * and don't necessarily need to be written, but we can't just free them. */
    IDP_BlendWrite(writer, cmd->settings.properties);
  }

  if (cmd->settings.properties != nullptr) {
    if (!BLO_write_is_undo(writer)) {
      LISTBASE_FOREACH (IDProperty *, prop, &cmd->settings.properties->data.group) {
        if (prop->type == IDP_INT) {
          if (IDPropertyUIDataBool **ui_data = boolean_props.lookup_ptr(prop)) {
            prop->type = IDP_BOOLEAN;
            if (ui_data) {
              prop->ui_data = reinterpret_cast<IDPropertyUIData *>(*ui_data);
            }
          }
        }
      }
    }
  }
}

static void compositor_modifier_blend_read_data(BlendDataReader *reader, StripModifierData *smd)
{
  SequencerCompositorModifierData *cmd = reinterpret_cast<SequencerCompositorModifierData *>(smd);
  if (cmd->node_group == nullptr) {
    cmd->settings.properties = nullptr;
  }
  else {
    BLO_read_struct(reader, IDProperty, &cmd->settings.properties);
    IDP_BlendDataRead(reader, &cmd->settings.properties);
  }
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
    /*blend_write*/ compositor_modifier_blend_write,
    /*blend_read*/ compositor_modifier_blend_read_data,
};

}  // namespace blender::seq
