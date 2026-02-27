/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_color.hh"
#include "BLI_math_base.hh"
#include "BLI_math_rotation.hh"
#include "BLI_string_utils.hh"

#include "BKE_attribute_math.hh"
#include "BKE_volume.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_grid_type_traits.hh"
#include "BKE_volume_openvdb.hh"

#include "GEO_points_to_volume.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/points/PointConversion.h>
#  include <openvdb/points/PointTransfer.h>
#  include <openvdb/tools/LevelSetUtil.h>
#  include <openvdb/tools/Morphology.h>
#  include <openvdb/tools/ParticlesToLevelSet.h>
#  include <openvdb/tools/PointIndexGrid.h>

namespace blender::geometry {

/* Implements the interface required by #openvdb::tools::ParticlesToLevelSet. */
class OpenVDBParticleList {
 public:
  using PosType = openvdb::Vec3R;

 private:
  Span<float3> positions_;
  Span<float> radii_;
  float voxel_size_inv_;

 public:
  OpenVDBParticleList(const Span<float3> positions,
                      const Span<float> radii,
                      const float voxel_size)
      : positions_(positions), radii_(radii), voxel_size_inv_(math::rcp(voxel_size))
  {
    BLI_assert(voxel_size > 0.0f);
  }

  size_t size() const
  {
    return size_t(positions_.size());
  }

  void getPos(size_t n, openvdb::Vec3R &xyz) const
  {
    const float3 pos = positions_[n] * voxel_size_inv_;
    xyz = &pos.x;
  }

  void getPosRad(size_t n, openvdb::Vec3R &xyz, openvdb::Real &radius) const
  {
    this->getPos(n, xyz);
    radius = radii_[n] * voxel_size_inv_;
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
  OpenVDBParticleList particles{positions, radii, voxel_size};
  op.rasterizeSpheres(particles);
  op.finalize();

  new_grid->transform().postScale(voxel_size);
  new_grid->setGridClass(openvdb::GRID_LEVEL_SET);

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
                                                     const float voxel_size,
                                                     const float density)
{
  openvdb::FloatGrid::Ptr new_grid = points_to_sdf_grid_impl(positions, radii, voxel_size);
  new_grid->setGridClass(openvdb::GRID_FOG_VOLUME);

  /* Convert the level set to a fog volume. This also sets the background value to zero. Inside the
   * fog there will be a density of 1. */
  openvdb::tools::sdfToFogVolume(*new_grid);

  /* Take the desired density into account. */
  openvdb::tools::foreach(new_grid->beginValueOn(),
                          [&](const openvdb::FloatGrid::ValueOnIter &iter) {
                            iter.modifyValue([&](float &value) { value *= density; });
                          });

  return BKE_volume_grid_add_vdb(*volume, name, std::move(new_grid));
}

/* Helper class providing a point data interface to OpenVDB. */
template<typename T> class PointAttributeSpan {
 private:
  Span<T> data_;

 public:
  using type_traits = bke::VolumeGridTraits<T>;

  using value_type = typename type_traits::PrimitiveType;
  using PosType = value_type;

  PointAttributeSpan(const Span<T> data) : data_(data) {}

  size_t size() const
  {
    return data_.size();
  }
  void getPos(size_t n, PosType &xyz) const
  {
    xyz = type_traits::to_openvdb(data_[n]);
  }
  void get(value_type &value, size_t n) const
  {
    value = type_traits::to_openvdb(data_[n]);
  }
  void get(value_type &value, size_t n, openvdb::Index m) const
  {
    value = type_traits::to_openvdb(data_[n + m]);
  }
};

static openvdb::math::Transform get_vdb_transform(const float4x4 &transform)
{
  openvdb::math::Mat4f matrix_openvdb;
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      matrix_openvdb(col, row) = transform[col][row];
    }
  }

  return openvdb::math::Transform(std::make_shared<openvdb::math::AffineMap>(matrix_openvdb));
}

