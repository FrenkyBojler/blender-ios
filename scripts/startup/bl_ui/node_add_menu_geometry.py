# SPDX-FileCopyrightText: 2022-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Menu
from bl_ui import node_add_menu
from bpy.app.translations import (
    contexts as i18n_contexts,
)


class NODE_MT_gn_attribute_base(Menu):
    bl_label = "Attribute"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeAttributeStatistic")
        self.node_operator(layout, "GeometryNodeAttributeDomainSize")
        layout.separator()
        self.node_operator(layout, "GeometryNodeBlurAttribute")
        self.node_operator(layout, "GeometryNodeCaptureAttribute")
        self.node_operator(layout, "GeometryNodeRemoveAttribute")
        self.node_operator(layout, "GeometryNodeStoreNamedAttribute", search_weight=1.0)
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_utilities_color_base(Menu):
    bl_label = "Color"
    menu_path = "Utilities/Color"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "ShaderNodeBlackbody")
        self.node_operator(layout, "ShaderNodeValToRGB")
        self.node_operator(layout, "ShaderNodeRGBCurve")
        layout.separator()
        self.node_operator(layout, "FunctionNodeCombineColor")
        self.color_mix_node(context, layout)
        self.node_operator(layout, "FunctionNodeSeparateColor")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_curve_base(Menu):
    bl_label = "Curve"

    def draw(self, _context):
        layout = self.layout
        self.draw_menu(layout, path="Curve/Read")
        self.draw_menu(layout, path="Curve/Sample")
        self.draw_menu(layout, path="Curve/Write")
        layout.separator()
        self.draw_menu(layout, path="Curve/Operations")
        self.draw_menu(layout, path="Curve/Primitives")
        self.draw_menu(layout, path="Curve/Topology")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_curve_read_base(Menu):
    bl_label = "Read"
    menu_path = "Curve/Read"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeInputCurveHandlePositions")
        self.node_operator(layout, "GeometryNodeCurveLength")
        self.node_operator(layout, "GeometryNodeInputTangent")
        self.node_operator(layout, "GeometryNodeInputCurveTilt")
        self.node_operator(layout, "GeometryNodeCurveEndpointSelection")
        self.node_operator(layout, "GeometryNodeCurveHandleTypeSelection")
        self.node_operator(layout, "GeometryNodeInputSplineCyclic")
        self.node_operator(layout, "GeometryNodeSplineLength")
        self.node_operator(layout, "GeometryNodeSplineParameter")
        self.node_operator(layout, "GeometryNodeInputSplineResolution")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_curve_sample_base(Menu):
    bl_label = "Sample"
    menu_path = "Curve/Sample"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeSampleCurve")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_curve_write_base(Menu):
    bl_label = "Write"
    menu_path = "Curve/Write"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeSetCurveNormal")
        self.node_operator(layout, "GeometryNodeSetCurveRadius")
        self.node_operator(layout, "GeometryNodeSetCurveTilt")
        self.node_operator(layout, "GeometryNodeSetCurveHandlePositions")
        self.node_operator(layout, "GeometryNodeCurveSetHandles")
        self.node_operator(layout, "GeometryNodeSetSplineCyclic")
        self.node_operator(layout, "GeometryNodeSetSplineResolution")
        self.node_operator(layout, "GeometryNodeCurveSplineType")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_curve_operations_base(Menu):
    bl_label = "Operations"
    menu_path = "Curve/Operations"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeCurveToMesh")
        self.node_operator(layout, "GeometryNodeCurveToPoints")
        self.node_operator(layout, "GeometryNodeCurvesToGreasePencil")
        self.node_operator(layout, "GeometryNodeDeformCurvesOnSurface")
        self.node_operator(layout, "GeometryNodeFillCurve")
        self.node_operator(layout, "GeometryNodeFilletCurve")
        self.node_operator(layout, "GeometryNodeInterpolateCurves")
        self.node_operator(layout, "GeometryNodeResampleCurve")
        self.node_operator(layout, "GeometryNodeReverseCurve")
        self.node_operator(layout, "GeometryNodeSubdivideCurve")
        self.node_operator(layout, "GeometryNodeTrimCurve")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_curve_primitives_base(Menu):
    bl_label = "Primitives"
    menu_path = "Curve/Primitives"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeCurveArc")
        self.node_operator(layout, "GeometryNodeCurvePrimitiveBezierSegment")
        self.node_operator(layout, "GeometryNodeCurvePrimitiveCircle")
        self.node_operator(layout, "GeometryNodeCurvePrimitiveLine")
        self.node_operator(layout, "GeometryNodeCurveSpiral")
        self.node_operator(layout, "GeometryNodeCurveQuadraticBezier")
        self.node_operator(layout, "GeometryNodeCurvePrimitiveQuadrilateral")
        self.node_operator(layout, "GeometryNodeCurveStar")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_curve_topology_base(Menu):
    bl_label = "Topology"
    menu_path = "Curve/Topology"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeCurveOfPoint")
        self.node_operator(layout, "GeometryNodeOffsetPointInCurve")
        self.node_operator(layout, "GeometryNodePointsOfCurve")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_grease_pencil_read_base(Menu):
    bl_label = "Read"
    menu_path = "Grease Pencil/Read"
    
    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeInputNamedLayerSelection")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_grease_pencil_write_base(Menu):
    bl_label = "Write"
    menu_path = "Grease Pencil/Write"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeSetGreasePencilColor")
        self.node_operator(layout, "GeometryNodeSetGreasePencilDepth")
        self.node_operator(layout, "GeometryNodeSetGreasePencilSoftness")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_grease_pencil_operations_base(Menu):
    bl_label = "Operations"
    menu_path = "Grease Pencil/Operations"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeGreasePencilToCurves")
        self.node_operator(layout, "GeometryNodeMergeLayers")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_grease_pencil_base(Menu):
    bl_label = "Grease Pencil"

    def draw(self, _context):
        layout = self.layout
        self.draw_menu(layout, path="Grease Pencil/Read")
        self.draw_menu(layout, path="Grease Pencil/Write")
        layout.separator()
        self.draw_menu(layout, path="Grease Pencil/Operations")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_geometry_base(Menu):
    bl_label = "Geometry"

    def draw(self, _context):
        layout = self.layout
        self.draw_menu(layout, path="Geometry/Read")
        self.draw_menu(layout, path="Geometry/Sample")
        self.draw_menu(layout, path="Geometry/Write")
        layout.separator()
        self.draw_menu(layout, path="Geometry/Operations")
        layout.separator()
        self.node_operator(layout, "GeometryNodeGeometryToInstance")
        self.node_operator(layout, "GeometryNodeJoinGeometry", search_weight=1.0)
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_geometry_read_base(Menu):
    bl_label = "Read"
    menu_path = "Geometry/Read"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeInputID")
        self.node_operator(layout, "GeometryNodeInputIndex")
        self.node_operator(layout, "GeometryNodeInputNamedAttribute", search_weight=1.0)
        self.node_operator(layout, "GeometryNodeInputNormal")
        self.node_operator(layout, "GeometryNodeInputPosition", search_weight=1.0)
        self.node_operator(layout, "GeometryNodeInputRadius")
        if context.space_data.geometry_nodes_type == 'TOOL':
            self.node_operator(layout, "GeometryNodeToolSelection")
            self.node_operator(layout, "GeometryNodeToolActiveElement")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_geometry_write_base(Menu):
    bl_label = "Write"
    menu_path = "Geometry/Write"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeSetGeometryName")
        self.node_operator(layout, "GeometryNodeSetID")
        self.node_operator(layout, "GeometryNodeSetPosition", search_weight=1.0)
        if context.space_data.geometry_nodes_type == 'TOOL':
            self.node_operator(layout, "GeometryNodeToolSetSelection")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_geometry_operations_base(Menu):
    bl_label = "Operations"
    menu_path = "Geometry/Operations"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeBake")
        self.node_operator(layout, "GeometryNodeBoundBox")
        self.node_operator(layout, "GeometryNodeConvexHull")
        self.node_operator(layout, "GeometryNodeDeleteGeometry")
        self.node_operator(layout, "GeometryNodeDuplicateElements")
        self.node_operator(layout, "GeometryNodeMergeByDistance")
        self.node_operator(layout, "GeometryNodeSortElements")
        self.node_operator(layout, "GeometryNodeTransform", search_weight=1.0)
        layout.separator()
        self.node_operator(layout, "GeometryNodeSeparateComponents")
        self.node_operator(layout, "GeometryNodeSeparateGeometry")
        self.node_operator(layout, "GeometryNodeSplitToInstances")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_geometry_sample_base(Menu):
    bl_label = "Sample"
    menu_path = "Geometry/Sample"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeProximity")
        self.node_operator(layout, "GeometryNodeIndexOfNearest")
        self.node_operator(layout, "GeometryNodeRaycast")
        self.node_operator(layout, "GeometryNodeSampleIndex")
        self.node_operator(layout, "GeometryNodeSampleNearest")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_input_base(Menu):
    bl_label = "Input"

    def draw(self, context):
        layout = self.layout
        self.draw_menu(layout, path="Input/Constant")
        if context.space_data.geometry_nodes_type != 'TOOL':
            self.draw_menu(layout, path="Input/Gizmo")
        self.draw_menu(layout, path="Input/Group")
        self.draw_menu(layout, path="Input/Import")
        self.draw_menu(layout, path="Input/Scene")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_input_constant_base(Menu):
    bl_label = "Constant"
    bl_translation_context = i18n_contexts.id_nodetree
    menu_path = "Input/Constant"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "FunctionNodeInputBool")
        self.node_operator(layout, "GeometryNodeInputCollection")
        self.node_operator(layout, "FunctionNodeInputColor")
        self.node_operator(layout, "GeometryNodeInputImage")
        self.node_operator(layout, "FunctionNodeInputInt")
        self.node_operator(layout, "GeometryNodeInputMaterial")
        self.node_operator(layout, "GeometryNodeInputObject")
        self.node_operator(layout, "FunctionNodeInputRotation")
        self.node_operator(layout, "FunctionNodeInputString")
        self.node_operator(layout, "ShaderNodeValue")
        self.node_operator(layout, "FunctionNodeInputVector")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_input_group_base(Menu):
    bl_label = "Group"
    menu_path = "Input/Group"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "NodeGroupInput")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_input_scene_base(Menu):
    bl_label = "Scene"
    menu_path = "Input/Scene"

    def draw(self, context):
        layout = self.layout
        if context.space_data.geometry_nodes_type == 'TOOL':
            self.node_operator(layout, "GeometryNodeTool3DCursor")
        self.node_operator(layout, "GeometryNodeInputActiveCamera")
        self.node_operator_with_outputs(
            context,
            layout,
            "GeometryNodeCameraInfo",
            [
                "Projection Matrix",
                "Focal Length",
                "Sensor",
                "Shift",
                "Clip Start",
                "Clip End",
                "Focus Distance",
                "Is Orthographic",
                "Orthographic Scale",
            ],
        )
        self.node_operator(layout, "GeometryNodeCollectionInfo")
        self.node_operator(layout, "GeometryNodeImageInfo")
        self.node_operator(layout, "GeometryNodeIsViewport")
        if context.space_data.geometry_nodes_type == 'TOOL':
            self.node_operator_with_outputs(
                context, layout, "GeometryNodeToolMousePosition",
                ["Mouse X", "Mouse Y", "Region Width", "Region Height"],
            )
        self.node_operator(layout, "GeometryNodeObjectInfo")
        self.node_operator_with_outputs(context, layout, "GeometryNodeInputSceneTime", ["Frame", "Seconds"])
        self.node_operator(layout, "GeometryNodeSelfObject")
        if context.space_data.geometry_nodes_type == 'TOOL':
            self.node_operator_with_outputs(
                context, layout, "GeometryNodeViewportTransform",
                ["Projection", "View", "Is Orthographic"],
            )

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_input_gizmo_base(Menu):
    bl_label = "Gizmo"
    menu_path = "Input/Gizmo"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeGizmoDial")
        self.node_operator(layout, "GeometryNodeGizmoLinear")
        self.node_operator(layout, "GeometryNodeGizmoTransform")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_instance_base(Menu):
    bl_label = "Instances"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeInstanceOnPoints", search_weight=2.0)
        self.node_operator(layout, "GeometryNodeInstancesToPoints")
        layout.separator()
        self.node_operator(layout, "GeometryNodeRealizeInstances", search_weight=1.0)
        self.node_operator(layout, "GeometryNodeRotateInstances")
        self.node_operator(layout, "GeometryNodeScaleInstances")
        self.node_operator(layout, "GeometryNodeTranslateInstances")
        self.node_operator(layout, "GeometryNodeSetInstanceTransform")
        layout.separator()
        self.node_operator(layout, "GeometryNodeInputInstanceBounds")
        self.node_operator(layout, "GeometryNodeInstanceTransform")
        self.node_operator(layout, "GeometryNodeInputInstanceRotation")
        self.node_operator(layout, "GeometryNodeInputInstanceScale")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_material_base(Menu):
    bl_label = "Material"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeReplaceMaterial")
        layout.separator()
        self.node_operator(layout, "GeometryNodeInputMaterialIndex")
        self.node_operator(layout, "GeometryNodeMaterialSelection")
        layout.separator()
        self.node_operator(layout, "GeometryNodeSetMaterial", search_weight=1.0)
        self.node_operator(layout, "GeometryNodeSetMaterialIndex")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_mesh_base(Menu):
    bl_label = "Mesh"

    def draw(self, _context):
        layout = self.layout
        self.draw_menu(layout, path="Mesh/Read")
        self.draw_menu(layout, path="Mesh/Sample")
        self.draw_menu(layout, path="Mesh/Write")
        layout.separator()
        self.draw_menu(layout, path="Mesh/Operations")
        self.draw_menu(layout, path="Mesh/Primitives")
        self.draw_menu(layout, path="Mesh/Topology")
        self.draw_menu(layout, path="Mesh/UV")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_mesh_read_base(Menu):
    bl_label = "Read"
    menu_path = "Mesh/Read"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeInputMeshEdgeAngle")
        self.node_operator(layout, "GeometryNodeInputMeshEdgeNeighbors")
        self.node_operator(layout, "GeometryNodeInputMeshEdgeVertices")
        self.node_operator(layout, "GeometryNodeEdgesToFaceGroups")
        self.node_operator(layout, "GeometryNodeInputMeshFaceArea")
        self.node_operator(layout, "GeometryNodeMeshFaceSetBoundaries")
        self.node_operator(layout, "GeometryNodeInputMeshFaceNeighbors")
        if context.space_data.geometry_nodes_type == 'TOOL':
            self.node_operator(layout, "GeometryNodeToolFaceSet")
        self.node_operator(layout, "GeometryNodeInputMeshFaceIsPlanar")
        self.node_operator(layout, "GeometryNodeInputShadeSmooth")
        self.node_operator(layout, "GeometryNodeInputEdgeSmooth")
        self.node_operator(layout, "GeometryNodeInputMeshIsland")
        self.node_operator(layout, "GeometryNodeInputShortestEdgePaths")
        self.node_operator(layout, "GeometryNodeInputMeshVertexNeighbors")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_mesh_sample_base(Menu):
    bl_label = "Sample"
    menu_path = "Mesh/Sample"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeSampleNearestSurface")
        self.node_operator(layout, "GeometryNodeSampleUVSurface")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_mesh_write_base(Menu):
    bl_label = "Write"
    menu_path = "Mesh/Write"

    def draw(self, context):
        layout = self.layout
        if context.space_data.geometry_nodes_type == 'TOOL':
            self.node_operator(layout, "GeometryNodeToolSetFaceSet")
        self.node_operator(layout, "GeometryNodeSetMeshNormal")
        self.node_operator(layout, "GeometryNodeSetShadeSmooth")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_mesh_operations_base(Menu):
    bl_label = "Operations"
    menu_path = "Mesh/Operations"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeDualMesh")
        self.node_operator(layout, "GeometryNodeEdgePathsToCurves")
        self.node_operator(layout, "GeometryNodeEdgePathsToSelection")
        self.node_operator(layout, "GeometryNodeExtrudeMesh")
        self.node_operator(layout, "GeometryNodeFlipFaces")
        self.node_operator(layout, "GeometryNodeMeshBoolean")
        self.node_operator(layout, "GeometryNodeMeshToCurve")
        if context.preferences.experimental.use_new_volume_nodes:
            self.node_operator(layout, "GeometryNodeMeshToDensityGrid")
        self.node_operator(layout, "GeometryNodeMeshToPoints")
        if context.preferences.experimental.use_new_volume_nodes:
            self.node_operator(layout, "GeometryNodeMeshToSDFGrid")
        self.node_operator(layout, "GeometryNodeMeshToVolume")
        self.node_operator(layout, "GeometryNodeScaleElements")
        self.node_operator(layout, "GeometryNodeSplitEdges")
        self.node_operator(layout, "GeometryNodeSubdivideMesh")
        self.node_operator(layout, "GeometryNodeSubdivisionSurface")
        self.node_operator(layout, "GeometryNodeTriangulate")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_mesh_primitives_base(Menu):
    bl_label = "Primitives"
    menu_path = "Mesh/Primitives"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeMeshCone")
        self.node_operator(layout, "GeometryNodeMeshCube")
        self.node_operator(layout, "GeometryNodeMeshCylinder")
        self.node_operator(layout, "GeometryNodeMeshGrid")
        self.node_operator(layout, "GeometryNodeMeshIcoSphere")
        self.node_operator(layout, "GeometryNodeMeshCircle")
        self.node_operator(layout, "GeometryNodeMeshLine")
        self.node_operator(layout, "GeometryNodeMeshUVSphere")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_input_import_base(Menu):
    bl_label = "Import"
    menu_path = "Input/Import"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeImportCSV", label="CSV (.csv)")
        self.node_operator(layout, "GeometryNodeImportOBJ", label="Wavefront (.obj)")
        self.node_operator(layout, "GeometryNodeImportPLY", label="Stanford PLY (.ply)")
        self.node_operator(layout, "GeometryNodeImportSTL", label="STL (.stl)")
        self.node_operator(layout, "GeometryNodeImportText", label="Text (.txt)")
        self.node_operator(layout, "GeometryNodeImportVDB", label="OpenVDB (.vdb)")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_mesh_topology_base(Menu):
    bl_label = "Topology"
    menu_path = "Mesh/Topology"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeCornersOfEdge")
        self.node_operator(layout, "GeometryNodeCornersOfFace")
        self.node_operator(layout, "GeometryNodeCornersOfVertex")
        self.node_operator(layout, "GeometryNodeEdgesOfCorner")
        self.node_operator(layout, "GeometryNodeEdgesOfVertex")
        self.node_operator(layout, "GeometryNodeFaceOfCorner")
        self.node_operator(layout, "GeometryNodeOffsetCornerInFace")
        self.node_operator(layout, "GeometryNodeVertexOfCorner")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_output_base(Menu):
    bl_label = "Output"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "NodeGroupOutput")
        self.node_operator(layout, "GeometryNodeViewer")
        self.node_operator_with_searchable_enum(context, layout, "GeometryNodeWarning", "warning_type")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_point_base(Menu):
    bl_label = "Point"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeDistributePointsInVolume")
        if context.preferences.experimental.use_new_volume_nodes:
            self.node_operator(layout, "GeometryNodeDistributePointsInGrid")
        self.node_operator(layout, "GeometryNodeDistributePointsOnFaces")
        layout.separator()
        self.node_operator(layout, "GeometryNodePoints")
        self.node_operator(layout, "GeometryNodePointsToCurves")
        if context.preferences.experimental.use_new_volume_nodes:
            self.node_operator(layout, "GeometryNodePointsToSDFGrid")
        self.node_operator(layout, "GeometryNodePointsToVertices")
        self.node_operator(layout, "GeometryNodePointsToVolume")
        layout.separator()
        self.node_operator(layout, "GeometryNodeSetPointRadius")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_simulation_base(Menu):
    bl_label = "Simulation"

    def draw(self, _context):
        layout = self.layout
        self.simulation_zone(layout, label="Simulation")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_utilities_text_base(Menu):
    bl_label = "Text"
    menu_path = "Utilities/Text"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "FunctionNodeFormatString")
        self.node_operator(layout, "GeometryNodeStringJoin")
        self.node_operator(layout, "FunctionNodeMatchString")
        self.node_operator(layout, "FunctionNodeReplaceString")
        self.node_operator(layout, "FunctionNodeSliceString")
        layout.separator()
        self.node_operator(layout, "FunctionNodeFindInString")
        self.node_operator(layout, "FunctionNodeStringLength")
        self.node_operator(layout, "GeometryNodeStringToCurves")
        self.node_operator(layout, "FunctionNodeValueToString")
        layout.separator()
        self.node_operator(layout, "FunctionNodeInputSpecialCharacters")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_texture_base(Menu):
    bl_label = "Texture"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "ShaderNodeTexBrick")
        self.node_operator(layout, "ShaderNodeTexChecker")
        self.node_operator(layout, "ShaderNodeTexGabor")
        self.node_operator(layout, "ShaderNodeTexGradient")
        self.node_operator(layout, "GeometryNodeImageTexture")
        self.node_operator(layout, "ShaderNodeTexMagic")
        self.node_operator(layout, "ShaderNodeTexNoise")
        self.node_operator(layout, "ShaderNodeTexVoronoi")
        self.node_operator(layout, "ShaderNodeTexWave")
        self.node_operator(layout, "ShaderNodeTexWhiteNoise")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_utilities_base(Menu):
    bl_label = "Utilities"

    def draw(self, context):
        layout = self.layout
        self.draw_menu(layout, path="Utilities/Color")
        self.draw_menu(layout, path="Utilities/Text")
        self.draw_menu(layout, path="Utilities/Vector")
        layout.separator()
        self.draw_menu(layout, path="Utilities/Bundle")
        self.draw_menu(layout, path="Utilities/Closure")
        self.draw_menu(layout, path="Utilities/Field")
        self.draw_menu(layout, path="Utilities/Math")
        if context.preferences.experimental.use_geometry_nodes_lists:
            self.draw_menu(layout, path="Utilities/List")
        self.draw_menu(layout, path="Utilities/Matrix")
        self.draw_menu(layout, path="Utilities/Rotation")
        self.draw_menu(layout, path="Utilities/Deprecated")
        layout.separator()
        self.for_each_element_zone(layout, label="For Each Element")
        self.node_operator(layout, "GeometryNodeIndexSwitch")
        self.node_operator(layout, "GeometryNodeMenuSwitch")
        self.node_operator(layout, "FunctionNodeRandomValue")
        self.repeat_zone(layout, label="Repeat")
        self.node_operator(layout, "GeometryNodeSwitch")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_utilities_deprecated_base(Menu):
    bl_label = "Deprecated"
    menu_path = "Utilities/Deprecated"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "FunctionNodeAlignEulerToVector")
        self.node_operator(layout, "FunctionNodeRotateEuler")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_utilities_field_base(Menu):
    bl_label = "Field"
    menu_path = "Utilities/Field"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeAccumulateField")
        self.node_operator(layout, "GeometryNodeFieldAtIndex")
        self.node_operator(layout, "GeometryNodeFieldOnDomain")
        self.node_operator(layout, "GeometryNodeFieldAverage")
        self.node_operator(layout, "GeometryNodeFieldMinAndMax")
        self.node_operator(layout, "GeometryNodeFieldVariance")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_utilities_rotation_base(Menu):
    bl_label = "Rotation"
    menu_path = "Utilities/Rotation"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "FunctionNodeAlignRotationToVector")
        self.node_operator(layout, "FunctionNodeAxesToRotation")
        self.node_operator(layout, "FunctionNodeAxisAngleToRotation")
        self.node_operator(layout, "FunctionNodeEulerToRotation")
        self.node_operator(layout, "FunctionNodeInvertRotation")
        self.node_operator(layout, "FunctionNodeRotateRotation")
        self.node_operator(layout, "FunctionNodeRotateVector")
        self.node_operator(layout, "FunctionNodeRotationToAxisAngle")
        self.node_operator(layout, "FunctionNodeRotationToEuler")
        self.node_operator(layout, "FunctionNodeRotationToQuaternion")
        self.node_operator(layout, "FunctionNodeQuaternionToRotation")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_utilities_matrix_base(Menu):
    bl_label = "Matrix"
    menu_path = "Utilities/Matrix"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "FunctionNodeCombineMatrix")
        self.node_operator(layout, "FunctionNodeCombineTransform")
        self.node_operator(layout, "FunctionNodeMatrixDeterminant", label="Determinant")
        self.node_operator(layout, "FunctionNodeInvertMatrix")
        self.node_operator(layout, "FunctionNodeMatrixMultiply")
        self.node_operator(layout, "FunctionNodeProjectPoint")
        self.node_operator(layout, "FunctionNodeSeparateMatrix")
        self.node_operator(layout, "FunctionNodeSeparateTransform")
        self.node_operator(layout, "FunctionNodeTransformDirection")
        self.node_operator(layout, "FunctionNodeTransformPoint")
        self.node_operator(layout, "FunctionNodeTransposeMatrix")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_category_utilities_bundle_base(Menu):
    bl_label = "Bundle"
    menu_path = "Utilities/Bundle"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "NodeCombineBundle")
        self.node_operator(layout, "NodeSeparateBundle")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_category_utilities_closure_base(Menu):
    bl_label = "Closure"
    menu_path = "Utilities/Closure"

    def draw(self, context):
        layout = self.layout
        self.closure_zone(layout, label="Closure")
        self.node_operator(layout, "NodeEvaluateClosure")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_utilities_list_base(Menu):
    bl_label = "List"
    menu_path = "Utilities/List"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeList")
        self.node_operator(layout, "GeometryNodeListGetItem")
        self.node_operator(layout, "GeometryNodeListLength")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_utilities_math_base(Menu):
    bl_label = "Math"
    menu_path = "Utilities/Math"

    def draw(self, context):
        layout = self.layout
        self.node_operator_with_searchable_enum(
            context, layout, "FunctionNodeBitMath", "operation", search_weight=-1.0,
        )
        self.node_operator_with_searchable_enum(context, layout, "FunctionNodeBooleanMath", "operation")
        self.node_operator_with_searchable_enum(context, layout, "FunctionNodeIntegerMath", "operation")
        self.node_operator(layout, "ShaderNodeClamp")
        self.node_operator(layout, "FunctionNodeCompare")
        self.node_operator(layout, "ShaderNodeFloatCurve")
        self.node_operator(layout, "FunctionNodeFloatToInt")
        self.node_operator(layout, "FunctionNodeHashValue")
        self.node_operator(layout, "ShaderNodeMapRange")
        self.node_operator_with_searchable_enum(context, layout, "ShaderNodeMath", "operation")
        self.node_operator(layout, "ShaderNodeMix")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_mesh_uv_base(Menu):
    bl_label = "UV"
    menu_path = "Mesh/UV"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeUVPackIslands")
        self.node_operator(layout, "GeometryNodeUVUnwrap")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_utilities_vector_base(Menu):
    bl_label = "Vector"
    menu_path = "Utilities/Vector"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "ShaderNodeVectorCurve")
        self.node_operator_with_searchable_enum(context, layout, "ShaderNodeVectorMath", "operation")
        self.node_operator(layout, "ShaderNodeVectorRotate")
        layout.separator()
        self.node_operator(layout, "ShaderNodeCombineXYZ")
        props = self.node_operator(layout, "ShaderNodeMix", label="Mix Vector")
        ops = props.settings.add()
        ops.name = "data_type"
        ops.value = "'VECTOR'"
        self.node_operator(layout, "ShaderNodeSeparateXYZ")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_volume_base(Menu):
    bl_label = "Volume"
    bl_translation_context = i18n_contexts.id_id

    def draw(self, context):
        layout = self.layout
        if context.preferences.experimental.use_new_volume_nodes:
            self.draw_menu(layout, path="Volume/Read")
            self.draw_menu(layout, path="Volume/Sample")
            self.draw_menu(layout, path="Volume/Write")
            layout.separator()
        self.draw_menu(layout, path="Volume/Operations")
        self.draw_menu(layout, path="Volume/Primitives")
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_gn_volume_read_base(Menu):
    bl_label = "Read"
    menu_path = "Volume/Read"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeGetNamedGrid")
        self.node_operator(layout, "GeometryNodeGridInfo")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_volume_write_base(Menu):
    bl_label = "Write"
    menu_path = "Volume/Write"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeStoreNamedGrid")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_volume_sample_base(Menu):
    bl_label = "Sample"
    menu_path = "Volume/Sample"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeSampleGrid")
        self.node_operator(layout, "GeometryNodeSampleGridIndex")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_volume_operations_base(Menu):
    bl_label = "Operations"
    menu_path = "Volume/Operations"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeVolumeToMesh")
        if context.preferences.experimental.use_new_volume_nodes:
            self.node_operator(layout, "GeometryNodeGridToMesh")
            self.node_operator(layout, "GeometryNodeSDFGridBoolean")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_gn_volume_primitives_base(Menu):
    bl_label = "Primitives"
    menu_path = "Volume/Primitives"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "GeometryNodeVolumeCube")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


