# SPDX-FileCopyrightText: 2022-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
import bpy
from bpy.types import Menu
from bl_ui import node_add_menu
from bpy.app.translations import (
    pgettext_iface as iface_,
    contexts as i18n_contexts,
)


def override_add_node_type(
    layout, node_type, *, label=None, poll=None, search_weight=0.0
):
    """Add a node type to a menu."""
    bl_rna = bpy.types.Node.bl_rna_get_subclass(node_type)
    if not label:
        label = bl_rna.name if bl_rna else iface_("Unknown")

    if poll is True or poll is None:
        translation_context = (
            bl_rna.translation_context if bl_rna else i18n_contexts.default
        )
        props = layout.operator(
            "node.swap_node",
            text=label,
            text_ctxt=translation_context,
            search_weight=search_weight,
        )
        props.type = node_type
        props.use_transform = False
        return props


class NODE_MT_dummy_geometry_node_GEO_ATTRIBUTE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_ATTRIBUTE"
    bl_label = "Attribute"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeAttributeStatistic")
        override_add_node_type(layout, "GeometryNodeAttributeDomainSize")
        layout.separator()
        override_add_node_type(layout, "GeometryNodeBlurAttribute")
        override_add_node_type(layout, "GeometryNodeCaptureAttribute")
        override_add_node_type(layout, "GeometryNodeRemoveAttribute")
        override_add_node_type(
            layout, "GeometryNodeStoreNamedAttribute", search_weight=1.0
        )
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_geometry_node_GEO_COLOR(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_COLOR"
    bl_label = "Color"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "ShaderNodeBlackbody")
        override_add_node_type(layout, "ShaderNodeValToRGB")
        override_add_node_type(layout, "ShaderNodeRGBCurve")
        layout.separator()
        override_add_node_type(layout, "FunctionNodeCombineColor")
        props = override_add_node_type(
            layout, "ShaderNodeMix", label=iface_("Mix Color")
        )
        ops = props.settings.add()
        ops.name = "data_type"
        ops.value = "'RGBA'"
        override_add_node_type(layout, "FunctionNodeSeparateColor")
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Color")