bool is_point_attribute_grid_supported(const CPPType &cpp_type)
{
  return ELEM(cpp_type,
              CPPType::get<bool>(),
              CPPType::get<float>(),
              CPPType::get<int>(),
              CPPType::get<int64_t>(),
              CPPType::get<float3>(),
              CPPType::get<int3>());
}

/* Remove characters that are invalid for OpenVDB attribute names. */
static std::string sanitize_name_for_openvdb(const StringRef name)
{
  /* Based on openvdb::points::AttributeSet::Descriptor::validName. */
  std::string result = name;
  result.erase(std::remove_if(result.begin(),
                              result.end(),
                              [](const char c) {
                                return !(isalnum(c) || (c == '_') || (c == '|') || (c == ':'));
                              }),
               result.end());
  return result;
}

/* Find a unique attribute name based on a generic string that may contain invalid characters. */
[[maybe_unused]] static std::string add_unique_vdb_attribute_name(
    const StringRef name, VectorSet<std::string> &used_names)
{
  const std::string vdb_base_name = sanitize_name_for_openvdb(name);

  std::string vdb_name = vdb_base_name;
  int duplicates = 0;
  while (!used_names.add(vdb_name)) {
    ++duplicates;
    vdb_name = vdb_base_name + "_" + std::to_string(duplicates);
  }
  return vdb_name;
}

static std::optional<StringRef> find_vdb_attribute_name(const PointAttributeNameMap &attribute_map,
                                                        const StringRef name)
{
  for (const std::pair<std::string, std::string> &name_pair : attribute_map) {
    if (name_pair.first == name) {
      return name_pair.second;
    }
  }
  return std::nullopt;
}

MappedPointDataGrid points_to_point_data_grid(const Span<float3> positions,
                                              const Span<PointDataGridAttributeInfo> attributes,
                                              const float4x4 &transform)
{
  const PointAttributeSpan positions_wrapper(positions);
  const openvdb::math::Transform vdb_transform = get_vdb_transform(transform);

  /* Create point index grid in advance so it can be used for all attribute grids. */
  openvdb::tools::PointIndexGrid::Ptr point_index_grid =
      openvdb::tools::createPointIndexGrid<openvdb::tools::PointIndexGrid>(positions_wrapper,
                                                                           vdb_transform);

  /* Convert the main positions array. */
  openvdb::points::PointDataGrid::Ptr point_data_grid =
      openvdb::points::createPointDataGrid<openvdb::points::NullCodec,
                                           openvdb::points::PointDataGrid>(
          *point_index_grid, positions_wrapper, vdb_transform);

  VectorSet<std::string> used_names;
  Vector<std::pair<std::string, std::string>> attribute_map;
  for (const PointDataGridAttributeInfo &info : attributes) {
    const CPPType &cpp_type = info.data.type();
    bke::attribute_math::to_static_type(cpp_type, [&]<typename ValueT>() {
      using type_traits = typename bke::VolumeGridTraits<ValueT>;

      if constexpr (!std::is_same_v<typename type_traits::TreeType, void>) {
        /* Note: some attributes could benefit from specialized codecs. The OpenVDB cookbook
         * suggests to store e.g. radius attribute with a fixed-point codec. This is not supported
         * here yet.
         */
        // openvdb::points::FixedPointCodec</*1-byte=*/false, openvdb::points::UnitRange>;
        using Codec = openvdb::points::NullCodec;
        using AttributeArray =
            openvdb::points::TypedAttributeArray<typename type_traits::PrimitiveType, Codec>;
        if (!AttributeArray::isRegistered()) {
          AttributeArray::registerType();
        }

        const std::string vdb_name = add_unique_vdb_attribute_name(info.name, used_names);
        attribute_map.append_as(info.name, vdb_name);

        openvdb::NamePair point_data_attribute = AttributeArray::attributeType();
        openvdb::points::appendAttribute(point_data_grid->tree(), vdb_name, point_data_attribute);

        const PointAttributeSpan data_wrapper(info.data.typed<ValueT>());
        openvdb::points::populateAttribute(
            point_data_grid->tree(), point_index_grid->tree(), vdb_name, data_wrapper);
      }
    });
  }

  return {bke::GVolumeGrid(std::move(point_data_grid)), std::move(attribute_map)};
}

