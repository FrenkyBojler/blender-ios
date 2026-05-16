/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_geom_curves.bsl.hh"
#include "eevee_geom_mesh.bsl.hh"
#include "eevee_geom_pointcloud.bsl.hh"
#include "eevee_geom_volume.bsl.hh"
#include "eevee_geom_world.bsl.hh"
#include "eevee_surf_capture.bsl.hh"
#include "eevee_surf_deferred.bsl.hh"
#include "eevee_surf_depth.bsl.hh"
#include "eevee_surf_forward.bsl.hh"
#include "eevee_surf_hybrid.bsl.hh"
#include "eevee_surf_occupancy.bsl.hh"
#include "eevee_surf_shadow.bsl.hh"
#include "eevee_surf_volume.bsl.hh"
#include "eevee_surf_world.bsl.hh"

namespace eevee {

#if 0 /* Would be nice to enable. But first we need to incompatibility between the includes. */
/* clang-format off */
PipelineGraphic eevee_surface_world_world(geom_world, surf_world);
PipelineGraphic eevee_surface_world_curves(geom_curves, surf_world);
PipelineGraphic eevee_surface_world_mesh(geom_mesh, surf_world);
PipelineGraphic eevee_surface_world_pointcloud(geom_pointcloud, surf_world);
PipelineGraphic eevee_surface_world_volume(geom_volume, surf_world);
PipelineGraphic eevee_surface_depth_world(geom_world, surf_depth<true>);
PipelineGraphic eevee_surface_depth_curves(geom_curves, surf_depth<true>);
PipelineGraphic eevee_surface_depth_mesh(geom_mesh, surf_depth<true>);
PipelineGraphic eevee_surface_depth_pointcloud(geom_pointcloud, surf_depth<true>);
PipelineGraphic eevee_surface_depth_volume(geom_volume, surf_depth<true>);
PipelineGraphic eevee_surface_deferred_world(geom_world, surf_deferred);
PipelineGraphic eevee_surface_deferred_curves(geom_curves, surf_deferred);
PipelineGraphic eevee_surface_deferred_mesh(geom_mesh, surf_deferred);
PipelineGraphic eevee_surface_deferred_pointcloud(geom_pointcloud, surf_deferred);
PipelineGraphic eevee_surface_deferred_volume(geom_volume, surf_deferred);
PipelineGraphic eevee_surface_hybrid_world(geom_world, surf_deferred);
PipelineGraphic eevee_surface_hybrid_curves(geom_curves, surf_deferred);
PipelineGraphic eevee_surface_hybrid_mesh(geom_mesh, surf_deferred);
PipelineGraphic eevee_surface_hybrid_pointcloud(geom_pointcloud, surf_deferred);
PipelineGraphic eevee_surface_hybrid_volume(geom_volume, surf_deferred);
PipelineGraphic eevee_surface_forward_world(geom_world, surf_forward);
PipelineGraphic eevee_surface_forward_curves(geom_curves, surf_forward);
PipelineGraphic eevee_surface_forward_mesh(geom_mesh, surf_forward);
PipelineGraphic eevee_surface_forward_pointcloud(geom_pointcloud, surf_forward);
PipelineGraphic eevee_surface_forward_volume(geom_volume, surf_forward);
PipelineGraphic eevee_surface_capture_world(geom_world, surf_capture);
PipelineGraphic eevee_surface_capture_curves(geom_curves, surf_capture);
PipelineGraphic eevee_surface_capture_mesh(geom_mesh, surf_capture);
PipelineGraphic eevee_surface_capture_pointcloud(geom_pointcloud, surf_capture);
PipelineGraphic eevee_surface_capture_volume(geom_volume, surf_capture);
PipelineGraphic eevee_surface_volume_world(geom_world, surf_volume);
PipelineGraphic eevee_surface_volume_curves(geom_curves, surf_volume);
PipelineGraphic eevee_surface_volume_mesh(geom_mesh, surf_volume);
PipelineGraphic eevee_surface_volume_pointcloud(geom_pointcloud, surf_volume);
PipelineGraphic eevee_surface_volume_volume(geom_volume, surf_volume);
PipelineGraphic eevee_surface_occupancy_world(geom_world, surf_occupancy);
PipelineGraphic eevee_surface_occupancy_curves(geom_curves, surf_occupancy);
PipelineGraphic eevee_surface_occupancy_mesh(geom_mesh, surf_occupancy);
PipelineGraphic eevee_surface_occupancy_pointcloud(geom_pointcloud, surf_occupancy);
PipelineGraphic eevee_surface_occupancy_volume(geom_volume, surf_occupancy);
PipelineGraphic eevee_surface_shadow_world(geom_world, surf_shadow);
PipelineGraphic eevee_surface_shadow_curves(geom_curves, surf_shadow);
PipelineGraphic eevee_surface_shadow_mesh(geom_mesh, surf_shadow);
PipelineGraphic eevee_surface_shadow_pointcloud(geom_pointcloud, surf_shadow);
PipelineGraphic eevee_surface_shadow_volume(geom_volume, surf_shadow);
/* clang-format on */
#endif

}  // namespace eevee
