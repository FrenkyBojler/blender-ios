# SPDX-FileCopyrightText: 2022-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import Menu
from bl_ui import node_add_menu
from bpy.app.translations import (
    pgettext_iface as iface_,
    contexts as i18n_contexts,
)


class NODE_MT_geometry_node_GEO_ATTRIBUTE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_ATTRIBUTE"
    bl_label = "Attribute"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeAttributeStatistic", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeAttributeDomainSize", context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeBlurAttribute", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCaptureAttribute", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeRemoveAttribute", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeStoreNamedAttribute", search_weight=1.0, context=context)
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_geometry_node_GEO_COLOR(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_COLOR"
    bl_label = "Color"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "ShaderNodeBlackbody", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeValToRGB", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeRGBCurve", context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "FunctionNodeCombineColor", context=context)
        props = node_add_menu.add_node_type(layout, "ShaderNodeMix", label=iface_("Mix Color"), context=context)
        ops = props.settings.add()
        ops.name = "data_type"
        ops.value = "'RGBA'"
        node_add_menu.add_node_type(layout, "FunctionNodeSeparateColor", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Color")


class NODE_MT_geometry_node_GEO_CURVE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_CURVE"
    bl_label = "Curve"

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_geometry_node_GEO_CURVE_READ")
        layout.menu("NODE_MT_geometry_node_GEO_CURVE_SAMPLE")
        layout.menu("NODE_MT_geometry_node_GEO_CURVE_WRITE")
        layout.separator()
        layout.menu("NODE_MT_geometry_node_GEO_CURVE_OPERATIONS")
        layout.menu("NODE_MT_geometry_node_GEO_PRIMITIVES_CURVE")
        layout.menu("NODE_MT_geometry_node_curve_topology")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_geometry_node_GEO_CURVE_READ(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_CURVE_READ"
    bl_label = "Read"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeInputCurveHandlePositions", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurveLength", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputTangent", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputCurveTilt", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurveEndpointSelection", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurveHandleTypeSelection", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputSplineCyclic", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSplineLength", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSplineParameter", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputSplineResolution", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Read")


class NODE_MT_geometry_node_GEO_CURVE_SAMPLE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_CURVE_SAMPLE"
    bl_label = "Sample"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeSampleCurve", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Sample")


class NODE_MT_geometry_node_GEO_CURVE_WRITE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_CURVE_WRITE"
    bl_label = "Write"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeSetCurveNormal", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSetCurveRadius", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSetCurveTilt", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSetCurveHandlePositions", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurveSetHandles", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSetSplineCyclic", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSetSplineResolution", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurveSplineType", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Write")


class NODE_MT_geometry_node_GEO_CURVE_OPERATIONS(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_CURVE_OPERATIONS"
    bl_label = "Operations"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeCurvesToGreasePencil", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurveToMesh", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurveToPoints", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeDeformCurvesOnSurface", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeFillCurve", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeFilletCurve", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeGreasePencilToCurves", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInterpolateCurves", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMergeLayers", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeResampleCurve", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeReverseCurve", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSubdivideCurve", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeTrimCurve", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Operations")


class NODE_MT_geometry_node_GEO_PRIMITIVES_CURVE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_PRIMITIVES_CURVE"
    bl_label = "Primitives"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeCurveArc", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurvePrimitiveBezierSegment", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurvePrimitiveCircle", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurvePrimitiveLine", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurveSpiral", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurveQuadraticBezier", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurvePrimitiveQuadrilateral", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCurveStar", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Primitives")


class NODE_MT_geometry_node_curve_topology(Menu):
    bl_idname = "NODE_MT_geometry_node_curve_topology"
    bl_label = "Topology"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeCurveOfPoint", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeOffsetPointInCurve", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodePointsOfCurve", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Curve/Topology")