add_menus = {
    # menu bl_idname : baseclass
    "NODE_MT_geometry_node_GEO_ATTRIBUTE" : NODE_MT_gn_attribute_base,
    "NODE_MT_geometry_node_GEO_INPUT" : NODE_MT_gn_input_base,
    "NODE_MT_geometry_node_GEO_INPUT_CONSTANT" : NODE_MT_gn_input_constant_base,
    "NODE_MT_geometry_node_GEO_INPUT_GIZMO" : NODE_MT_gn_input_gizmo_base,
    "NODE_MT_geometry_node_GEO_INPUT_GROUP" : NODE_MT_gn_input_group_base,
    "NODE_MT_category_import" : NODE_MT_gn_input_import_base,
    "NODE_MT_geometry_node_GEO_INPUT_SCENE" : NODE_MT_gn_input_scene_base,
    "NODE_MT_category_GEO_OUTPUT" : NODE_MT_gn_output_base,
    "NODE_MT_geometry_node_GEO_CURVE" : NODE_MT_gn_curve_base,
    "NODE_MT_geometry_node_GEO_CURVE_READ" : NODE_MT_gn_curve_read_base,
    "NODE_MT_geometry_node_GEO_CURVE_SAMPLE" : NODE_MT_gn_curve_sample_base,
    "NODE_MT_geometry_node_GEO_CURVE_WRITE" : NODE_MT_gn_curve_write_base,
    "NODE_MT_geometry_node_GEO_CURVE_OPERATIONS" : NODE_MT_gn_curve_operations_base,
    "NODE_MT_geometry_node_GEO_PRIMITIVES_CURVE" : NODE_MT_gn_curve_primitives_base,
    "NODE_MT_geometry_node_curve_topology" : NODE_MT_gn_curve_topology_base,
    "NODE_MT_geometry_node_grease_pencil" : NODE_MT_gn_grease_pencil_base,
    "NODE_MT_geometry_node_grease_pencil_read" : NODE_MT_gn_grease_pencil_read_base,
    "NODE_MT_geometry_node_grease_pencil_write" : NODE_MT_gn_grease_pencil_write_base,
    "NODE_MT_geometry_node_grease_pencil_operations" : NODE_MT_gn_grease_pencil_operations_base,
    "NODE_MT_geometry_node_GEO_GEOMETRY" : NODE_MT_gn_geometry_base,
    "NODE_MT_geometry_node_GEO_GEOMETRY_READ" : NODE_MT_gn_geometry_read_base,
    "NODE_MT_geometry_node_GEO_GEOMETRY_WRITE" : NODE_MT_gn_geometry_write_base,
    "NODE_MT_geometry_node_GEO_GEOMETRY_OPERATIONS" : NODE_MT_gn_geometry_operations_base,
    "NODE_MT_geometry_node_GEO_GEOMETRY_SAMPLE" : NODE_MT_gn_geometry_sample_base,
    "NODE_MT_geometry_node_GEO_INSTANCE" : NODE_MT_gn_instance_base,
    "NODE_MT_geometry_node_GEO_MESH" : NODE_MT_gn_mesh_base,
    "NODE_MT_geometry_node_GEO_MESH_READ" : NODE_MT_gn_mesh_read_base,
    "NODE_MT_geometry_node_GEO_MESH_SAMPLE" : NODE_MT_gn_mesh_sample_base,
    "NODE_MT_geometry_node_GEO_MESH_WRITE" : NODE_MT_gn_mesh_write_base,
    "NODE_MT_geometry_node_GEO_MESH_OPERATIONS" : NODE_MT_gn_mesh_operations_base,
    "NODE_MT_category_PRIMITIVES_MESH" : NODE_MT_gn_mesh_uv_base,
    "NODE_MT_geometry_node_mesh_topology" : NODE_MT_gn_mesh_topology_base,
    "NODE_MT_category_GEO_UV" : NODE_MT_gn_mesh_primitives_base,
    "NODE_MT_category_GEO_POINT" : NODE_MT_gn_point_base,
    "NODE_MT_category_simulation" : NODE_MT_gn_simulation_base,
    "NODE_MT_category_GEO_VOLUME" : NODE_MT_gn_volume_base,
    "NODE_MT_geometry_node_GEO_VOLUME_READ" : NODE_MT_gn_volume_read_base,
    "NODE_MT_geometry_node_volume_sample" : NODE_MT_gn_volume_sample_base,
    "NODE_MT_geometry_node_GEO_VOLUME_WRITE" : NODE_MT_gn_volume_write_base,
    "NODE_MT_geometry_node_GEO_VOLUME_OPERATIONS" : NODE_MT_gn_volume_operations_base,
    "NODE_MT_geometry_node_GEO_VOLUME_PRIMITIVES" : NODE_MT_gn_volume_primitives_base,
    "NODE_MT_geometry_node_GEO_MATERIAL" : NODE_MT_gn_material_base,
    "NODE_MT_category_GEO_TEXTURE" : NODE_MT_gn_texture_base,
    "NODE_MT_category_GEO_UTILITIES" : NODE_MT_gn_utilities_base,
    "NODE_MT_geometry_node_GEO_COLOR" : NODE_MT_gn_utilities_color_base,
    "NODE_MT_category_GEO_TEXT" : NODE_MT_gn_utilities_text_base,
    "NODE_MT_category_GEO_VECTOR" : NODE_MT_gn_utilities_vector_base,
    "NODE_MT_category_utilities_bundle" : NODE_MT_category_utilities_bundle_base,
    "NODE_MT_category_utilities_closure" : NODE_MT_category_utilities_closure_base,
    "NODE_MT_category_GEO_UTILITIES_FIELD" : NODE_MT_gn_utilities_field_base,
    "NODE_MT_category_GEO_UTILITIES_MATH" : NODE_MT_gn_utilities_math_base,
    "NODE_MT_category_GEO_UTILITIES_ROTATION" : NODE_MT_gn_utilities_rotation_base,
    "NODE_MT_category_utilities_list" : NODE_MT_gn_utilities_list_base,
    "NODE_MT_category_utilities_matrix" : NODE_MT_gn_utilities_matrix_base,
    "NODE_MT_category_GEO_UTILITIES_DEPRECATED" : NODE_MT_gn_utilities_deprecated_base,
}
add_menus = node_add_menu.generate_menus(add_menus, template=node_add_menu.AddNodeMenu)


