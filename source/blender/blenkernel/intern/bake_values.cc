/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_bake_values.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"
#include "BKE_mesh_types.hh"
#include "BKE_node.hh"

#include "BKE_pointcloud.hh"
#include "BLI_set.hh"

#include "DNA_curves_types.h"
#include "DNA_grease_pencil_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"
#include "FN_field.hh"

#include "NOD_geometry_nodes_bundle.hh"

namespace blender::bke::bake {

static std::unique_ptr<BakeMaterialsList> materials_to_weak_references(
    Material ***materials, short *materials_num, BakeDataBlockMap *data_block_map)
{
  if (*materials_num == 0) {
    return {};
  }
  auto materials_list = std::make_unique<BakeMaterialsList>();
  materials_list->resize(*materials_num);
  for (const int i : materials_list->index_range()) {
    Material *material = (*materials)[i];
    if (material) {
      (*materials_list)[i] = BakeDataBlockID(material->id);
      if (data_block_map) {
        data_block_map->try_add(material->id);
      }
    }
  }

  MEM_SAFE_FREE(*materials);
  *materials_num = 0;

  return materials_list;
}

class BakeValuePreparation {
 private:
  MutableSpan<BakeValues::InputValue> root_values_;
  Map<std::string, std::string> referenced_anonymous_attributes_;
  BakeDataBlockMap *data_block_map_ = nullptr;

 public:
  BakeValuePreparation(MutableSpan<BakeValues::InputValue> root_values,
                       BakeDataBlockMap *data_block_map)
      : root_values_(root_values), data_block_map_(data_block_map)
  {
  }

  void prepare()
  {
    /* As a pre-pass, gather all directly referenced anonymous attributes, because those will be
     * kept on the geometries. */
    for (const BakeValues::InputValue &input_value : root_values_) {
      this->gather(input_value);
    }

    /* Now process all data to be stored in a bake. This involves removing data that can't be
     * baked. */
    for (BakeValues::InputValue &input_value : root_values_) {
      this->process(input_value);
    }
  }

 private:
  void gather(const BakeValues::InputValue &input_value)
  {
    this->gather__socket_value_variant(input_value.value);
  }

  void gather__socket_value_variant(const SocketValueVariant &value_variant)
  {
    if (value_variant.is_context_dependent_field()) {
      const fn::GField &field = value_variant.get<fn::GField>();
      if (const auto *attribute_field = dynamic_cast<const AttributeFieldInput *>(&field.node())) {
        const StringRef attribute_name = attribute_field->attribute_name();
        if (attribute_name_is_anonymous(attribute_name)) {
          this->handle_anonymous_attribute_reference(attribute_name);
        }
      }
      return;
    }
    if (value_variant.is_single()) {
      const GPointer value_ptr = value_variant.get_single_ptr();
      this->gather__gpointer(value_ptr);
      return;
    }
    // TODO: handle list
  }

  void gather__gpointer(const GPointer &value_ptr)
  {
    const CPPType &type = *value_ptr.type();
    if (type.is<GeometrySet>()) {
      const GeometrySet &geometry = *value_ptr.get<GeometrySet>();
      this->gather__geometry(geometry);
      return;
    }
    if (type.is<nodes::BundlePtr>()) {
      const nodes::BundlePtr &bundle_ptr = *value_ptr.get<nodes::BundlePtr>();
      if (bundle_ptr) {
        this->gather__bundle(*bundle_ptr);
      }
      return;
    }
  }

  void gather__geometry(const GeometrySet &geometry)
  {
    if (geometry.has_bundle()) {
      const nodes::Bundle &bundle = *geometry.bundle();
      this->gather__bundle(bundle);
    }
    if (geometry.has_instances()) {
      const Instances &instances = *geometry.get_instances();
      for (const bke::InstanceReference &reference : instances.references()) {
        GeometrySet geometry;
        reference.to_geometry_set(geometry);
        this->gather__geometry(geometry);
      }
    }
  }

  void gather__bundle(const nodes::Bundle &bundle)
  {
    for (const auto &item : bundle.items()) {
      if (const auto *socket_value = std::get_if<nodes::BundleItemSocketValue>(&item.value.value))
      {
        this->gather__socket_value_variant(socket_value->value);
      }
    }
  }

  void handle_anonymous_attribute_reference(const StringRef attribute_name)
  {
    referenced_anonymous_attributes_.lookup_or_add_cb_as(attribute_name, [&]() {
      return fmt::format(".bake_{}", referenced_anonymous_attributes_.size());
    });
  }

  void process(BakeValues::InputValue &input_value)
  {
    this->process__socket_value_variant(input_value.value);
  }

  void process__socket_value_variant(SocketValueVariant &value_variant)
  {
    if (value_variant.is_context_dependent_field()) {
      const fn::GField field = value_variant.get<fn::GField>();
      if (const auto *attribute_field = dynamic_cast<const AttributeFieldInput *>(&field.node())) {
        if (const std::string *new_name = referenced_anonymous_attributes_.lookup_ptr(
                attribute_field->attribute_name()))
        {
          value_variant.set(AttributeFieldInput::from(*new_name, field.cpp_type()));
        }
      }
      else {
        /* Only attribute fields can be baked. Other fields are discarded. */
        value_variant.convert_to_single();
      }
      return;
    }
    if (value_variant.is_single()) {
      GMutablePointer value_ptr = value_variant.get_single_ptr();
      this->process__gpointer(value_ptr);
      return;
    }
    // TODO: Handle list
  }