class NODE_MT_geometry_node_GEO_GEOMETRY(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_GEOMETRY"
    bl_label = "Geometry"

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_geometry_node_GEO_GEOMETRY_READ")
        layout.menu("NODE_MT_geometry_node_GEO_GEOMETRY_SAMPLE")
        layout.menu("NODE_MT_geometry_node_GEO_GEOMETRY_WRITE")
        layout.separator()
        layout.menu("NODE_MT_geometry_node_GEO_GEOMETRY_OPERATIONS")
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeGeometryToInstance", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeJoinGeometry", search_weight=1.0, context=context)
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_geometry_node_GEO_GEOMETRY_READ(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_GEOMETRY_READ"
    bl_label = "Read"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeInputID", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputIndex", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputNamedAttribute", search_weight=1.0, context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputNormal", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputPosition", search_weight=1.0, context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputRadius", context=context)
        if context.space_data.geometry_nodes_type == 'TOOL':
            node_add_menu.add_node_type(layout, "GeometryNodeToolSelection", context=context)
            node_add_menu.add_node_type(layout, "GeometryNodeToolActiveElement", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Geometry/Read")


class NODE_MT_geometry_node_GEO_GEOMETRY_WRITE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_GEOMETRY_WRITE"
    bl_label = "Write"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeSetGeometryName", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSetID", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSetPosition", search_weight=1.0, context=context)
        if context.space_data.geometry_nodes_type == 'TOOL':
            node_add_menu.add_node_type(layout, "GeometryNodeToolSetSelection", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Geometry/Write")


class NODE_MT_geometry_node_GEO_GEOMETRY_OPERATIONS(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_GEOMETRY_OPERATIONS"
    bl_label = "Operations"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeBake", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeBoundBox", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeConvexHull", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeDeleteGeometry", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeDuplicateElements", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMergeByDistance", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSortElements", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeTransform", search_weight=1.0, context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeSeparateComponents", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSeparateGeometry", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSplitToInstances", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Geometry/Operations")


class NODE_MT_geometry_node_GEO_GEOMETRY_SAMPLE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_GEOMETRY_SAMPLE"
    bl_label = "Sample"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeProximity", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeIndexOfNearest", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeRaycast", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSampleIndex", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSampleNearest", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Geometry/Sample")


class NODE_MT_geometry_node_GEO_INPUT(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_INPUT"
    bl_label = "Input"

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_geometry_node_GEO_INPUT_CONSTANT")
        if context.space_data.geometry_nodes_type != 'TOOL':
            layout.menu("NODE_MT_geometry_node_GEO_INPUT_GIZMO")
        layout.menu("NODE_MT_geometry_node_GEO_INPUT_GROUP")
        layout.menu("NODE_MT_geometry_node_GEO_INPUT_SCENE")
        if context.preferences.experimental.use_new_file_import_nodes:
            layout.menu("NODE_MT_category_import")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_geometry_node_GEO_INPUT_CONSTANT(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_INPUT_CONSTANT"
    bl_label = "Constant"
    bl_translation_context = i18n_contexts.id_nodetree

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "FunctionNodeInputBool", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputCollection", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeInputColor", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputImage", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeInputInt", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputMaterial", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputObject", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeInputRotation", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeInputString", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeValue", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeInputVector", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Input/Constant")


class NODE_MT_geometry_node_GEO_INPUT_GROUP(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_INPUT_GROUP"
    bl_label = "Group"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "NodeGroupInput", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Input/Group")


class NODE_MT_geometry_node_GEO_INPUT_SCENE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_INPUT_SCENE"
    bl_label = "Scene"

    def draw(self, context):
        layout = self.layout
        if context.space_data.geometry_nodes_type == 'TOOL':
            node_add_menu.add_node_type(layout, "GeometryNodeTool3DCursor", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputActiveCamera", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCollectionInfo", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeImageInfo", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeIsViewport", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputNamedLayerSelection", context=context)
        if context.space_data.geometry_nodes_type == 'TOOL':
            node_add_menu.add_node_type(layout, "GeometryNodeToolMousePosition", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeObjectInfo", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputSceneTime", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSelfObject", context=context)
        if context.space_data.geometry_nodes_type == 'TOOL':
            node_add_menu.add_node_type(layout, "GeometryNodeViewportTransform", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Input/Scene")


class NODE_MT_geometry_node_GEO_INPUT_GIZMO(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_INPUT_GIZMO"
    bl_label = "Gizmo"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeGizmoDial", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeGizmoLinear", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeGizmoTransform", context=context)
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_geometry_node_GEO_INSTANCE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_INSTANCE"
    bl_label = "Instances"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeInstanceOnPoints", search_weight=2.0, context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInstancesToPoints", context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeRealizeInstances", search_weight=1.0, context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeRotateInstances", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeScaleInstances", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeTranslateInstances", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSetInstanceTransform", context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeInstanceTransform", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputInstanceRotation", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputInstanceScale", context=context)
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_geometry_node_GEO_MATERIAL(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_MATERIAL"
    bl_label = "Material"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeReplaceMaterial", context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeInputMaterialIndex", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMaterialSelection", context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeSetMaterial", search_weight=1.0, context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSetMaterialIndex", context=context)
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_geometry_node_GEO_MESH(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_MESH"
    bl_label = "Mesh"

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_geometry_node_GEO_MESH_READ")
        layout.menu("NODE_MT_geometry_node_GEO_MESH_SAMPLE")
        layout.menu("NODE_MT_geometry_node_GEO_MESH_WRITE")
        layout.separator()
        layout.menu("NODE_MT_geometry_node_GEO_MESH_OPERATIONS")
        layout.menu("NODE_MT_category_PRIMITIVES_MESH")
        layout.menu("NODE_MT_geometry_node_mesh_topology")
        layout.menu("NODE_MT_category_GEO_UV")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_geometry_node_GEO_MESH_READ(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_MESH_READ"
    bl_label = "Read"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshEdgeAngle", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshEdgeNeighbors", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshEdgeVertices", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeEdgesToFaceGroups", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshFaceArea", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshFaceSetBoundaries", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshFaceNeighbors", context=context)
        if context.space_data.geometry_nodes_type == 'TOOL':
            node_add_menu.add_node_type(layout, "GeometryNodeToolFaceSet", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshFaceIsPlanar", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputShadeSmooth", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputEdgeSmooth", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshIsland", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputShortestEdgePaths", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshVertexNeighbors", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Read")


class NODE_MT_geometry_node_GEO_MESH_SAMPLE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_MESH_SAMPLE"
    bl_label = "Sample"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeSampleNearestSurface", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSampleUVSurface", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Sample")


class NODE_MT_geometry_node_GEO_MESH_WRITE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_MESH_WRITE"
    bl_label = "Write"

    def draw(self, context):
        layout = self.layout
        if context.space_data.geometry_nodes_type == 'TOOL':
            node_add_menu.add_node_type(layout, "GeometryNodeToolSetFaceSet", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSetShadeSmooth", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Write")


class NODE_MT_geometry_node_GEO_MESH_OPERATIONS(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_MESH_OPERATIONS"
    bl_label = "Operations"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeDualMesh", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeEdgePathsToCurves", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeEdgePathsToSelection", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeExtrudeMesh", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeFlipFaces", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshBoolean", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshToCurve", context=context)
        if context.preferences.experimental.use_new_volume_nodes:
            node_add_menu.add_node_type(layout, "GeometryNodeMeshToDensityGrid", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshToPoints", context=context)
        if context.preferences.experimental.use_new_volume_nodes:
            node_add_menu.add_node_type(layout, "GeometryNodeMeshToSDFGrid", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshToVolume", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeScaleElements", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSplitEdges", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSubdivideMesh", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSubdivisionSurface", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeTriangulate", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Operations")


class NODE_MT_category_PRIMITIVES_MESH(Menu):
    bl_idname = "NODE_MT_category_PRIMITIVES_MESH"
    bl_label = "Primitives"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeMeshCone", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshCube", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshCylinder", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshGrid", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshIcoSphere", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshCircle", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshLine", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMeshUVSphere", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Primitives")


class NODE_MT_category_import(Menu):
    bl_idname = "NODE_MT_category_import"
    bl_label = "Import"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeImportCSV", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeImportOBJ", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeImportPLY", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeImportSTL", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Input/Import")


class NODE_MT_geometry_node_mesh_topology(Menu):
    bl_idname = "NODE_MT_geometry_node_mesh_topology"
    bl_label = "Topology"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeCornersOfEdge", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCornersOfFace", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeCornersOfVertex", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeEdgesOfCorner", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeEdgesOfVertex", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeFaceOfCorner", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeOffsetCornerInFace", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeVertexOfCorner", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/Topology")


class NODE_MT_category_GEO_OUTPUT(Menu):
    bl_idname = "NODE_MT_category_GEO_OUTPUT"
    bl_label = "Output"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "NodeGroupOutput", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeViewer", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeWarning", context=context)
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_category_GEO_POINT(Menu):
    bl_idname = "NODE_MT_category_GEO_POINT"
    bl_label = "Point"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeDistributePointsInVolume", context=context)
        if context.preferences.experimental.use_new_volume_nodes:
            node_add_menu.add_node_type(layout, "GeometryNodeDistributePointsInGrid", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeDistributePointsOnFaces", context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodePoints", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodePointsToCurves", context=context)
        if context.preferences.experimental.use_new_volume_nodes:
            node_add_menu.add_node_type(layout, "GeometryNodePointsToSDFGrid", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodePointsToVertices", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodePointsToVolume", context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeSetPointRadius", context=context)
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_category_simulation(Menu):
    bl_idname = "NODE_MT_category_simulation"
    bl_label = "Simulation"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_simulation_zone(layout, label="Simulation")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_category_GEO_TEXT(Menu):
    bl_idname = "NODE_MT_category_GEO_TEXT"
    bl_label = "Text"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeStringJoin", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeReplaceString", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeSliceString", context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "FunctionNodeStringLength", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeFindInString", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeStringToCurves", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeValueToString", context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "FunctionNodeInputSpecialCharacters", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Text")


class NODE_MT_category_GEO_TEXTURE(Menu):
    bl_idname = "NODE_MT_category_GEO_TEXTURE"
    bl_label = "Texture"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "ShaderNodeTexBrick", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeTexChecker", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeTexGabor", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeTexGradient", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeImageTexture", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeTexMagic", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeTexNoise", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeTexVoronoi", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeTexWave", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeTexWhiteNoise", context=context)
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_category_GEO_UTILITIES(Menu):
    bl_idname = "NODE_MT_category_GEO_UTILITIES"
    bl_label = "Utilities"

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_geometry_node_GEO_COLOR")
        layout.menu("NODE_MT_category_GEO_TEXT")
        layout.menu("NODE_MT_category_GEO_VECTOR")
        layout.separator()
        layout.menu("NODE_MT_category_GEO_UTILITIES_FIELD")
        layout.menu("NODE_MT_category_GEO_UTILITIES_MATH")
        layout.menu("NODE_MT_category_utilities_matrix")
        layout.menu("NODE_MT_category_GEO_UTILITIES_ROTATION")
        layout.menu("NODE_MT_category_GEO_UTILITIES_DEPRECATED")
        layout.separator()
        node_add_menu.add_foreach_geometry_element_zone(layout, label="For Each Element")
        node_add_menu.add_node_type(layout, "GeometryNodeIndexSwitch", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeMenuSwitch", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeRandomValue", context=context)
        node_add_menu.add_repeat_zone(layout, label="Repeat")
        node_add_menu.add_node_type(layout, "GeometryNodeSwitch", context=context)
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_category_GEO_UTILITIES_DEPRECATED(Menu):
    bl_idname = "NODE_MT_category_GEO_UTILITIES_DEPRECATED"
    bl_label = "Deprecated"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "FunctionNodeAlignEulerToVector", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeRotateEuler", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Deprecated")


class NODE_MT_category_GEO_UTILITIES_FIELD(Menu):
    bl_idname = "NODE_MT_category_GEO_UTILITIES_FIELD"
    bl_label = "Field"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeAccumulateField", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeFieldAtIndex", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeFieldOnDomain", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Field")


class NODE_MT_category_GEO_UTILITIES_ROTATION(Menu):
    bl_idname = "NODE_MT_category_GEO_UTILITIES_ROTATION"
    bl_label = "Rotation"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "FunctionNodeAlignRotationToVector", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeAxesToRotation", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeAxisAngleToRotation", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeEulerToRotation", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeInvertRotation", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeRotateRotation", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeRotateVector", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeRotationToAxisAngle", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeRotationToEuler", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeRotationToQuaternion", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeQuaternionToRotation", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Rotation")


class NODE_MT_category_utilities_matrix(Menu):
    bl_idname = "NODE_MT_category_utilities_matrix"
    bl_label = "Matrix"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "FunctionNodeCombineMatrix", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeCombineTransform", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeMatrixDeterminant", label="Determinant", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeInvertMatrix", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeMatrixMultiply", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeProjectPoint", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeSeparateMatrix", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeSeparateTransform", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeTransformDirection", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeTransformPoint", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeTransposeMatrix", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Matrix")


class NODE_MT_category_GEO_UTILITIES_MATH(Menu):
    bl_idname = "NODE_MT_category_GEO_UTILITIES_MATH"
    bl_label = "Math"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "FunctionNodeBooleanMath", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeIntegerMath", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeClamp", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeCompare", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeFloatCurve", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeFloatToInt", context=context)
        node_add_menu.add_node_type(layout, "FunctionNodeHashValue", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeMapRange", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeMath", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeMix", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Math")


class NODE_MT_category_GEO_UV(Menu):
    bl_idname = "NODE_MT_category_GEO_UV"
    bl_label = "UV"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeUVPackIslands", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeUVUnwrap", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Mesh/UV")


class NODE_MT_category_GEO_VECTOR(Menu):
    bl_idname = "NODE_MT_category_GEO_VECTOR"
    bl_label = "Vector"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "ShaderNodeVectorCurve", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeVectorMath", context=context)
        node_add_menu.add_node_type(layout, "ShaderNodeVectorRotate", context=context)
        layout.separator()
        node_add_menu.add_node_type(layout, "ShaderNodeCombineXYZ", context=context)
        props = node_add_menu.add_node_type(layout, "ShaderNodeMix", label=iface_("Mix Vector"), context=context)
        ops = props.settings.add()
        ops.name = "data_type"
        ops.value = "'VECTOR'"
        node_add_menu.add_node_type(layout, "ShaderNodeSeparateXYZ", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Utilities/Vector")


class NODE_MT_category_GEO_VOLUME(Menu):
    bl_idname = "NODE_MT_category_GEO_VOLUME"
    bl_label = "Volume"
    bl_translation_context = i18n_contexts.id_id

    def draw(self, context):
        layout = self.layout
        if context.preferences.experimental.use_new_volume_nodes:
            layout.menu("NODE_MT_geometry_node_GEO_VOLUME_READ")
            layout.menu("NODE_MT_geometry_node_volume_sample")
            layout.menu("NODE_MT_geometry_node_GEO_VOLUME_WRITE")
            layout.separator()
        layout.menu("NODE_MT_geometry_node_GEO_VOLUME_OPERATIONS")
        layout.menu("NODE_MT_geometry_node_GEO_VOLUME_PRIMITIVES")
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_geometry_node_GEO_VOLUME_READ(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_VOLUME_READ"
    bl_label = "Read"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeGetNamedGrid", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Volume/Read")


class NODE_MT_geometry_node_GEO_VOLUME_WRITE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_VOLUME_WRITE"
    bl_label = "Write"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeStoreNamedGrid", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Volume/Write")


class NODE_MT_geometry_node_volume_sample(Menu):
    bl_idname = "NODE_MT_geometry_node_volume_sample"
    bl_label = "Sample"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeSampleGrid", context=context)
        node_add_menu.add_node_type(layout, "GeometryNodeSampleGridIndex", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Volume/Sample")


class NODE_MT_geometry_node_GEO_VOLUME_OPERATIONS(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_VOLUME_OPERATIONS"
    bl_label = "Operations"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeVolumeToMesh", context=context)
        if context.preferences.experimental.use_new_volume_nodes:
            node_add_menu.add_node_type(layout, "GeometryNodeGridToMesh", context=context)
            node_add_menu.add_node_type(layout, "GeometryNodeSDFGridBoolean", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Volume/Operations")


class NODE_MT_geometry_node_GEO_VOLUME_PRIMITIVES(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_VOLUME_PRIMITIVES"
    bl_label = "Primitives"

    def draw(self, context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeVolumeCube", context=context)
        node_add_menu.draw_assets_for_catalog(layout, "Volume/Primitives")


class NODE_MT_category_GEO_GROUP(Menu):
    bl_idname = "NODE_MT_category_GEO_GROUP"
    bl_label = "Group"

    def draw(self, context):
        layout = self.layout
        node_add_menu.draw_node_group_add_menu(context, layout)
        node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_geometry_node_add_all(Menu):
    bl_idname = "NODE_MT_geometry_node_add_all"
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
        layout.menu("NODE_MT_category_GEO_GROUP")
        layout.menu("NODE_MT_category_layout")
        node_add_menu.draw_root_assets(layout)


classes = (
    NODE_MT_geometry_node_add_all,
    NODE_MT_geometry_node_GEO_ATTRIBUTE,
    NODE_MT_geometry_node_GEO_INPUT,
    NODE_MT_geometry_node_GEO_INPUT_CONSTANT,
    NODE_MT_geometry_node_GEO_INPUT_GROUP,
    NODE_MT_geometry_node_GEO_INPUT_SCENE,
    NODE_MT_category_GEO_OUTPUT,
    NODE_MT_geometry_node_GEO_CURVE,
    NODE_MT_geometry_node_GEO_CURVE_READ,
    NODE_MT_geometry_node_GEO_CURVE_SAMPLE,
    NODE_MT_geometry_node_GEO_CURVE_WRITE,
    NODE_MT_geometry_node_GEO_CURVE_OPERATIONS,
    NODE_MT_geometry_node_GEO_PRIMITIVES_CURVE,
    NODE_MT_geometry_node_curve_topology,
    NODE_MT_geometry_node_GEO_GEOMETRY,
    NODE_MT_geometry_node_GEO_GEOMETRY_READ,
    NODE_MT_geometry_node_GEO_GEOMETRY_WRITE,
    NODE_MT_geometry_node_GEO_GEOMETRY_OPERATIONS,
    NODE_MT_geometry_node_GEO_GEOMETRY_SAMPLE,
    NODE_MT_geometry_node_GEO_INSTANCE,
    NODE_MT_geometry_node_GEO_MESH,
    NODE_MT_geometry_node_GEO_MESH_READ,
    NODE_MT_geometry_node_GEO_MESH_SAMPLE,
    NODE_MT_geometry_node_GEO_MESH_WRITE,
    NODE_MT_geometry_node_GEO_MESH_OPERATIONS,
    NODE_MT_category_GEO_UV,
    NODE_MT_category_PRIMITIVES_MESH,
    NODE_MT_category_import,
    NODE_MT_geometry_node_mesh_topology,
    NODE_MT_category_GEO_POINT,
    NODE_MT_category_simulation,
    NODE_MT_category_GEO_VOLUME,
    NODE_MT_geometry_node_GEO_VOLUME_READ,
    NODE_MT_geometry_node_volume_sample,
    NODE_MT_geometry_node_GEO_VOLUME_WRITE,
    NODE_MT_geometry_node_GEO_VOLUME_OPERATIONS,
    NODE_MT_geometry_node_GEO_VOLUME_PRIMITIVES,
    NODE_MT_geometry_node_GEO_MATERIAL,
    NODE_MT_category_GEO_TEXTURE,
    NODE_MT_category_GEO_UTILITIES,
    NODE_MT_geometry_node_GEO_COLOR,
    NODE_MT_category_GEO_TEXT,
    NODE_MT_category_GEO_VECTOR,
    NODE_MT_category_GEO_UTILITIES_FIELD,
    NODE_MT_category_GEO_UTILITIES_MATH,
    NODE_MT_category_GEO_UTILITIES_ROTATION,
    NODE_MT_geometry_node_GEO_INPUT_GIZMO,
    NODE_MT_category_utilities_matrix,
    NODE_MT_category_GEO_UTILITIES_DEPRECATED,
    NODE_MT_category_GEO_GROUP,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
