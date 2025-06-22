/* SPDX-FileCopyrightText: 2018-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_clog
 */

#pragma once

struct CLG_LogRef;

/* Add-ons */
extern CLG_LogRef *LOG_ADDON;

/* Animation & Rigging */
extern CLG_LogRef *LOG_ANIM_ACTION;
extern CLG_LogRef *LOG_ANIM_DATA;
extern CLG_LogRef *LOG_ANIM_DRIVER;
extern CLG_LogRef *LOG_ANIM_EVALUATION;
extern CLG_LogRef *LOG_ANIM_FCURVE;
extern CLG_LogRef *LOG_ANIM_FMODIFIER;
extern CLG_LogRef *LOG_ANIM_IPO;
extern CLG_LogRef *LOG_ANIM_KEYINGSET;
extern CLG_LogRef *LOG_ANIM_MOTION_PATH;
extern CLG_LogRef *LOG_ANIM_NLA;

/* Asset System */
extern CLG_LogRef *LOG_ASSET_CATALOG;
extern CLG_LogRef *LOG_ASSET_INDEX;
extern CLG_LogRef *LOG_ASSET_LIBRARY;

/* Blend file read and write */
extern CLG_LogRef *LOG_BLEND;
extern CLG_LogRef *LOG_BLEND_DOVERSION;
extern CLG_LogRef *LOG_BLEND_LINK;
extern CLG_LogRef *LOG_BLEND_PARTIAL_WRITE;
extern CLG_LogRef *LOG_BLEND_READFILE;
extern CLG_LogRef *LOG_BLEND_VALIDATE;
extern CLG_LogRef *LOG_BLEND_WRITEFILE;

/* Context */
extern CLG_LogRef *LOG_CONTEXT;

/* Depenency graph */
extern CLG_LogRef *LOG_DEPSGRAPH;

/* Geometry */
extern CLG_LogRef *LOG_GEOM_ARMATURE_DEFORM;
extern CLG_LogRef *LOG_GEOM_ATTRIBUTE;
extern CLG_LogRef *LOG_GEOM_BMESH_CONVERT;
extern CLG_LogRef *LOG_GEOM_CURVE;
extern CLG_LogRef *LOG_GEOM_CUSTOMDATA;
extern CLG_LogRef *LOG_GEOM_GPENCIL;
extern CLG_LogRef *LOG_GEOM_MESH;
extern CLG_LogRef *LOG_GEOM_MESH_CONVERT;
extern CLG_LogRef *LOG_GEOM_VFONT;
extern CLG_LogRef *LOG_GEOM_VOLUME;

/* GPU  */
extern CLG_LogRef *LOG_GPU_DEBUG;
extern CLG_LogRef *LOG_GPU_DEBUG_METAL;
extern CLG_LogRef *LOG_GPU_PLATFORM;
extern CLG_LogRef *LOG_GPU_SHADER;
extern CLG_LogRef *LOG_GPU_VULKAN;

/* Images */
extern CLG_LogRef *LOG_IMAGE;
extern CLG_LogRef *LOG_IMAGE_COLOR_MANAGEMENT;

/* Import & Export */
extern CLG_LogRef *LOG_IO_ALEMBIC;
extern CLG_LogRef *LOG_IO_COMMON;
extern CLG_LogRef *LOG_IO_DROP_IMPORT_FILE;
extern CLG_LogRef *LOG_IO_FBX;
extern CLG_LogRef *LOG_IO_MATERIALX;
extern CLG_LogRef *LOG_IO_OBJ;
extern CLG_LogRef *LOG_IO_PLY;
extern CLG_LogRef *LOG_IO_STL;
extern CLG_LogRef *LOG_IO_USD;

/* Datablocks and library linking */
extern CLG_LogRef *LOG_LIB_BPATH;
extern CLG_LogRef *LOG_LIB_ICONS;
extern CLG_LogRef *LOG_LIB_ID_MANAGEMENT;
extern CLG_LogRef *LOG_LIB_ID;
extern CLG_LogRef *LOG_LIB_IDPROP;
extern CLG_LogRef *LOG_LIB_IDTYPE;
extern CLG_LogRef *LOG_LIB_LIBRARY;
extern CLG_LogRef *LOG_LIB_LINK_APPEND;
extern CLG_LogRef *LOG_LIB_MAIN_NAMEMAP;
extern CLG_LogRef *LOG_LIB_MAIN;
extern CLG_LogRef *LOG_LIB_OVERRIDE;
extern CLG_LogRef *LOG_LIB_OVERRIDE_PROXY_CONVERSION;
extern CLG_LogRef *LOG_LIB_OVERRIDE_RESYNC;
extern CLG_LogRef *LOG_LIB_PACKEDFILE;
extern CLG_LogRef *LOG_LIB_QUERY;
extern CLG_LogRef *LOG_LIB_REMAP;

