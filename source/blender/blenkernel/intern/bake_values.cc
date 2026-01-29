/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "MEM_guardedalloc.h"

#include "BKE_anonymous_attribute_make.hh"
#include "BKE_bake_values.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"
#include "BKE_mesh_types.hh"
#include "BKE_node.hh"
#include "BKE_pointcloud.hh"

#include "BKE_volume.hh"
#include "DNA_curves_types.h"
#include "DNA_grease_pencil_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "DNA_volume_types.h"
#include "FN_field.hh"

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_closure.hh"
#include "NOD_geometry_nodes_list.hh"
#include "NOD_geometry_nodes_values.hh"

namespace blender::bke::bake {

static constexpr StringRef anonymous_bake_attribute_prefix = ".bake_";

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

  MEM_delete(*materials);
  *materials = nullptr;
  *materials_num = 0;

  return materials_list;
}

static void restore_materials(Material ***materials,
                              short *materials_num,
                              std::unique_ptr<BakeMaterialsList> materials_list,
                              BakeDataBlockMap *data_block_map)
{
  if (!materials_list) {
    return;
  }
  BLI_assert(*materials == nullptr);
  *materials_num = materials_list->size();
  *materials = MEM_new_array_zeroed<Material *>(materials_list->size(), __func__);
  if (!data_block_map) {
    return;
  }

  for (const int i : materials_list->index_range()) {
    const std::optional<BakeDataBlockID> &data_block_id = (*materials_list)[i];
    if (data_block_id) {
      (*materials)[i] = reinterpret_cast<Material *>(
          data_block_map->lookup_or_remember_missing(*data_block_id));
    }
  }
}

class RuntimeToBakeValue {
 private:
  MutableSpan<BakeValues::InputValue> root_values_;
  Map<std::string, std::string> referenced_anonymous_attributes_;
  BakeDataBlockMap *data_block_map_ = nullptr;

 public:
  RuntimeToBakeValue(MutableSpan<BakeValues::InputValue> root_values,
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
      const fn::GField field = value_variant.get<fn::GField>();
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
    if (value_variant.is_list()) {
      const nodes::ListPtr list_ptr = value_variant.get<nodes::ListPtr>();
      if (list_ptr) {
        this->gather__list(*list_ptr);
      }
    }
  }

  void gather__list(const nodes::List &list)
  {
    const CPPType &list_cpp_type = list.cpp_type();
    if (list_cpp_type.is<SocketValueVariant>()) {
      list.foreach<SocketValueVariant>([&](const SocketValueVariant &value_variant) {
        this->gather__socket_value_variant(value_variant);
      });
    }
    else if (list_cpp_type.is<GeometrySet>()) {
      list.foreach<GeometrySet>(
          [&](const GeometrySet &geometry) { this->gather__geometry(geometry); });
    }
    else if (list_cpp_type.is<nodes::BundlePtr>()) {
      list.foreach<nodes::BundlePtr>([&](const nodes::BundlePtr &bundle_ptr) {
        if (bundle_ptr) {
          this->gather__bundle(*bundle_ptr);
        }
      });
    }
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
      return fmt::format(
          "{}{}", anonymous_bake_attribute_prefix, referenced_anonymous_attributes_.size());
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
    if (value_variant.is_list()) {
      nodes::ListPtr list_ptr = value_variant.extract<nodes::ListPtr>();
      if (list_ptr) {
        nodes::List &list = list_ptr.ensure_mutable_inplace();
        this->process__list(list);
      }
      value_variant.set(std::move(list_ptr));
    }
  }