  void process__gpointer(GMutablePointer value_ptr)
  {
    const CPPType &type = *value_ptr.type();
    if (type.is<GeometrySet>()) {
      GeometrySet &geometry = *value_ptr.get<GeometrySet>();
      this->process__geometry(geometry);
    }
    if (type.is<nodes::BundlePtr>()) {
      nodes::BundlePtr &bundle_ptr = *value_ptr.get<nodes::BundlePtr>();
      if (bundle_ptr) {
        nodes::Bundle &bundle = bundle_ptr.ensure_mutable_inplace();
        this->process__bundle(bundle);
      }
    }
  }

  void process__geometry(GeometrySet &geometry)
  {
    geometry.ensure_owns_all_data();
    if (geometry.has_bundle()) {
      nodes::BundlePtr &bundle_ptr = geometry.bundle_ptr();
      nodes::Bundle &bundle = bundle_ptr.ensure_mutable_inplace();
      this->process__bundle(bundle);
    }
    if (geometry.has_instances()) {
      Instances &instances = *geometry.get_instances_for_write();
      instances.ensure_geometry_instances();
      this->process__attributes(instances.attribute_storage());
      for (bke::InstanceReference &reference : instances.references_for_write()) {
        GeometrySet &geometry = reference.geometry_set();
        this->process__geometry(geometry);
      }
    }
    if (geometry.has_mesh()) {
      Mesh &mesh = *geometry.get_mesh_for_write();
      this->process__attributes(mesh.attribute_storage.wrap());
      mesh.runtime->bake_materials = materials_to_weak_references(
          &mesh.mat, &mesh.totcol, data_block_map_);
    }
    if (geometry.has_curves()) {
      Curves &curves = *geometry.get_curves_for_write();
      this->process__attributes(curves.geometry.attribute_storage.wrap());
      curves.geometry.runtime->bake_materials = materials_to_weak_references(
          &curves.mat, &curves.totcol, data_block_map_);
    }
    if (geometry.has_pointcloud()) {
      PointCloud &pointcloud = *geometry.get_pointcloud_for_write();
      this->process__attributes(pointcloud.attribute_storage.wrap());
      pointcloud.runtime->bake_materials = materials_to_weak_references(
          &pointcloud.mat, &pointcloud.totcol, data_block_map_);
    }
    if (geometry.has_grease_pencil()) {
      GreasePencil &grease_pencil = *geometry.get_grease_pencil_for_write();
      this->process__attributes(grease_pencil.attribute_storage.wrap());
      for (GreasePencilDrawingBase *base : grease_pencil.drawings()) {
        if (base->type != GP_DRAWING) {
          continue;
        }
        greasepencil::Drawing &drawing = reinterpret_cast<GreasePencilDrawing *>(base)->wrap();
        this->process__attributes(drawing.strokes_for_write().attribute_storage.wrap());
      }
      grease_pencil.runtime->bake_materials = materials_to_weak_references(
          &grease_pencil.material_array, &grease_pencil.material_array_num, data_block_map_);
    }
  }

  void process__attributes(AttributeStorage &attributes)
  {
    Vector<std::string> attributes_to_remove;
    Vector<std::pair<std::string, std::string>> attributes_to_rename;
    for (const Attribute &attribute : attributes) {
      const StringRef attribute_name = attribute.name();
      if (attribute_name_is_anonymous(attribute_name)) {
        const std::string *new_name = referenced_anonymous_attributes_.lookup_ptr(attribute_name);
        if (new_name) {
          attributes_to_rename.append({attribute_name, *new_name});
        }
        else {
          attributes_to_remove.append(attribute_name);
        }
      }
    }
    for (const StringRef attribute_name : attributes_to_remove) {
      attributes.remove(attribute_name);
    }
    for (const std::pair<std::string, std::string> &attribute_to_rename : attributes_to_rename) {
      attributes.rename(attribute_to_rename.first, attribute_to_rename.second);
    }
  }

  void process__bundle(nodes::Bundle &bundle)
  {
    for (const auto &item : bundle.items()) {
      if (auto *socket_value = std::get_if<nodes::BundleItemSocketValue>(&item.value.value)) {
        this->process__socket_value_variant(socket_value->value);
      }
    }
  }
};

BakeValues BakeValues::from_runtime_values(Vector<InputValue> runtime_values,
                                           BakeDataBlockMap *data_block_map)
{
  BakeValuePreparation preparation{runtime_values, data_block_map};
  preparation.prepare();

  BakeValues bake_values;
  // TODO: ensure owns all data, material pointers, clear fields/closures, ...
  for (InputValue &input_value : runtime_values) {
    input_value.value.ensure_owns_direct_data();
  }
  for (InputValue &input_value : runtime_values) {
    bake_values.values_by_id_.add(input_value.id,
                                  Item{std::move(input_value.value), std::move(input_value.name)});
  }
  return bake_values;
}

Vector<SocketValueVariant> BakeValues::to_runtime_values(
    const Span<OutputKey> keys,
    const ComputeContext & /*compute_context*/,
    BakeDataBlockMap * /*data_block_map*/) const
{
  Vector<SocketValueVariant> output_values(keys.size());
  for (const int i : keys.index_range()) {
    const OutputKey &key = keys[i];
    SocketValueVariant &output_value = output_values[i];
    const Item *item = values_by_id_.lookup_ptr(key.id);
    if (item == nullptr) {
      bke::bNodeSocketType *stype = node_socket_type_find_static(key.type);
      if (!stype) {
        continue;
      }
      if (!stype->geometry_nodes_default_value) {
        continue;
      }
      output_value = *stype->geometry_nodes_default_value;
      continue;
    }
    // TODO: potentially implicit conversion
    output_value = item->value;
  }
  return output_values;
}

}  // namespace blender::bke::bake