/* RNA */
extern CLG_LogRef *LOG_MAKESRNA;

/* Objects, collections and layers */
extern CLG_LogRef *LOG_OBJECT;
extern CLG_LogRef *LOG_OBJECT_COLLECTION;
extern CLG_LogRef *LOG_OBJECT_CONSTRAINT;
extern CLG_LogRef *LOG_OBJECT_DYNAMICPAINT;
extern CLG_LogRef *LOG_OBJECT_EDIT;
extern CLG_LogRef *LOG_OBJECT_LAYER;
extern CLG_LogRef *LOG_OBJECT_MODIFIER;

/* Editors */
extern CLG_LogRef *LOG_EDITOR_OUTLINER;

/* Sculpt & Paint */
extern CLG_LogRef *LOG_PAINT_VERTEX;
extern CLG_LogRef *LOG_SCULPT;
extern CLG_LogRef *LOG_SCULPT_BMESH;
extern CLG_LogRef *LOG_SCULPT_DETAIL;

/* Physics */
extern CLG_LogRef *LOG_PHYSICS_FLUID;
extern CLG_LogRef *LOG_PHYSICS_POINTCACHE;
extern CLG_LogRef *LOG_PHYSICS_RIGIDBODY;
extern CLG_LogRef *LOG_PHYSICS_SOFTBODY;

/* Rendering */
extern CLG_LogRef *LOG_RENDER;
extern CLG_LogRef *LOG_CYCLES;

/* Report system */
extern CLG_LogRef *LOG_REPORTS;

/* RNA */
extern CLG_LogRef *LOG_RNA_ACCESS_COMPARE_OVERRIDE;
extern CLG_LogRef *LOG_RNA_ACCESS;
extern CLG_LogRef *LOG_RNA_DEFINE;
extern CLG_LogRef *LOG_RNA_RNA_COMPARE_OVERRIDE;

/* A few datablock that don't fit anywhere else */
extern CLG_LogRef *LOG_MASK;
extern CLG_LogRef *LOG_MASK_RASTERIZE;
extern CLG_LogRef *LOG_MATERIAL;
extern CLG_LogRef *LOG_NODE;
extern CLG_LogRef *LOG_SOUND;

/* System functionality */
extern CLG_LogRef *LOG_SYSTEM_PATH;

/* Events, operators, tools, msgbus */
extern CLG_LogRef *LOG_EVENTS;
extern CLG_LogRef *LOG_OPERATORS;
extern CLG_LogRef *LOG_TOOL_GIZMO;
extern CLG_LogRef *LOG_MSGBUS_PUB;
extern CLG_LogRef *LOG_MSGBUS_SUB;

/* Translation */
extern CLG_LogRef *LOG_TRANSLATION;

/* Undo system */
extern CLG_LogRef *LOG_UNDO;
extern CLG_LogRef *LOG_UNDO_ARMATURE;
extern CLG_LogRef *LOG_UNDO_CURVE;
extern CLG_LogRef *LOG_UNDO_CURVES;
extern CLG_LogRef *LOG_UNDO_FONT;
extern CLG_LogRef *LOG_UNDO_GREASEPENCIL;
extern CLG_LogRef *LOG_UNDO_IMAGE;
extern CLG_LogRef *LOG_UNDO_LATTICE;
extern CLG_LogRef *LOG_UNDO_MBALL;
extern CLG_LogRef *LOG_UNDO_MESH;
extern CLG_LogRef *LOG_UNDO_PARTICLE;
extern CLG_LogRef *LOG_UNDO_POINTCLOUD;
extern CLG_LogRef *LOG_UNDO_SCULPT;

/* XR and VR */
extern CLG_LogRef *LOG_XR;