MappedPointDataGrid points_to_point_data_grid(const VArray<float3> positions,
                                              const bke::AttributeAccessor &attributes,
                                              const bke::AttributeFilter &attribute_filter,
                                              const float4x4 &transform)
{
  const VArraySpan<float3> positions_span = positions;

  Vector<GVArraySpan> attribute_arrays;
  Vector<PointDataGridAttributeInfo> attributes_info;
  MappedPointDataGrid result;
  attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (attribute_filter.allow_skip(iter.name)) {
      return;
    }
    const CPPType &cpp_type = bke::attribute_type_to_cpp_type(iter.data_type);
    if (!is_point_attribute_grid_supported(cpp_type)) {
      return;
    }
    const bke::GAttributeReader reader = iter.get(bke::AttrDomain::Point);
    if (!reader) {
      return;
    }
    attribute_arrays.append(*reader);
    attributes_info.append({iter.name, attribute_arrays.last()});

    result = points_to_point_data_grid(positions_span, attributes_info, transform);
  });

  return result;
}

/* Note: openvdb::VolumeTransfer supports writing to multiple destination grids at once, but
 * requires known static types for each grid. This could be useful for efficiently rasterizing all
 * attributes at the same time, but would also generate a lot of code if using generic attribute
 * combinations. For now just output a single attribute at a time. */
// https://github.com/AcademySoftwareFoundation/openvdb/blob/9437cd7867eb67600cc855b11c145cf5db40bdc3/openvdb/openvdb/unittest/TestPointRasterizeTrilinear.cc#L209