  void process__list(nodes::List &list)
  {
    const CPPType &list_cpp_type = list.cpp_type();
    if (list_cpp_type.is<SocketValueVariant>()) {
      list.foreach_for_write<SocketValueVariant>([&](SocketValueVariant &value_variant) {
        this->process__socket_value_variant(value_variant);
      });
    }
    else if (list_cpp_type.is<GeometrySet>()) {
      list.foreach_for_write<GeometrySet>(
          [&](GeometrySet &geometry) { this->process__geometry(geometry); });
    }
    else if (list_cpp_type.is<nodes::BundlePtr>()) {
      list.foreach_for_write<nodes::BundlePtr>([&](nodes::BundlePtr &bundle_ptr) {
        this->process__bundle(bundle_ptr.ensure_mutable_inplace());
      });
    }
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
    if (type.is<nodes::ClosurePtr>()) {
      nodes::ClosurePtr &closure_ptr = *value_ptr.get<nodes::ClosurePtr>();
      closure_ptr.reset();
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
    if (geometry.has_volume()) {
      Volume &volume = *geometry.get_volume_for_write();
      volume.runtime->bake_materials = materials_to_weak_references(
          &volume.mat, &volume.totcol, data_block_map_);
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

class BakeToRuntimeValue {
 private:
  std::string anonymous_attribute_name_mixin_;
  Map<std::string, std::string> used_anonymous_attributes_;
  BakeDataBlockMap *data_block_map_ = nullptr;

 public:
  BakeToRuntimeValue(std::string anonymous_attribute_name_mixin, BakeDataBlockMap *data_block_map)
      : anonymous_attribute_name_mixin_(std::move(anonymous_attribute_name_mixin)),
        data_block_map_(data_block_map)
  {
  }

  void bake_to_runtime(SocketValueVariant &root_value)
  {
    this->process__socket_value_variant(root_value);
  }

 private:
  void process__socket_value_variant(SocketValueVariant &value_variant)
  {
    if (value_variant.is_context_dependent_field()) {
      const fn::GField field = value_variant.get<fn::GField>();
      if (const auto *attribute_field = dynamic_cast<const AttributeFieldInput *>(&field.node())) {
        const StringRef bake_attribute_name = attribute_field->attribute_name();
        if (bake_attribute_name.startswith(anonymous_bake_attribute_prefix)) {
          std::string anonymous_attribute_name = this->get_anonymous_attribute_name(
              attribute_field->attribute_name());
          value_variant.set(AttributeFieldInput::from(std::move(anonymous_attribute_name),
                                                      attribute_field->cpp_type()));
        }
      }
      return;
    }
    if (value_variant.is_single()) {
      GMutablePointer value_ptr = value_variant.get_single_ptr();
      this->process__gpointer(value_ptr);
      return;
    }
    if (value_variant.is_list()) {
      nodes::ListPtr list_ptr = value_variant.extract<nodes::ListPtr>();
      if (list_ptr) {
        nodes::List &list = list_ptr.ensure_mutable_inplace();
        this->process__list(list);
      }
      value_variant.set(std::move(list_ptr));
    }
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
    if (geometry.has_bundle()) {
      nodes::BundlePtr &bundle_ptr = geometry.bundle_ptr();
      nodes::Bundle &bundle = bundle_ptr.ensure_mutable_inplace();
      this->process__bundle(bundle);
    }
    if (geometry.has_instances()) {
      Instances &instances = *geometry.get_instances_for_write();
      instances.ensure_geometry_instances();
      for (bke::InstanceReference &reference : instances.references_for_write()) {
        GeometrySet &geometry = reference.geometry_set();
        this->process__geometry(geometry);
      }
      this->process__attributes(instances.attribute_storage());
    }
    if (geometry.has_mesh()) {
      Mesh &mesh = *geometry.get_mesh_for_write();
      this->process__attributes(mesh.attribute_storage.wrap());
      restore_materials(
          &mesh.mat, &mesh.totcol, std::move(mesh.runtime->bake_materials), data_block_map_);
    }
    if (geometry.has_curves()) {
      Curves &curves = *geometry.get_curves_for_write();
      this->process__attributes(curves.geometry.attribute_storage.wrap());
      restore_materials(&curves.mat,
                        &curves.totcol,
                        std::move(curves.geometry.runtime->bake_materials),
                        data_block_map_);
    }
    if (geometry.has_pointcloud()) {
      PointCloud &pointcloud = *geometry.get_pointcloud_for_write();
      this->process__attributes(pointcloud.attribute_storage.wrap());
      restore_materials(&pointcloud.mat,
                        &pointcloud.totcol,
                        std::move(pointcloud.runtime->bake_materials),
                        data_block_map_);
    }
    if (geometry.has_grease_pencil()) {
      GreasePencil &grease_pencil = *geometry.get_grease_pencil_for_write();
      this->process__attributes(grease_pencil.attribute_storage.wrap());
      restore_materials(&grease_pencil.material_array,
                        &grease_pencil.material_array_num,
                        std::move(grease_pencil.runtime->bake_materials),
                        data_block_map_);
      for (GreasePencilDrawingBase *base : grease_pencil.drawings()) {
        if (base->type != GP_DRAWING) {
          continue;
        }
        greasepencil::Drawing &drawing = reinterpret_cast<GreasePencilDrawing *>(base)->wrap();
        this->process__attributes(drawing.strokes_for_write().attribute_storage.wrap());
      }
    }
    if (geometry.has_volume()) {
      Volume &volume = *geometry.get_volume_for_write();
      restore_materials(
          &volume.mat, &volume.totcol, std::move(volume.runtime->bake_materials), data_block_map_);
    }
  }

  void process__attributes(AttributeStorage &attributes)
  {
    Vector<std::pair<std::string, std::string>> attributes_to_rename;
    for (const Attribute &attribute : attributes) {
      const StringRef attribute_name = attribute.name();
      if (attribute_name.startswith(anonymous_bake_attribute_prefix)) {
        attributes_to_rename.append(
            {attribute_name, this->get_anonymous_attribute_name(attribute_name)});
      }
    }
    for (const std::pair<std::string, std::string> &attribute_to_rename : attributes_to_rename) {
      attributes.rename(attribute_to_rename.first, attribute_to_rename.second);
    }
  }

  void process__bundle(nodes::Bundle &bundle)
  {
    for (auto item : bundle.items()) {
      if (auto *socket_value = std::get_if<nodes::BundleItemSocketValue>(&item.value.value)) {
        this->process__socket_value_variant(socket_value->value);
      }
    }
  }

  void process__list(nodes::List &list)
  {
    const CPPType &list_cpp_type = list.cpp_type();
    if (list_cpp_type.is<SocketValueVariant>()) {
      list.foreach_for_write<SocketValueVariant>([&](SocketValueVariant &value_variant) {
        this->process__socket_value_variant(value_variant);
      });
    }
    else if (list_cpp_type.is<GeometrySet>()) {
      list.foreach_for_write<GeometrySet>(
          [&](GeometrySet &geometry) { this->process__geometry(geometry); });
    }
    else if (list_cpp_type.is<nodes::BundlePtr>()) {
      list.foreach_for_write<nodes::BundlePtr>([&](nodes::BundlePtr &bundle_ptr) {
        this->process__bundle(bundle_ptr.ensure_mutable_inplace());
      });
    }
  }

  std::string get_anonymous_attribute_name(const StringRef bake_attribute_name)
  {
    return used_anonymous_attributes_.lookup_or_add_cb(bake_attribute_name, [&]() {
      return hash_to_anonymous_attribute_name(anonymous_attribute_name_mixin_,
                                              bake_attribute_name);
    });
  }
};

BakeValues BakeValues::from_runtime_values(Vector<InputValue> runtime_values,
                                           BakeDataBlockMap *data_block_map)
{
  RuntimeToBakeValue preparation{runtime_values, data_block_map};
  preparation.prepare();

  BakeValues bake_values;
  for (InputValue &input_value : runtime_values) {
    input_value.value.ensure_owns_direct_data();
  }
  for (InputValue &input_value : runtime_values) {
    bake_values.values_by_id_.add(input_value.id,
                                  Item{std::move(input_value.value), std::move(input_value.name)});
  }
  return bake_values;
}

Vector<SocketValueVariant> BakeValues::to_runtime_values(const Span<OutputKey> keys,
                                                         const ComputeContext &compute_context,
                                                         BakeDataBlockMap *data_block_map) const
{
  Vector<SocketValueVariant> output_values(keys.size());
  std::stringstream ss;
  ss << compute_context.hash();
  BakeToRuntimeValue bake_to_runtime_op(ss.str(), data_block_map);
  for (const int i : keys.index_range()) {
    const OutputKey &key = keys[i];
    SocketValueVariant &output_value = output_values[i];
    const Item *item = values_by_id_.lookup_ptr(key.id);
    if (!item || !item->value.valid_for_socket(key.type)) {
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

    output_value = item->value;

    bake_to_runtime_op.bake_to_runtime(output_value);
  }
  return output_values;
}

}  // namespace blender::bke::bake