class NODE_MT_dummy_geometry_node_GEO_CURVE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_CURVE"
    bl_label = "Curve"

    def draw(self, _context):
        layout = self.layout
        layout.menu("NODE_MT_dummy_geometry_node_GEO_CURVE_READ")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_CURVE_SAMPLE")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_CURVE_WRITE")
        layout.separator()
        layout.menu("NODE_MT_dummy_geometry_node_GEO_CURVE_OPERATIONS")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_PRIMITIVES_CURVE")
        layout.menu("NODE_MT_dummy_geometry_node_curve_topology")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_geometry_node_GEO_CURVE_READ(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_CURVE_READ"
    bl_label = "Read"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeInputCurveHandlePositions")
        override_add_node_type(layout, "GeometryNodeCurveLength")
        override_add_node_type(layout, "GeometryNodeInputTangent")
        override_add_node_type(layout, "GeometryNodeInputCurveTilt")
        override_add_node_type(layout, "GeometryNodeCurveEndpointSelection")
        override_add_node_type(layout, "GeometryNodeCurveHandleTypeSelection")
        override_add_node_type(layout, "GeometryNodeInputSplineCyclic")
        override_add_node_type(layout, "GeometryNodeSplineLength")
        override_add_node_type(layout, "GeometryNodeSplineParameter")
        override_add_node_type(layout, "GeometryNodeInputSplineResolution")
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Read")


class NODE_MT_dummy_geometry_node_GEO_CURVE_SAMPLE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_CURVE_SAMPLE"
    bl_label = "Sample"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeSampleCurve")
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Sample")


class NODE_MT_dummy_geometry_node_GEO_CURVE_WRITE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_CURVE_WRITE"
    bl_label = "Write"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeSetCurveNormal")
        override_add_node_type(layout, "GeometryNodeSetCurveRadius")
        override_add_node_type(layout, "GeometryNodeSetCurveTilt")
        override_add_node_type(layout, "GeometryNodeSetCurveHandlePositions")
        override_add_node_type(layout, "GeometryNodeCurveSetHandles")
        override_add_node_type(layout, "GeometryNodeSetSplineCyclic")
        override_add_node_type(layout, "GeometryNodeSetSplineResolution")
        override_add_node_type(layout, "GeometryNodeCurveSplineType")
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Write")


class NODE_MT_dummy_geometry_node_GEO_CURVE_OPERATIONS(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_CURVE_OPERATIONS"
    bl_label = "Operations"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeCurvesToGreasePencil")
        override_add_node_type(layout, "GeometryNodeCurveToMesh")
        override_add_node_type(layout, "GeometryNodeCurveToPoints")
        override_add_node_type(layout, "GeometryNodeDeformCurvesOnSurface")
        override_add_node_type(layout, "GeometryNodeFillCurve")
        override_add_node_type(layout, "GeometryNodeFilletCurve")
        override_add_node_type(layout, "GeometryNodeGreasePencilToCurves")
        override_add_node_type(layout, "GeometryNodeInterpolateCurves")
        override_add_node_type(layout, "GeometryNodeMergeLayers")
        override_add_node_type(layout, "GeometryNodeResampleCurve")
        override_add_node_type(layout, "GeometryNodeReverseCurve")
        override_add_node_type(layout, "GeometryNodeSubdivideCurve")
        override_add_node_type(layout, "GeometryNodeTrimCurve")
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Operations")


class NODE_MT_dummy_geometry_node_GEO_PRIMITIVES_CURVE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_PRIMITIVES_CURVE"
    bl_label = "Primitives"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeCurveArc")
        override_add_node_type(layout, "GeometryNodeCurvePrimitiveBezierSegment")
        override_add_node_type(layout, "GeometryNodeCurvePrimitiveCircle")
        override_add_node_type(layout, "GeometryNodeCurvePrimitiveLine")
        override_add_node_type(layout, "GeometryNodeCurveSpiral")
        override_add_node_type(layout, "GeometryNodeCurveQuadraticBezier")
        override_add_node_type(layout, "GeometryNodeCurvePrimitiveQuadrilateral")
        override_add_node_type(layout, "GeometryNodeCurveStar")
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Primitives")


class NODE_MT_dummy_geometry_node_curve_topology(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_curve_topology"
    bl_label = "Topology"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeCurveOfPoint")
        override_add_node_type(layout, "GeometryNodeOffsetPointInCurve")
        override_add_node_type(layout, "GeometryNodePointsOfCurve")
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Topology")


class NODE_MT_dummy_geometry_node_GEO_GEOMETRY(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_GEOMETRY"
    bl_label = "Geometry"

    def draw(self, _context):
        layout = self.layout
        layout.menu("NODE_MT_dummy_geometry_node_GEO_GEOMETRY_READ")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_GEOMETRY_SAMPLE")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_GEOMETRY_WRITE")
        layout.separator()
        layout.menu("NODE_MT_dummy_geometry_node_GEO_GEOMETRY_OPERATIONS")
        layout.separator()
        override_add_node_type(layout, "GeometryNodeGeometryToInstance")
        override_add_node_type(
            layout, "GeometryNodeJoinGeometry", search_weight=1.0
        )
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_geometry_node_GEO_GEOMETRY_READ(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_GEOMETRY_READ"
    bl_label = "Read"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeInputID")
        override_add_node_type(layout, "GeometryNodeInputIndex")
        override_add_node_type(
            layout, "GeometryNodeInputNamedAttribute", search_weight=1.0
        )
        override_add_node_type(layout, "GeometryNodeInputNormal")
        override_add_node_type(
            layout, "GeometryNodeInputPosition", search_weight=1.0
        )
        override_add_node_type(layout, "GeometryNodeInputRadius")
        if context.space_data.geometry_nodes_type == "TOOL":
            override_add_node_type(layout, "GeometryNodeToolSelection")
            override_add_node_type(layout, "GeometryNodeToolActiveElement")
        node_add_menu.draw_assets_for_catalog(layout, "Geometry/Read")


class NODE_MT_dummy_geometry_node_GEO_GEOMETRY_WRITE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_GEOMETRY_WRITE"
    bl_label = "Write"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeSetGeometryName")
        override_add_node_type(layout, "GeometryNodeSetID")
        override_add_node_type(
            layout, "GeometryNodeSetPosition", search_weight=1.0
        )
        if context.space_data.geometry_nodes_type == "TOOL":
            override_add_node_type(layout, "GeometryNodeToolSetSelection")
        node_add_menu.draw_assets_for_catalog(layout, "Geometry/Write")


class NODE_MT_dummy_geometry_node_GEO_GEOMETRY_OPERATIONS(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_GEOMETRY_OPERATIONS"
    bl_label = "Operations"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeBake")
        override_add_node_type(layout, "GeometryNodeBoundBox")
        override_add_node_type(layout, "GeometryNodeConvexHull")
        override_add_node_type(layout, "GeometryNodeDeleteGeometry")
        override_add_node_type(layout, "GeometryNodeDuplicateElements")
        override_add_node_type(layout, "GeometryNodeMergeByDistance")
        override_add_node_type(layout, "GeometryNodeSortElements")
        override_add_node_type(layout, "GeometryNodeTransform", search_weight=1.0)
        layout.separator()
        override_add_node_type(layout, "GeometryNodeSeparateComponents")
        override_add_node_type(layout, "GeometryNodeSeparateGeometry")
        override_add_node_type(layout, "GeometryNodeSplitToInstances")
        node_add_menu.draw_assets_for_catalog(layout, "Geometry/Operations")


class NODE_MT_dummy_geometry_node_GEO_GEOMETRY_SAMPLE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_GEOMETRY_SAMPLE"
    bl_label = "Sample"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeProximity")
        override_add_node_type(layout, "GeometryNodeIndexOfNearest")
        override_add_node_type(layout, "GeometryNodeRaycast")
        override_add_node_type(layout, "GeometryNodeSampleIndex")
        override_add_node_type(layout, "GeometryNodeSampleNearest")
        node_add_menu.draw_assets_for_catalog(layout, "Geometry/Sample")


class NODE_MT_dummy_geometry_node_GEO_INPUT(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_INPUT"
    bl_label = "Input"

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_dummy_geometry_node_GEO_INPUT_CONSTANT")
        if context.space_data.geometry_nodes_type != "TOOL":
            layout.menu("NODE_MT_dummy_geometry_node_GEO_INPUT_GIZMO")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_INPUT_GROUP")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_INPUT_SCENE")
        if context.preferences.experimental.use_new_file_import_nodes:
            layout.menu("NODE_MT_dummy_category_import")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_geometry_node_GEO_INPUT_CONSTANT(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_INPUT_CONSTANT"
    bl_label = "Constant"
    bl_translation_context = i18n_contexts.id_nodetree

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "FunctionNodeInputBool")
        override_add_node_type(layout, "GeometryNodeInputCollection")
        override_add_node_type(layout, "FunctionNodeInputColor")
        override_add_node_type(layout, "GeometryNodeInputImage")
        override_add_node_type(layout, "FunctionNodeInputInt")
        override_add_node_type(layout, "GeometryNodeInputMaterial")
        override_add_node_type(layout, "GeometryNodeInputObject")
        override_add_node_type(layout, "FunctionNodeInputRotation")
        override_add_node_type(layout, "FunctionNodeInputString")
        override_add_node_type(layout, "ShaderNodeValue")
        override_add_node_type(layout, "FunctionNodeInputVector")
        node_add_menu.draw_assets_for_catalog(layout, "Input/Constant")


class NODE_MT_dummy_geometry_node_GEO_INPUT_GROUP(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_INPUT_GROUP"
    bl_label = "Group"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "NodeGroupInput")
        node_add_menu.draw_assets_for_catalog(layout, "Input/Group")


class NODE_MT_dummy_geometry_node_GEO_INPUT_SCENE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_INPUT_SCENE"
    bl_label = "Scene"

    def draw(self, context):
        layout = self.layout
        if context.space_data.geometry_nodes_type == "TOOL":
            override_add_node_type(layout, "GeometryNodeTool3DCursor")
        override_add_node_type(layout, "GeometryNodeInputActiveCamera")
        override_add_node_type(layout, "GeometryNodeCollectionInfo")
        override_add_node_type(layout, "GeometryNodeImageInfo")
        override_add_node_type(layout, "GeometryNodeIsViewport")
        override_add_node_type(layout, "GeometryNodeInputNamedLayerSelection")
        if context.space_data.geometry_nodes_type == "TOOL":
            override_add_node_type(layout, "GeometryNodeToolMousePosition")
        override_add_node_type(layout, "GeometryNodeObjectInfo")
        override_add_node_type(layout, "GeometryNodeInputSceneTime")
        override_add_node_type(layout, "GeometryNodeSelfObject")
        if context.space_data.geometry_nodes_type == "TOOL":
            override_add_node_type(layout, "GeometryNodeViewportTransform")
        node_add_menu.draw_assets_for_catalog(layout, "Input/Scene")


class NODE_MT_dummy_geometry_node_GEO_INPUT_GIZMO(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_INPUT_GIZMO"
    bl_label = "Gizmo"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeGizmoDial")
        override_add_node_type(layout, "GeometryNodeGizmoLinear")
        override_add_node_type(layout, "GeometryNodeGizmoTransform")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_geometry_node_GEO_INSTANCE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_INSTANCE"
    bl_label = "Instances"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(
            layout, "GeometryNodeInstanceOnPoints", search_weight=2.0
        )
        override_add_node_type(layout, "GeometryNodeInstancesToPoints")
        layout.separator()
        override_add_node_type(
            layout, "GeometryNodeRealizeInstances", search_weight=1.0
        )
        override_add_node_type(layout, "GeometryNodeRotateInstances")
        override_add_node_type(layout, "GeometryNodeScaleInstances")
        override_add_node_type(layout, "GeometryNodeTranslateInstances")
        override_add_node_type(layout, "GeometryNodeSetInstanceTransform")
        layout.separator()
        override_add_node_type(layout, "GeometryNodeInstanceTransform")
        override_add_node_type(layout, "GeometryNodeInputInstanceRotation")
        override_add_node_type(layout, "GeometryNodeInputInstanceScale")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_geometry_node_GEO_MATERIAL(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_MATERIAL"
    bl_label = "Material"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeReplaceMaterial")
        layout.separator()
        override_add_node_type(layout, "GeometryNodeInputMaterialIndex")
        override_add_node_type(layout, "GeometryNodeMaterialSelection")
        layout.separator()
        override_add_node_type(
            layout, "GeometryNodeSetMaterial", search_weight=1.0
        )
        override_add_node_type(layout, "GeometryNodeSetMaterialIndex")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_geometry_node_GEO_MESH(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_MESH"
    bl_label = "Mesh"

    def draw(self, _context):
        layout = self.layout
        layout.menu("NODE_MT_dummy_geometry_node_GEO_MESH_READ")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_MESH_SAMPLE")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_MESH_WRITE")
        layout.separator()
        layout.menu("NODE_MT_dummy_geometry_node_GEO_MESH_OPERATIONS")
        layout.menu("NODE_MT_dummy_category_PRIMITIVES_MESH")
        layout.menu("NODE_MT_dummy_geometry_node_mesh_topology")
        layout.menu("NODE_MT_dummy_category_GEO_UV")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_geometry_node_GEO_MESH_READ(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_MESH_READ"
    bl_label = "Read"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeInputMeshEdgeAngle")
        override_add_node_type(layout, "GeometryNodeInputMeshEdgeNeighbors")
        override_add_node_type(layout, "GeometryNodeInputMeshEdgeVertices")
        override_add_node_type(layout, "GeometryNodeEdgesToFaceGroups")
        override_add_node_type(layout, "GeometryNodeInputMeshFaceArea")
        override_add_node_type(layout, "GeometryNodeMeshFaceSetBoundaries")
        override_add_node_type(layout, "GeometryNodeInputMeshFaceNeighbors")
        if context.space_data.geometry_nodes_type == "TOOL":
            override_add_node_type(layout, "GeometryNodeToolFaceSet")
        override_add_node_type(layout, "GeometryNodeInputMeshFaceIsPlanar")
        override_add_node_type(layout, "GeometryNodeInputShadeSmooth")
        override_add_node_type(layout, "GeometryNodeInputEdgeSmooth")
        override_add_node_type(layout, "GeometryNodeInputMeshIsland")
        override_add_node_type(layout, "GeometryNodeInputShortestEdgePaths")
        override_add_node_type(layout, "GeometryNodeInputMeshVertexNeighbors")
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Read")


class NODE_MT_dummy_geometry_node_GEO_MESH_SAMPLE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_MESH_SAMPLE"
    bl_label = "Sample"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeSampleNearestSurface")
        override_add_node_type(layout, "GeometryNodeSampleUVSurface")
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Sample")


class NODE_MT_dummy_geometry_node_GEO_MESH_WRITE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_MESH_WRITE"
    bl_label = "Write"

    def draw(self, context):
        layout = self.layout
        if context.space_data.geometry_nodes_type == "TOOL":
            override_add_node_type(layout, "GeometryNodeToolSetFaceSet")
        override_add_node_type(layout, "GeometryNodeSetShadeSmooth")
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Write")


class NODE_MT_dummy_geometry_node_GEO_MESH_OPERATIONS(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_MESH_OPERATIONS"
    bl_label = "Operations"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeDualMesh")
        override_add_node_type(layout, "GeometryNodeEdgePathsToCurves")
        override_add_node_type(layout, "GeometryNodeEdgePathsToSelection")
        override_add_node_type(layout, "GeometryNodeExtrudeMesh")
        override_add_node_type(layout, "GeometryNodeFlipFaces")
        override_add_node_type(layout, "GeometryNodeMeshBoolean")
        override_add_node_type(layout, "GeometryNodeMeshToCurve")
        if context.preferences.experimental.use_new_volume_nodes:
            override_add_node_type(layout, "GeometryNodeMeshToDensityGrid")
        override_add_node_type(layout, "GeometryNodeMeshToPoints")
        if context.preferences.experimental.use_new_volume_nodes:
            override_add_node_type(layout, "GeometryNodeMeshToSDFGrid")
        override_add_node_type(layout, "GeometryNodeMeshToVolume")
        override_add_node_type(layout, "GeometryNodeScaleElements")
        override_add_node_type(layout, "GeometryNodeSplitEdges")
        override_add_node_type(layout, "GeometryNodeSubdivideMesh")
        override_add_node_type(layout, "GeometryNodeSubdivisionSurface")
        override_add_node_type(layout, "GeometryNodeTriangulate")
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Operations")


class NODE_MT_dummy_category_PRIMITIVES_MESH(Menu):
    bl_idname = "NODE_MT_dummy_category_PRIMITIVES_MESH"
    bl_label = "Primitives"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeMeshCone")
        override_add_node_type(layout, "GeometryNodeMeshCube")
        override_add_node_type(layout, "GeometryNodeMeshCylinder")
        override_add_node_type(layout, "GeometryNodeMeshGrid")
        override_add_node_type(layout, "GeometryNodeMeshIcoSphere")
        override_add_node_type(layout, "GeometryNodeMeshCircle")
        override_add_node_type(layout, "GeometryNodeMeshLine")
        override_add_node_type(layout, "GeometryNodeMeshUVSphere")
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Primitives")


class NODE_MT_dummy_category_import(Menu):
    bl_idname = "NODE_MT_dummy_category_import"
    bl_label = "Import"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeImportCSV")
        override_add_node_type(layout, "GeometryNodeImportOBJ")
        override_add_node_type(layout, "GeometryNodeImportPLY")
        override_add_node_type(layout, "GeometryNodeImportSTL")
        node_add_menu.draw_assets_for_catalog(layout, "Input/Import")


class NODE_MT_dummy_geometry_node_mesh_topology(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_mesh_topology"
    bl_label = "Topology"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeCornersOfEdge")
        override_add_node_type(layout, "GeometryNodeCornersOfFace")
        override_add_node_type(layout, "GeometryNodeCornersOfVertex")
        override_add_node_type(layout, "GeometryNodeEdgesOfCorner")
        override_add_node_type(layout, "GeometryNodeEdgesOfVertex")
        override_add_node_type(layout, "GeometryNodeFaceOfCorner")
        override_add_node_type(layout, "GeometryNodeOffsetCornerInFace")
        override_add_node_type(layout, "GeometryNodeVertexOfCorner")
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Topology")


class NODE_MT_dummy_category_GEO_OUTPUT(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_OUTPUT"
    bl_label = "Output"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "NodeGroupOutput")
        override_add_node_type(layout, "GeometryNodeViewer")
        override_add_node_type(layout, "GeometryNodeWarning")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_category_GEO_POINT(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_POINT"
    bl_label = "Point"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeDistributePointsInVolume")
        if context.preferences.experimental.use_new_volume_nodes:
            override_add_node_type(layout, "GeometryNodeDistributePointsInGrid")
        override_add_node_type(layout, "GeometryNodeDistributePointsOnFaces")
        layout.separator()
        override_add_node_type(layout, "GeometryNodePoints")
        override_add_node_type(layout, "GeometryNodePointsToCurves")
        if context.preferences.experimental.use_new_volume_nodes:
            override_add_node_type(layout, "GeometryNodePointsToSDFGrid")
        override_add_node_type(layout, "GeometryNodePointsToVertices")
        override_add_node_type(layout, "GeometryNodePointsToVolume")
        layout.separator()
        override_add_node_type(layout, "GeometryNodeSetPointRadius")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_category_simulation(Menu):
    bl_idname = "NODE_MT_dummy_category_simulation"
    bl_label = "Simulation"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_simulation_zone(layout, label="Simulation")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_category_GEO_TEXT(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_TEXT"
    bl_label = "Text"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeStringJoin")
        override_add_node_type(layout, "FunctionNodeReplaceString")
        override_add_node_type(layout, "FunctionNodeSliceString")
        layout.separator()
        override_add_node_type(layout, "FunctionNodeStringLength")
        override_add_node_type(layout, "FunctionNodeFindInString")
        override_add_node_type(layout, "GeometryNodeStringToCurves")
        override_add_node_type(layout, "FunctionNodeValueToString")
        layout.separator()
        override_add_node_type(layout, "FunctionNodeInputSpecialCharacters")
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Text")


class NODE_MT_dummy_category_GEO_TEXTURE(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_TEXTURE"
    bl_label = "Texture"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "ShaderNodeTexBrick")
        override_add_node_type(layout, "ShaderNodeTexChecker")
        override_add_node_type(layout, "ShaderNodeTexGabor")
        override_add_node_type(layout, "ShaderNodeTexGradient")
        override_add_node_type(layout, "GeometryNodeImageTexture")
        override_add_node_type(layout, "ShaderNodeTexMagic")
        override_add_node_type(layout, "ShaderNodeTexNoise")
        override_add_node_type(layout, "ShaderNodeTexVoronoi")
        override_add_node_type(layout, "ShaderNodeTexWave")
        override_add_node_type(layout, "ShaderNodeTexWhiteNoise")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_category_GEO_UTILITIES(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_UTILITIES"
    bl_label = "Utilities"

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_dummy_geometry_node_GEO_COLOR")
        layout.menu("NODE_MT_dummy_category_GEO_TEXT")
        layout.menu("NODE_MT_dummy_category_GEO_VECTOR")
        layout.separator()
        layout.menu("NODE_MT_dummy_category_GEO_UTILITIES_FIELD")
        layout.menu("NODE_MT_dummy_category_GEO_UTILITIES_MATH")
        layout.menu("NODE_MT_dummy_category_utilities_matrix")
        layout.menu("NODE_MT_dummy_category_GEO_UTILITIES_ROTATION")
        layout.menu("NODE_MT_dummy_category_GEO_UTILITIES_DEPRECATED")
        layout.separator()
        node_add_menu.add_foreach_geometry_element_zone(
            layout, label="For Each Element"
        )
        override_add_node_type(layout, "GeometryNodeIndexSwitch")
        override_add_node_type(layout, "GeometryNodeMenuSwitch")
        override_add_node_type(layout, "FunctionNodeRandomValue")
        node_add_menu.add_repeat_zone(layout, label="Repeat")
        override_add_node_type(layout, "GeometryNodeSwitch")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_category_GEO_UTILITIES_DEPRECATED(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_UTILITIES_DEPRECATED"
    bl_label = "Deprecated"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "FunctionNodeAlignEulerToVector")
        override_add_node_type(layout, "FunctionNodeRotateEuler")
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Deprecated")


class NODE_MT_dummy_category_GEO_UTILITIES_FIELD(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_UTILITIES_FIELD"
    bl_label = "Field"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeAccumulateField")
        override_add_node_type(layout, "GeometryNodeFieldAtIndex")
        override_add_node_type(layout, "GeometryNodeFieldOnDomain")
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Field")


class NODE_MT_dummy_category_GEO_UTILITIES_ROTATION(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_UTILITIES_ROTATION"
    bl_label = "Rotation"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "FunctionNodeAlignRotationToVector")
        override_add_node_type(layout, "FunctionNodeAxesToRotation")
        override_add_node_type(layout, "FunctionNodeAxisAngleToRotation")
        override_add_node_type(layout, "FunctionNodeEulerToRotation")
        override_add_node_type(layout, "FunctionNodeInvertRotation")
        override_add_node_type(layout, "FunctionNodeRotateRotation")
        override_add_node_type(layout, "FunctionNodeRotateVector")
        override_add_node_type(layout, "FunctionNodeRotationToAxisAngle")
        override_add_node_type(layout, "FunctionNodeRotationToEuler")
        override_add_node_type(layout, "FunctionNodeRotationToQuaternion")
        override_add_node_type(layout, "FunctionNodeQuaternionToRotation")
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Rotation")


class NODE_MT_dummy_category_utilities_matrix(Menu):
    bl_idname = "NODE_MT_dummy_category_utilities_matrix"
    bl_label = "Matrix"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "FunctionNodeCombineMatrix")
        override_add_node_type(layout, "FunctionNodeCombineTransform")
        override_add_node_type(
            layout, "FunctionNodeMatrixDeterminant", label="Determinant"
        )
        override_add_node_type(layout, "FunctionNodeInvertMatrix")
        override_add_node_type(layout, "FunctionNodeMatrixMultiply")
        override_add_node_type(layout, "FunctionNodeProjectPoint")
        override_add_node_type(layout, "FunctionNodeSeparateMatrix")
        override_add_node_type(layout, "FunctionNodeSeparateTransform")
        override_add_node_type(layout, "FunctionNodeTransformDirection")
        override_add_node_type(layout, "FunctionNodeTransformPoint")
        override_add_node_type(layout, "FunctionNodeTransposeMatrix")
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Matrix")


class NODE_MT_dummy_category_GEO_UTILITIES_MATH(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_UTILITIES_MATH"
    bl_label = "Math"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "FunctionNodeBooleanMath")
        override_add_node_type(layout, "FunctionNodeIntegerMath")
        override_add_node_type(layout, "ShaderNodeClamp")
        override_add_node_type(layout, "FunctionNodeCompare")
        override_add_node_type(layout, "ShaderNodeFloatCurve")
        override_add_node_type(layout, "FunctionNodeFloatToInt")
        override_add_node_type(layout, "FunctionNodeHashValue")
        override_add_node_type(layout, "ShaderNodeMapRange")
        override_add_node_type(layout, "ShaderNodeMath")
        override_add_node_type(layout, "ShaderNodeMix")
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Math")


class NODE_MT_dummy_category_GEO_UV(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_UV"
    bl_label = "UV"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeUVPackIslands")
        override_add_node_type(layout, "GeometryNodeUVUnwrap")
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/UV")


class NODE_MT_dummy_category_GEO_VECTOR(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_VECTOR"
    bl_label = "Vector"

    def draw(self, _context):
        layout = self.layout
        override_add_node_type(layout, "ShaderNodeVectorCurve")
        override_add_node_type(layout, "ShaderNodeVectorMath")
        override_add_node_type(layout, "ShaderNodeVectorRotate")
        layout.separator()
        override_add_node_type(layout, "ShaderNodeCombineXYZ")
        props = override_add_node_type(
            layout, "ShaderNodeMix", label=iface_("Mix Vector")
        )
        ops = props.settings.add()
        ops.name = "data_type"
        ops.value = "'VECTOR'"
        override_add_node_type(layout, "ShaderNodeSeparateXYZ")
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Vector")


class NODE_MT_dummy_category_GEO_VOLUME(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_VOLUME"
    bl_label = "Volume"
    bl_translation_context = i18n_contexts.id_id

    def draw(self, context):
        layout = self.layout
        if context.preferences.experimental.use_new_volume_nodes:
            layout.menu("NODE_MT_dummy_geometry_node_GEO_VOLUME_READ")
            layout.menu("NODE_MT_dummy_geometry_node_volume_sample")
            layout.menu("NODE_MT_dummy_geometry_node_GEO_VOLUME_WRITE")
            layout.separator()
        layout.menu("NODE_MT_dummy_geometry_node_GEO_VOLUME_OPERATIONS")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_VOLUME_PRIMITIVES")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_geometry_node_GEO_VOLUME_READ(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_VOLUME_READ"
    bl_label = "Read"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeGetNamedGrid")
        node_add_menu.draw_assets_for_catalog(layout, "Volume/Read")


class NODE_MT_dummy_geometry_node_GEO_VOLUME_WRITE(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_VOLUME_WRITE"
    bl_label = "Write"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeStoreNamedGrid")
        node_add_menu.draw_assets_for_catalog(layout, "Volume/Write")


class NODE_MT_dummy_geometry_node_volume_sample(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_volume_sample"
    bl_label = "Sample"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeSampleGrid")
        override_add_node_type(layout, "GeometryNodeSampleGridIndex")
        node_add_menu.draw_assets_for_catalog(layout, "Volume/Sample")


class NODE_MT_dummy_geometry_node_GEO_VOLUME_OPERATIONS(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_VOLUME_OPERATIONS"
    bl_label = "Operations"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeVolumeToMesh")
        if context.preferences.experimental.use_new_volume_nodes:
            override_add_node_type(layout, "GeometryNodeGridToMesh")
            override_add_node_type(layout, "GeometryNodeSDFGridBoolean")
        node_add_menu.draw_assets_for_catalog(layout, "Volume/Operations")


class NODE_MT_dummy_geometry_node_GEO_VOLUME_PRIMITIVES(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_GEO_VOLUME_PRIMITIVES"
    bl_label = "Primitives"

    def draw(self, context):
        layout = self.layout
        override_add_node_type(layout, "GeometryNodeVolumeCube")
        node_add_menu.draw_assets_for_catalog(layout, "Volume/Primitives")


class NODE_MT_dummy_category_GEO_GROUP(Menu):
    bl_idname = "NODE_MT_dummy_category_GEO_GROUP"
    bl_label = "Group"

    def draw(self, context):
        layout = self.layout
        node_add_menu.draw_node_group_add_menu(context, layout)
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_dummy_geometry_node_add_all(Menu):
    bl_idname = "NODE_MT_dummy_geometry_node_add_all"
    bl_label = ""

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_dummy_geometry_node_GEO_ATTRIBUTE")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_INPUT")
        layout.menu("NODE_MT_dummy_category_GEO_OUTPUT")
        layout.separator()
        layout.menu("NODE_MT_dummy_geometry_node_GEO_GEOMETRY")
        layout.separator()
        layout.menu("NODE_MT_dummy_geometry_node_GEO_CURVE")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_INSTANCE")
        layout.menu("NODE_MT_dummy_geometry_node_GEO_MESH")
        layout.menu("NODE_MT_dummy_category_GEO_POINT")
        layout.menu("NODE_MT_dummy_category_GEO_VOLUME")
        layout.separator()
        layout.menu("NODE_MT_dummy_category_simulation")
        layout.separator()
        layout.menu("NODE_MT_dummy_geometry_node_GEO_MATERIAL")
        layout.menu("NODE_MT_dummy_category_GEO_TEXTURE")
        layout.menu("NODE_MT_dummy_category_GEO_UTILITIES")
        layout.separator()
        layout.menu("NODE_MT_dummy_category_GEO_GROUP")
        layout.menu("NODE_MT_dummy_category_layout")
        node_add_menu.draw_root_assets(layout)


classes = (
    NODE_MT_dummy_geometry_node_add_all,
    NODE_MT_dummy_geometry_node_GEO_ATTRIBUTE,
    NODE_MT_dummy_geometry_node_GEO_INPUT,
    NODE_MT_dummy_geometry_node_GEO_INPUT_CONSTANT,
    NODE_MT_dummy_geometry_node_GEO_INPUT_GROUP,
    NODE_MT_dummy_geometry_node_GEO_INPUT_SCENE,
    NODE_MT_dummy_category_GEO_OUTPUT,
    NODE_MT_dummy_geometry_node_GEO_CURVE,
    NODE_MT_dummy_geometry_node_GEO_CURVE_READ,
    NODE_MT_dummy_geometry_node_GEO_CURVE_SAMPLE,
    NODE_MT_dummy_geometry_node_GEO_CURVE_WRITE,
    NODE_MT_dummy_geometry_node_GEO_CURVE_OPERATIONS,
    NODE_MT_dummy_geometry_node_GEO_PRIMITIVES_CURVE,
    NODE_MT_dummy_geometry_node_curve_topology,
    NODE_MT_dummy_geometry_node_GEO_GEOMETRY,
    NODE_MT_dummy_geometry_node_GEO_GEOMETRY_READ,
    NODE_MT_dummy_geometry_node_GEO_GEOMETRY_WRITE,
    NODE_MT_dummy_geometry_node_GEO_GEOMETRY_OPERATIONS,
    NODE_MT_dummy_geometry_node_GEO_GEOMETRY_SAMPLE,
    NODE_MT_dummy_geometry_node_GEO_INSTANCE,
    NODE_MT_dummy_geometry_node_GEO_MESH,
    NODE_MT_dummy_geometry_node_GEO_MESH_READ,
    NODE_MT_dummy_geometry_node_GEO_MESH_SAMPLE,
    NODE_MT_dummy_geometry_node_GEO_MESH_WRITE,
    NODE_MT_dummy_geometry_node_GEO_MESH_OPERATIONS,
    NODE_MT_dummy_category_GEO_UV,
    NODE_MT_dummy_category_PRIMITIVES_MESH,
    NODE_MT_dummy_category_import,
    NODE_MT_dummy_geometry_node_mesh_topology,
    NODE_MT_dummy_category_GEO_POINT,
    NODE_MT_dummy_category_simulation,
    NODE_MT_dummy_category_GEO_VOLUME,
    NODE_MT_dummy_geometry_node_GEO_VOLUME_READ,
    NODE_MT_dummy_geometry_node_volume_sample,
    NODE_MT_dummy_geometry_node_GEO_VOLUME_WRITE,
    NODE_MT_dummy_geometry_node_GEO_VOLUME_OPERATIONS,
    NODE_MT_dummy_geometry_node_GEO_VOLUME_PRIMITIVES,
    NODE_MT_dummy_geometry_node_GEO_MATERIAL,
    NODE_MT_dummy_category_GEO_TEXTURE,
    NODE_MT_dummy_category_GEO_UTILITIES,
    NODE_MT_dummy_geometry_node_GEO_COLOR,
    NODE_MT_dummy_category_GEO_TEXT,
    NODE_MT_dummy_category_GEO_VECTOR,
    NODE_MT_dummy_category_GEO_UTILITIES_FIELD,
    NODE_MT_dummy_category_GEO_UTILITIES_MATH,
    NODE_MT_dummy_category_GEO_UTILITIES_ROTATION,
    NODE_MT_dummy_geometry_node_GEO_INPUT_GIZMO,
    NODE_MT_dummy_category_utilities_matrix,
    NODE_MT_dummy_category_GEO_UTILITIES_DEPRECATED,
    NODE_MT_dummy_category_GEO_GROUP,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class

    for cls in classes:
        register_class(cls)