template<typename GridValueT, KernelType kernel_type>
struct KernelTransferBase : public openvdb::points::TransformTransfer,
                            public openvdb::points::VolumeTransfer<
                                typename bke::VolumeGridTraits<GridValueT>::TreeType> {
  using GridTraits = bke::VolumeGridTraits<GridValueT>;
  using TreeType = typename GridTraits::TreeType;
  using GridType = openvdb::Grid<TreeType>;
  using GridValueType = typename GridTraits::PrimitiveType;
  using NodeMaskType = typename TreeType::LeafNodeType::NodeMaskType;

  static const int32_t LOG2DIM = TreeType::LeafNodeType::LOG2DIM;
  static const int32_t DIM = TreeType::LeafNodeType::DIM;

  static constexpr int voxel_range_ = kernel_functions::kernel_voxel_range<kernel_type>();
  static constexpr float inv_voxel_range_ = 1.0f / voxel_range_;

  /* Point attribute handles for positions in the current leaf. */
  std::unique_ptr<openvdb::points::AttributeHandle<openvdb::Vec3f>> position_handle_;

  KernelTransferBase(const openvdb::points::PointDataGrid &source, GridType &dest)
      : TransformTransfer(source.transform(), dest.transform()),
        openvdb::points::VolumeTransfer<TreeType>(dest.tree()),
        position_handle_(nullptr)
  {
  }

  KernelTransferBase(const KernelTransferBase &other)
      : TransformTransfer(other),
        openvdb::points::VolumeTransfer<TreeType>(other),
        position_handle_(nullptr)
  {
  }

  /* Voxel range of the target grid to cover. */
  openvdb::Int32 range(const openvdb::Coord & /*leaf_origin*/, size_t /*leaf_idx*/) const
  {
    return openvdb::Int32(voxel_range_);
  }

  void update_positions(const openvdb::points::PointDataTree::LeafNodeType &leaf)
  {
    position_handle_.reset(
        new openvdb::points::AttributeHandle<openvdb::Vec3f>(leaf.constAttributeArray("P")));
  }

  /* For each point, compute its relative index space position in the destination tree and
   * sum a function of per-point values.
   *
   * \param ijk Point voxel coordinate which contains the point.
   * \param point_index Index of the point within its leaf node buffer.
   * \param target_bounds Coordinate region of the destination tree to add into.
   */
  template<typename ValueFn>
  void add_point_to_voxels(const openvdb::Coord &ijk,
                           const openvdb::Index point_index,
                           const openvdb::CoordBBox &target_bounds,
                           ValueFn value_fn)
  {
    openvdb::CoordBBox intersect_box(ijk.offsetBy(-voxel_range_), ijk.offsetBy(voxel_range_));
    intersect_box.intersect(target_bounds);
    if (intersect_box.empty()) {
      return;
    }

    auto *const data = this->template buffer<0>();
    const auto &mask = *(this->template mask<0>());

    const openvdb::Vec3d source_position = ijk.asVec3d() +
                                           this->position_handle_->get(point_index);
    const openvdb::Vec3d target_position = this->transformSourceToTarget(source_position);

    const openvdb::Coord &a(intersect_box.min());
    const openvdb::Coord &b(intersect_box.max());
    for (openvdb::Coord c = a; c.x() <= b.x(); ++c.x()) {
      const openvdb::Index i = ((c.x() & (DIM - 1u)) << 2 * LOG2DIM);
      for (c.y() = a.y(); c.y() <= b.y(); ++c.y()) {
        const openvdb::Index j = ((c.y() & (DIM - 1u)) << LOG2DIM);
        for (c.z() = a.z(); c.z() <= b.z(); ++c.z()) {
          BLI_assert(target_bounds.isInside(c));
          const openvdb::Index offset = i + j + /*k*/ (c.z() & (DIM - 1u));
          if (!mask.isOn(offset)) {
            continue;
          }

          const float3 kernel_distance = float3(
              openvdb::Vec3f(c.asVec3d() - target_position).asV());
          if (kernel_functions::kernel_non_zero<kernel_type>(kernel_distance)) {
            const float weight = kernel_functions::kernel_eval<kernel_type>(kernel_distance);
            data[offset] += value_fn(kernel_distance, weight);
          }
        }
      }
    }
  }

  bool endPointLeaf(const openvdb::points::PointDataTree::LeafNodeType & /*leaf_node*/)
  {
    return true;
  }

  // XXX Example comment says:
  // "Return true for endPointLeaf() to continue, false for finalize() so we don't
  // recurse." but it looks like both should return "true"? Is this a bug in documentation?
  bool finalize(const openvdb::Coord & /*origin*/, size_t /*idx*/)
  {
    return true;
  }
};

template<typename AttributeT, typename GridValueT, KernelType kernel_type, bool weighted>
struct ValueSumTransfer : public KernelTransferBase<GridValueT, kernel_type> {
  using Base = KernelTransferBase<GridValueT, kernel_type>;
  using GridType = typename Base::GridType;
  using TreeType = typename Base::TreeType;
  using GridValueType = typename Base::GridValueType;

  using AttributeTraits = bke::VolumeGridTraits<AttributeT>;
  using AttributeType = typename AttributeTraits::PrimitiveType;

  StringRef value_attribute_;
  StringRef mass_attribute_;
  std::unique_ptr<openvdb::points::AttributeHandle<AttributeType>> value_handle_;
  std::unique_ptr<openvdb::points::AttributeHandle<float>> mass_handle_;

  ValueSumTransfer(const openvdb::points::PointDataGrid &source,
                   GridType &dest,
                   StringRef value_attribute)
      : KernelTransferBase<GridValueT, kernel_type>(source, dest),
        value_attribute_(value_attribute)
  {
  }

  ValueSumTransfer(const openvdb::points::PointDataGrid &source,
                   GridType &dest,
                   StringRef value_attribute,
                   StringRef mass_attribute)
      : KernelTransferBase<GridValueT, kernel_type>(source, dest),
        value_attribute_(value_attribute),
        mass_attribute_(mass_attribute)
  {
  }

  ValueSumTransfer(const ValueSumTransfer &other)
      : KernelTransferBase<GridValueT, kernel_type>(other),
        value_attribute_(other.value_attribute_),
        mass_attribute_(other.mass_attribute_)
  {
  }

