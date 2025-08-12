/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_color.hh"
#include "BLI_math_base.hh"

#include "BKE_volume.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_openvdb.hh"

#include "GEO_points_to_volume.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/LevelSetUtil.h>
#  include <openvdb/tools/ParticlesToLevelSet.h>

namespace blender::geometry {

/* Implements the interface required by #openvdb::tools::ParticlesToLevelSet. */
template<typename AttrType = void> class OpenVDBParticleList {
 public:
  using PosType = openvdb::Vec3R;

 private:
  Span<float3> positions_;
  Span<float> radii_;
  Span<AttrType> attribute_;
  float voxel_size_inv_;

 public:
  OpenVDBParticleList(const Span<float3> positions,
                      const Span<float> radii,
                      const float voxel_size)
      : positions_(positions), radii_(radii), voxel_size_inv_(math::rcp(voxel_size))
  {
    BLI_assert(voxel_size > 0.0f);
  }

  OpenVDBParticleList(const Span<float3> positions,
                      const Span<float> radii,
                      const Span<AttrType> attribute,
                      const float voxel_size)
      : positions_(positions),
        radii_(radii),
        attribute_(attribute),
        voxel_size_inv_(math::rcp(voxel_size))
  {
    BLI_assert(voxel_size > 0.0f);
  }

  size_t size() const
  {
    return size_t(positions_.size());
  }

  void getPos(size_t n, openvdb::Vec3R &xyz) const
  {
    float3 pos = positions_[n] * voxel_size_inv_;
    /* Better align generated grid with source points. */
    pos -= float3(0.5f);
    xyz = &pos.x;
  }

  void getPosRad(size_t n, openvdb::Vec3R &xyz, openvdb::Real &radius) const
  {
    this->getPos(n, xyz);
    radius = radii_[n] * voxel_size_inv_;
  }

  void getAtt(size_t n, float &att) const
  {
    att = attribute_[n];
  }

  void getAtt(size_t n, openvdb::Vec3f &att) const
  {
    att = openvdb::Vec3f(static_cast<const float *>(attribute_[n]));
  }
};

static openvdb::FloatGrid::Ptr points_to_sdf_grid_impl(const Span<float3> positions,
                                                       const Span<float> radii,
                                                       const float voxel_size)
{
  if (!BKE_volume_voxel_size_valid(float3(voxel_size))) {
    return nullptr;
  }

  /* Create a new grid that will be filled. #ParticlesToLevelSet requires
   * the background value to be positive */
  openvdb::FloatGrid::Ptr new_grid = openvdb::FloatGrid::create(1.0f);

  /* Create a narrow-band level set grid based on the positions and radii. */
  openvdb::tools::ParticlesToLevelSet op{*new_grid};
  /* Don't ignore particles based on their radius. */
  op.setRmin(0.0f);
  op.setRmax(std::numeric_limits<float>::max());
  OpenVDBParticleList<float> particles{positions, radii, voxel_size};
  op.rasterizeSpheres(particles);
  op.finalize();

  new_grid->transform().postScale(voxel_size);
  new_grid->setGridClass(openvdb::GRID_LEVEL_SET);

  return new_grid;
}

template<typename AttrType = void, typename AttrGrid = void>
static openvdb::FloatGrid::Ptr points_to_sdf_grid_with_attr_impl(const Span<float3> positions,
                                                                 const Span<float> radii,
                                                                 const Span<AttrType> attribute,
                                                                 const float voxel_size,
                                                                 typename AttrGrid::Ptr &attr_grid)
{
  if (!BKE_volume_voxel_size_valid(float3(voxel_size))) {
    return nullptr;
  }

  /* Create a new grid that will be filled. #ParticlesToLevelSet requires
   * the background value to be positive */
  openvdb::FloatGrid::Ptr new_grid = openvdb::FloatGrid::create(1.0f);

  /* Create a narrow-band level set grid based on the positions and radii. */
  openvdb::tools::ParticlesToLevelSet<openvdb::FloatGrid, typename AttrGrid::ValueType> op{
      *new_grid};
  /* Don't ignore particles based on their radius. */
  op.setRmin(0.0f);
  op.setRmax(std::numeric_limits<float>::max());
  OpenVDBParticleList<AttrType> particles{positions, radii, attribute, voxel_size};
  op.rasterizeSpheres(particles);
  op.finalize();

  new_grid->transform().postScale(voxel_size);
  new_grid->setGridClass(openvdb::GRID_LEVEL_SET);

  attr_grid = op.attributeGrid();
  attr_grid->transform().postScale(voxel_size);
  attr_grid->setGridClass(openvdb::GRID_FOG_VOLUME);

  return new_grid;
}

bke::VolumeGrid<float> points_to_sdf_grid(const Span<float3> positions,
                                          const Span<float> radii,
                                          const float voxel_size)
{
  return bke::VolumeGrid<float>(points_to_sdf_grid_impl(positions, radii, voxel_size));
}

bke::VolumeGridData *fog_volume_grid_add_from_points(Volume *volume,
                                                     const StringRefNull name,
                                                     const Span<float3> positions,
                                                     const Span<float> radii,
                                                     const Span<ColorGeometry4f> colors,
                                                     const float voxel_size,
                                                     const float density)
{
  openvdb::Vec3fGrid::Ptr color_grid;
  openvdb::FloatGrid::Ptr density_grid;

  if (colors.is_empty()) {
    density_grid = points_to_sdf_grid_impl(positions, radii, voxel_size);
  }
  else {
    density_grid = points_to_sdf_grid_with_attr_impl<ColorGeometry4f, openvdb::Vec3fGrid>(
        positions, radii, colors, voxel_size, color_grid);
  }
  density_grid->setGridClass(openvdb::GRID_FOG_VOLUME);

  /* Convert the level set to a fog volume. This also sets the background value to zero. Inside the
   * fog there will be a density of 1. */
  openvdb::tools::sdfToFogVolume(*density_grid);

  /* Take the desired density into account. */
  openvdb::tools::foreach(density_grid->beginValueOn(),
                          [&](const openvdb::FloatGrid::ValueOnIter &iter) {
                            iter.modifyValue([&](float &value) { value *= density; });
                          });

  bke::VolumeGridData *grid_data = BKE_volume_grid_add_vdb(*volume, name, std::move(density_grid));
  if (color_grid) {
    BKE_volume_grid_add_vdb(*volume, "color", std::move(color_grid));
  }
  return grid_data;
}

}  // namespace blender::geometry
#endif
