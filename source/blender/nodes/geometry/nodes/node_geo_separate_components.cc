/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_separate_components_cc {

enum class Mode {
  All = 0,
  Single = 1,
};

static const EnumPropertyItem mode_items[] = {
    {int(Mode::All), "ALL", 0, "All", "Split all components into separate geometries"},
    {int(Mode::Single), "SINGLE", 0, "Single", "Split out a single component"},
    {},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .description("Geometry to split into separate components");
  b.add_input<decl::Menu>("Mode"_ustr).static_items(mode_items).expanded().optional_label();
  b.add_input<decl::Menu>("Type"_ustr)
      .static_items(rna_enum_geometry_component_type_items)
      .optional_label()
      .usage_by_menu("Mode"_ustr, int(Mode::Single));

  b.add_output<decl::Geometry>("Mesh"_ustr)
      .propagate_all()
      .usage_by_menu("Mode"_ustr, int(Mode::All));
  b.add_output<decl::Geometry>("Curve"_ustr)
      .propagate_all()
      .usage_by_menu("Mode"_ustr, int(Mode::All));
  b.add_output<decl::Geometry>("Grease Pencil"_ustr)
      .propagate_all()
      .usage_by_menu("Mode"_ustr, int(Mode::All));
  b.add_output<decl::Geometry>("Point Cloud"_ustr)
      .propagate_all()
      .usage_by_menu("Mode"_ustr, int(Mode::All));
  b.add_output<decl::Geometry>("Volume"_ustr)
      .translation_context(BLT_I18NCONTEXT_ID_ID)
      .propagate_all()
      .usage_by_menu("Mode"_ustr, int(Mode::All));
  b.add_output<decl::Geometry>("Instances"_ustr)
      .propagate_all()
      .usage_by_menu("Mode"_ustr, int(Mode::All));

  b.add_output<decl::Geometry>("Component"_ustr)
      .propagate_all()
      .usage_by_menu("Mode"_ustr, int(Mode::Single));
  b.add_output<decl::Geometry>("Other"_ustr)
      .propagate_all()
      .usage_by_menu("Mode"_ustr, int(Mode::Single));
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry"_ustr);
  const Mode mode = params.extract_input<Mode>("Mode"_ustr);

  switch (mode) {
    case Mode::All: {
      GeometrySet meshes;
      GeometrySet curves;
      GeometrySet grease_pencil;
      GeometrySet pointclouds;
      GeometrySet volumes;
      GeometrySet instances;

      const StringRef name = geometry_set.name();
      if (!name.is_empty()) {
        meshes.set_name(name);
        curves.set_name(name);
        grease_pencil.set_name(name);
        pointclouds.set_name(name);
        volumes.set_name(name);
        instances.set_name(name);
      }

      meshes.copy_bundle_from(geometry_set);
      curves.copy_bundle_from(geometry_set);
      grease_pencil.copy_bundle_from(geometry_set);
      pointclouds.copy_bundle_from(geometry_set);
      volumes.copy_bundle_from(geometry_set);
      instances.copy_bundle_from(geometry_set);

      if (geometry_set.has<MeshComponent>()) {
        meshes.add(*geometry_set.get_component<MeshComponent>());
      }
      if (geometry_set.has<CurveComponent>()) {
        curves.add(*geometry_set.get_component<CurveComponent>());
      }
      if (geometry_set.has<GreasePencilComponent>()) {
        grease_pencil.add(*geometry_set.get_component<GreasePencilComponent>());
      }
      if (geometry_set.has<PointCloudComponent>()) {
        pointclouds.add(*geometry_set.get_component<PointCloudComponent>());
      }
      if (geometry_set.has<VolumeComponent>()) {
        volumes.add(*geometry_set.get_component<VolumeComponent>());
      }
      if (geometry_set.has<InstancesComponent>()) {
        instances.add(*geometry_set.get_component<InstancesComponent>());
      }

      params.set_output("Mesh"_ustr, meshes);
      params.set_output("Curve"_ustr, curves);
      params.set_output("Grease Pencil"_ustr, grease_pencil);
      params.set_output("Point Cloud"_ustr, pointclouds);
      params.set_output("Volume"_ustr, volumes);
      params.set_output("Instances"_ustr, instances);
      break;
    }
    case Mode::Single: {
      const GeometryComponent::Type type = params.extract_input<GeometryComponent::Type>(
          "Type"_ustr);

      GeometrySet component_geo;
      switch (type) {
        case bke::GeometryComponent::Type::Mesh:
        case bke::GeometryComponent::Type::PointCloud:
        case bke::GeometryComponent::Type::Instance:
        case bke::GeometryComponent::Type::Volume:
        case bke::GeometryComponent::Type::Curve:
        case bke::GeometryComponent::Type::GreasePencil: {
          if (const GeometryComponent *component = geometry_set.get_component(type)) {
            component_geo.add(*component);
          }
          component_geo.copy_bundle_from(geometry_set);
          component_geo.set_name(geometry_set.name());
          geometry_set.remove(type);
          break;
        }
        default: {
          break;
        }
      }
      params.set_output("Component"_ustr, component_geo);
      params.set_output("Other"_ustr, geometry_set);
      break;
    }
  }
  params.set_default_remaining_outputs();
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSeparateComponents"_ustr, GEO_NODE_SEPARATE_COMPONENTS);
  ntype.ui_name = "Separate Components";
  ntype.ui_description =
      "Split a geometry into a separate output for each type of data in the geometry";
  ntype.enum_name_legacy = "SEPARATE_COMPONENTS";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_separate_components_cc