  bool startPointLeaf(const openvdb::points::PointDataTree::LeafNodeType &leaf)
  {
    this->update_positions(leaf);
    BLI_assert(leaf.hasAttribute(value_attribute_));
    value_handle_.reset(new openvdb::points::AttributeHandle<AttributeType>(
        leaf.constAttributeArray(value_attribute_)));
    if constexpr (weighted) {
      BLI_assert(leaf.hasAttribute(mass_attribute_));
      mass_handle_.reset(
          new openvdb::points::AttributeHandle<float>(leaf.constAttributeArray(mass_attribute_)));
    }
    return true;
  }

  void rasterizePoint(const openvdb::Coord &ijk,
                      const openvdb::Index point_index,
                      const openvdb::CoordBBox &target_bounds)
  {
    const AttributeType source_value = value_handle_->get(point_index);

    if constexpr (weighted) {
      const AttributeType source_value = value_handle_->get(point_index);
      const float source_mass = mass_handle_ ? mass_handle_->get(point_index) : 0.0f;

      this->add_point_to_voxels(ijk,
                                point_index,
                                target_bounds,
                                [&](const float3 & /*kernel_distance*/, const float weight) {
                                  return weight * source_value * source_mass;
                                });
    }
    else {
      this->add_point_to_voxels(ijk,
                                point_index,
                                target_bounds,
                                [&](const float3 & /*kernel_distance*/, const float weight) {
                                  return weight * source_value;
                                });
    }
  }
};

template<typename GridType, KernelType kernel_type>
static typename GridType::Ptr prepare_destination_grid(
    const openvdb::points::PointDataGrid &point_data_grid, const float4x4 &transform)
{
  typename std::shared_ptr<GridType> dst_grid = GridType::create();
  dst_grid->transform() = get_vdb_transform(transform);
  /* Activate all voxels with particles in them. */
  dst_grid->tree().topologyUnion(point_data_grid.tree());
  /* Dilate to ensure all voxels within range of a particle are active. */
  const int voxel_range = kernel_functions::kernel_voxel_range(kernel_type);
  openvdb::tools::dilateActiveValues(dst_grid->tree(),
                                     voxel_range,
                                     openvdb::tools::NN_FACE_EDGE_VERTEX,
                                     openvdb::tools::TilePolicy::PRESERVE_TILES,
                                     true);
  /* Voxelize all tiles since each voxel gets a different value. */
  dst_grid->tree().voxelizeActiveTiles(true);
  return dst_grid;
}

template<typename AttributeT, typename GridValueT, KernelType kernel_type>
static bke::VolumeGrid<GridValueT> points_rasterize_with_kernel(
    const openvdb::points::PointDataGrid &point_data_grid,
    const StringRef value_attribute,
    const PointRasterizeAttributeInfo &attribute_info,
    const float4x4 &transform)
{
  using GridTraits = bke::VolumeGridTraits<GridValueT>;
  using TreeType = typename GridTraits::TreeType;
  using GridType = openvdb::Grid<TreeType>;

  typename std::shared_ptr<GridType> dst_grid = prepare_destination_grid<GridType, kernel_type>(
      point_data_grid, transform);

  BLI_assert(!value_attribute.is_empty());
  ValueSumTransfer<AttributeT, GridValueT, kernel_type, false> transfer(
      point_data_grid, *dst_grid, value_attribute);
  openvdb::points::rasterize(point_data_grid, transfer);

  if constexpr (std::is_same_v<GridValueT, float3>) {
    if (attribute_info.use_staggered_vector) {
      /* Note: Due to the separable kernel function the weight at each of the face centers is the
       * same as the weight at the voxel corner. Rasterizing a staggered velocity is no different
       * from rasterizing a centered vector, and only require declaring the output staggered, and
       * then moving the grid origin to make the transform voxel-centered. */
      dst_grid->setGridClass(openvdb::GridClass::GRID_STAGGERED);
      dst_grid->transform().preTranslate(openvdb::Vec3d(0.5, 0.5, 0.5));
    }
    else {
      dst_grid->setGridClass(openvdb::GridClass::GRID_FOG_VOLUME);
    }
  }
  else {
    dst_grid->setGridClass(openvdb::GridClass::GRID_FOG_VOLUME);
  }

  return bke::VolumeGrid<GridValueT>(std::move(dst_grid));
}