class NODE_MT_geometry_node_add_all(Menu):
    bl_label = ""

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_geometry_node_GEO_ATTRIBUTE")
        layout.menu("NODE_MT_geometry_node_GEO_INPUT")
        layout.menu("NODE_MT_category_GEO_OUTPUT")
        layout.separator()
        layout.menu("NODE_MT_geometry_node_GEO_GEOMETRY")
        layout.separator()
        layout.menu("NODE_MT_geometry_node_GEO_CURVE")
        layout.menu("NODE_MT_geometry_node_grease_pencil")
        layout.menu("NODE_MT_geometry_node_GEO_INSTANCE")
        layout.menu("NODE_MT_geometry_node_GEO_MESH")
        layout.menu("NODE_MT_category_GEO_POINT")
        layout.menu("NODE_MT_category_GEO_VOLUME")
        layout.separator()
        layout.menu("NODE_MT_category_simulation")
        layout.separator()
        layout.menu("NODE_MT_geometry_node_GEO_MATERIAL")
        layout.menu("NODE_MT_category_GEO_TEXTURE")
        layout.menu("NODE_MT_category_GEO_UTILITIES")
        layout.separator()
        layout.menu("NODE_MT_group_add")
        layout.menu("NODE_MT_category_layout")
        node_add_menu.draw_root_assets(layout)


