/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Standard attribute names for the Instance domain (#AttrDomain::Instance).
 *
 * These names define a shared vocabulary between geometry node trees, render
 * delegates, importers/exporters, and any other system that reads or writes
 * per-instance data. Using these constants instead of raw string literals
 * prevents typo-bugs and makes it easy to grep for every consumer of a given
 * attribute.
 *
 * Attribute types and domains are listed next to each constant so that a
 * reader does not have to cross-reference the implementation.
 *
 * ## Design rationale
 * Blender's #bke::Instances already stores arbitrary named attributes via
 * #bke::AttributeStorage. What was missing is a stable, documented set of
 * *well-known* names with agreed-upon semantics — the Instance-domain
 * equivalent of the built-in "position", "normal", and "id" attributes that
 * exist for Mesh and PointCloud.
 *
 * The names follow the `instance_*` prefix convention to:
 *   - avoid clashes with point/face attributes when geometry is converted,
 *   - make the domain immediately obvious in the Spreadsheet editor,
 *   - mirror Houdini's `crowd_*` / `agent*` convention while staying generic.
 */

#include "BLI_string_ref.hh"

namespace blender::bke::instances {

/* -------------------------------------------------------------------- */
/** \name Identity & Lifecycle
 * \{ */

/**
 * Unique integer identifier for each instance, stable across frames.
 * Used for motion blur, per-instance f-curves, and selection tracking.
 *
 * Domain: Instance  |  Type: int32
 *
 * \note This is distinct from the built-in anonymous "id" attribute.
 * Unlike the anonymous id, this one is named, persistent, and intended
 * to be set explicitly by the user or a cache-reader node.
 */
constexpr StringRef ATTR_INSTANCE_ID = "instance_id";

/**
 * When false the instance is excluded from viewport display and rendering.
 * Equivalent to Houdini's `crowdactive` point attribute.
 *
 * Domain: Instance  |  Type: bool
 */
constexpr StringRef ATTR_INSTANCE_ENABLED = "instance_enabled";

/** \} */

/* -------------------------------------------------------------------- */
/** \name Animation State
 *
 * These attributes drive the procedural deformation pipeline:
 * a deform evaluator reads them at eval-time, looks up the corresponding
 * bone matrices from the referenced AgentDefinition (or Action), and
 * applies Linear Blend Skinning to produce the deformed mesh.
 *
 * All clip attributes operate in pairs (_a / _b) to support two-clip
 * blending, which covers walk→run, idle→wave, and similar transitions.
 * \{ */

/**
 * Index into the AgentDefinition's clip array for the primary animation clip.
 * -1 means "no clip" (instance stays in bind pose).
 *
 * Domain: Instance  |  Type: int32
 */
constexpr StringRef ATTR_INSTANCE_CLIP_INDEX = "instance_clip_index";

/**
 * Playback position within the primary clip, in seconds from clip start.
 * The deform evaluator interpolates between adjacent baked frames.
 *
 * Domain: Instance  |  Type: float
 */
constexpr StringRef ATTR_INSTANCE_CLIP_TIME = "instance_clip_time";

/**
 * Blend weight of the primary clip in the range [0, 1].
 * When 1.0 only the primary clip contributes; when 0.0 only the secondary.
 * Values outside [0, 1] are clamped.
 *
 * Domain: Instance  |  Type: float
 */
constexpr StringRef ATTR_INSTANCE_CLIP_WEIGHT = "instance_clip_weight";

/**
 * Index of the secondary clip used for blending.
 * Ignored when #ATTR_INSTANCE_CLIP_WEIGHT is 1.0.
 *
 * Domain: Instance  |  Type: int32
 */
constexpr StringRef ATTR_INSTANCE_CLIP_INDEX_B = "instance_clip_index_b";

/**
 * Playback position within the secondary clip, in seconds from clip start.
 *
 * Domain: Instance  |  Type: float
 */
constexpr StringRef ATTR_INSTANCE_CLIP_TIME_B = "instance_clip_time_b";

/** \} */

/* -------------------------------------------------------------------- */
/** \name Locomotion
 * \{ */

/**
 * World-space velocity of the instance (meters per second).
 * Used for:
 *   - motion blur (render delegate reads this),
 *   - locomotion engine: scales clip playback speed to match movement,
 *   - foot IK: detects when feet should be planted.
 *
 * Domain: Instance  |  Type: float3
 */
constexpr StringRef ATTR_INSTANCE_VELOCITY = "instance_velocity";

/**
 * Normalized world-space facing direction of the instance.
 * Derived from velocity by default; can be overridden for creatures
 * that strafe or move backwards.
 *
 * Domain: Instance  |  Type: float3
 */
constexpr StringRef ATTR_INSTANCE_HEADING = "instance_heading";

/**
 * Scalar speed (magnitude of velocity) in meters per second.
 * Stored separately so the locomotion engine can read it cheaply without
 * recomputing length(velocity) per instance per frame.
 *
 * Domain: Instance  |  Type: float
 */
constexpr StringRef ATTR_INSTANCE_SPEED = "instance_speed";

/** \} */

/* -------------------------------------------------------------------- */
/** \name Appearance
 * \{ */

/**
 * Level-of-detail selector:
 *   0 = full resolution mesh
 *   1 = medium LOD
 *   2 = low LOD
 *   3 = bounding box only
 *
 * Typically driven by camera distance via a GN tree, but can be set
 * statically for background elements.
 *
 * Domain: Instance  |  Type: int32
 */
constexpr StringRef ATTR_INSTANCE_LOD_LEVEL = "instance_lod_level";

/**
 * Index into the AgentDefinition's shape-variant array.
 * Selects which mesh variant (body type, costume, props) this instance uses.
 * All variants share the same rig, so clip_index remains valid regardless.
 *
 * Domain: Instance  |  Type: int32
 */
constexpr StringRef ATTR_INSTANCE_SHAPE_VARIANT = "instance_shape_variant";

/**
 * Per-instance RGBA tint applied as a multiplier in the shader.
 * The shader must explicitly read this attribute; it is not applied
 * automatically by the render engine.
 *
 * Domain: Instance  |  Type: float4 (linear RGBA)
 */
constexpr StringRef ATTR_INSTANCE_COLOR = "instance_color";

/** \} */

/* -------------------------------------------------------------------- */
/** \name Simulation State (for future crowd sim integration)
 * \{ */

/**
 * Opaque integer state tag written by a simulation node and read back
 * on the next frame. Semantics are user-defined; typical use is a
 * finite-state-machine state index (0=idle, 1=walk, 2=run, …).
 *
 * Domain: Instance  |  Type: int32
 */
constexpr StringRef ATTR_INSTANCE_SIM_STATE = "instance_sim_state";

/**
 * Arbitrary float payload carried by the instance across frames.
 * Equivalent to Golaem's "Blind Data" and Houdini's point attributes
 * that survive simulation steps. Useful for timers, counters, RNG seeds.
 *
 * Domain: Instance  |  Type: float
 */
constexpr StringRef ATTR_INSTANCE_USER_FLOAT = "instance_user_float";

/** \} */

}  // namespace blender::bke::instances