template<typename AttributeT, typename GridValueT>
static bke::VolumeGrid<GridValueT> points_rasterize_with_static_type(
    const openvdb::points::PointDataGrid &point_data_grid,
    const KernelType kernel_type,
    const StringRef value_attribute,
    const PointRasterizeAttributeInfo &attribute_info,
    const float4x4 &transform)
{
  switch (kernel_type) {
    case KernelType::Constant: {
      return points_rasterize_with_kernel<AttributeT, GridValueT, KernelType::Constant>(
          point_data_grid, value_attribute, attribute_info, transform);
    }
    case KernelType::Linear: {
      return points_rasterize_with_kernel<AttributeT, GridValueT, KernelType::Linear>(
          point_data_grid, value_attribute, attribute_info, transform);
    }
    case KernelType::Quadratic: {
      return points_rasterize_with_kernel<AttributeT, GridValueT, KernelType::Quadratic>(
          point_data_grid, value_attribute, attribute_info, transform);
    }
    case KernelType::Cubic: {
      return points_rasterize_with_kernel<AttributeT, GridValueT, KernelType::Cubic>(
          point_data_grid, value_attribute, attribute_info, transform);
    }
  }
  BLI_assert_unreachable();
  return {};
}

static bke::GVolumeGrid points_attribute_rasterize(
    const openvdb::points::PointDataGrid &point_data_grid,
    const KernelType kernel_type,
    const StringRef value_attribute,
    const PointRasterizeAttributeInfo &attribute_info,
    const float4x4 &transform)
{
  bke::GVolumeGrid result;
  bke::attribute_math::to_static_type(attribute_info.type, [&]<typename T>() {
    using TreeType = typename bke::VolumeGridTraits<T>::TreeType;
    if constexpr (std::is_same_v<TreeType, void>) {
      BLI_assert_unreachable();
    }
    else if constexpr (std::is_same_v<TreeType, openvdb::MaskTree> ||
                       std::is_same_v<TreeType, openvdb::BoolTree>)
    {
      // TODO not yet supported
      result = {};
    }
    else {
      // TODO support affine vector attribute conversion (float4x4 -> float3)
      using AttributeT = T;
      using GridValueT = T;
      result = points_rasterize_with_static_type<AttributeT, GridValueT>(
          point_data_grid, kernel_type, value_attribute, attribute_info, transform);
    }
  });
  return result;
}

void points_rasterize(const MappedPointDataGrid &point_data_grid,
                      const KernelType kernel_type,
                      Span<PointRasterizeAttributeInfo> point_attributes,
                      const float4x4 &transform,
                      MutableSpan<bke::GVolumeGrid> r_attribute_grids)
{
  BLI_assert(r_attribute_grids.size() == point_attributes.size());

  if (!point_data_grid.grid || point_data_grid.grid->grid_type() != VOLUME_GRID_POINTS) {
    return;
  }
  bke::VolumeTreeAccessToken access_token;
  const openvdb::points::PointDataGrid::ConstPtr vdb_point_data_grid =
      openvdb::GridBase::grid<openvdb::points::PointDataGrid>(
          point_data_grid.grid->grid_ptr(access_token));
  BLI_assert(vdb_point_data_grid);

  for (const int i : point_attributes.index_range()) {
    const PointRasterizeAttributeInfo &attribute = point_attributes[i];

    const std::optional<std::string> vdb_attribute_name = find_vdb_attribute_name(
        point_data_grid.attribute_map, attribute.name);
    BLI_assert(vdb_attribute_name.has_value());
    r_attribute_grids[i] = points_attribute_rasterize(
        *vdb_point_data_grid, kernel_type, *vdb_attribute_name, attribute, transform);
  }
}

}  // namespace blender::geometry
#endif