swap_menus = {
    # menu bl_idname : baseclass
    "NODE_MT_gn_attribute_swap" : NODE_MT_gn_attribute_base,
    "NODE_MT_gn_input_swap" : NODE_MT_gn_input_base,
    "NODE_MT_gn_input_constant_swap" : NODE_MT_gn_input_constant_base,
    "NODE_MT_gn_input_gizmo_swap" : NODE_MT_gn_input_gizmo_base,
    "NODE_MT_gn_input_group_swap" : NODE_MT_gn_input_group_base,
    "NODE_MT_gn_input_import_swap" : NODE_MT_gn_input_import_base,
    "NODE_MT_gn_input_scene_swap" : NODE_MT_gn_input_scene_base,
    "NODE_MT_gn_output_swap" : NODE_MT_gn_output_base,
    "NODE_MT_gn_curve_swap" : NODE_MT_gn_curve_base,
    "NODE_MT_gn_curve_read_swap" : NODE_MT_gn_curve_read_base,
    "NODE_MT_gn_curve_sample_swap" : NODE_MT_gn_curve_sample_base,
    "NODE_MT_gn_curve_write_swap" : NODE_MT_gn_curve_write_base,
    "NODE_MT_gn_curve_operations_swap" : NODE_MT_gn_curve_operations_base,
    "NODE_MT_gn_curve_primitives_swap" : NODE_MT_gn_curve_primitives_base,
    "NODE_MT_gn_curve_topology_swap" : NODE_MT_gn_curve_topology_base,
    "NODE_MT_gn_grease_pencil_swap" : NODE_MT_gn_grease_pencil_base,
    "NODE_MT_gn_grease_pencil_read_swap" : NODE_MT_gn_grease_pencil_read_base,
    "NODE_MT_gn_grease_pencil_write_swap" : NODE_MT_gn_grease_pencil_write_base,
    "NODE_MT_gn_grease_pencil_operations_swap" : NODE_MT_gn_grease_pencil_operations_base,
    "NODE_MT_gn_geometry_swap" : NODE_MT_gn_geometry_base,
    "NODE_MT_gn_geometry_read_swap" : NODE_MT_gn_geometry_read_base,
    "NODE_MT_gn_geometry_write_swap" : NODE_MT_gn_geometry_write_base,
    "NODE_MT_gn_geometry_operations_swap" : NODE_MT_gn_geometry_operations_base,
    "NODE_MT_gn_geometry_sample_swap" : NODE_MT_gn_geometry_sample_base,
    "NODE_MT_gn_instance_swap" : NODE_MT_gn_instance_base,
    "NODE_MT_gn_mesh_swap" : NODE_MT_gn_mesh_base,
    "NODE_MT_gn_mesh_read_swap" : NODE_MT_gn_mesh_read_base,
    "NODE_MT_gn_mesh_sample_swap" : NODE_MT_gn_mesh_sample_base,
    "NODE_MT_gn_mesh_write_swap" : NODE_MT_gn_mesh_write_base,
    "NODE_MT_gn_mesh_operations_swap" : NODE_MT_gn_mesh_operations_base,
    "NODE_MT_gn_mesh_uv_swap" : NODE_MT_gn_mesh_uv_base,
    "NODE_MT_gn_mesh_topology_swap" : NODE_MT_gn_mesh_topology_base,
    "NODE_MT_gn_mesh_primitives_swap" : NODE_MT_gn_mesh_primitives_base,
    "NODE_MT_gn_point_swap" : NODE_MT_gn_point_base,
    "NODE_MT_gn_simulation_swap" : NODE_MT_gn_simulation_base,
    "NODE_MT_gn_volume_swap" : NODE_MT_gn_volume_base,
    "NODE_MT_gn_volume_read_swap" : NODE_MT_gn_volume_read_base,
    "NODE_MT_gn_volume_sample_swap" : NODE_MT_gn_volume_sample_base,
    "NODE_MT_gn_volume_write_swap" : NODE_MT_gn_volume_write_base,
    "NODE_MT_gn_volume_operations_swap" : NODE_MT_gn_volume_operations_base,
    "NODE_MT_gn_volume_primitives_swap" : NODE_MT_gn_volume_primitives_base,
    "NODE_MT_gn_material_swap" : NODE_MT_gn_material_base,
    "NODE_MT_gn_texture_swap" : NODE_MT_gn_texture_base,
    "NODE_MT_gn_utilities_swap" : NODE_MT_gn_utilities_base,
    "NODE_MT_gn_utilities_color_swap" : NODE_MT_gn_utilities_color_base,
    "NODE_MT_gn_utilities_text_swap" : NODE_MT_gn_utilities_text_base,
    "NODE_MT_gn_utilities_vector_swap" : NODE_MT_gn_utilities_vector_base,
    "NODE_MT_gn_utilities_bundle_swap" : NODE_MT_category_utilities_bundle_base,
    "NODE_MT_gn_utilities_closure_swap" : NODE_MT_category_utilities_closure_base,
    "NODE_MT_gn_utilities_field_swap" : NODE_MT_gn_utilities_field_base,
    "NODE_MT_gn_utilities_math_swap" : NODE_MT_gn_utilities_math_base,
    "NODE_MT_gn_utilities_rotation_swap" : NODE_MT_gn_utilities_rotation_base,
    "NODE_MT_gn_utilities_list_swap" : NODE_MT_gn_utilities_list_base,
    "NODE_MT_gn_utilities_matrix_swap" : NODE_MT_gn_utilities_matrix_base,
    "NODE_MT_gn_utilities_deprecated_swap" : NODE_MT_gn_utilities_deprecated_base,
}
swap_menus = node_add_menu.generate_menus(swap_menus, template=node_add_menu.SwapNodeMenu)


class NODE_MT_geometry_node_swap_all(Menu):
    bl_label = ""

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_gn_attribute_swap")
        layout.menu("NODE_MT_gn_input_swap")
        layout.menu("NODE_MT_gn_output_swap")
        layout.separator()
        layout.menu("NODE_MT_gn_curve_swap")
        layout.separator()
        layout.menu("NODE_MT_gn_grease_pencil_swap")
        layout.menu("NODE_MT_gn_geometry_swap")
        layout.menu("NODE_MT_gn_instance_swap")
        layout.menu("NODE_MT_gn_mesh_swap")
        layout.menu("NODE_MT_gn_point_swap")
        layout.menu("NODE_MT_gn_simulation_swap")
        layout.separator()
        layout.menu("NODE_MT_gn_volume_swap")
        layout.separator()
        layout.menu("NODE_MT_gn_material_swap")
        layout.menu("NODE_MT_gn_texture_swap")
        layout.menu("NODE_MT_gn_utilities_swap")
        layout.separator()
        layout.menu("NODE_MT_group_swap")
        layout.menu("NODE_MT_layout_swap")
        #node_add_menu.draw_root_assets(layout)


classes = (
    NODE_MT_geometry_node_add_all,
    *add_menus,
    NODE_MT_geometry_node_swap_all,
    *swap_menus,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
