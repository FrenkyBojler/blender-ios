/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup balembic
 */

#include "abc_reader_object.h"
#include "abc_axis_conversion.h"
#include "abc_util.h"

#include "DNA_cachefile_types.h"
#include "DNA_constraint_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BKE_constraint.h"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_string.h"

#include "RNA_types.hh"

namespace blender {

using Alembic::Abc::Dimensions;
using Alembic::Abc::ICompoundProperty;
using Alembic::Abc::PropertyHeader;
using Alembic::AbcCoreAbstract::ArraySamplePtr;
using Alembic::AbcCoreAbstract::DataType;
using Alembic::AbcGeom::IArrayProperty;
using Alembic::AbcGeom::IObject;
using Alembic::AbcGeom::IScalarProperty;
using Alembic::AbcGeom::IXform;
using Alembic::AbcGeom::IXformSchema;
namespace io::alembic {

AbcReaderConstructorArgs create_reader_constructor_args(const IObject &object,
                                                        ImportSettings &settings)
{
  return AbcReaderConstructorArgs{.object = object, .settings = settings};
}

AbcObjectReader::AbcObjectReader(const AbcReaderConstructorArgs &args)
    : m_object(nullptr),
      m_iobject(args.object),
      m_settings(&args.settings),
      m_is_reading_a_file_sequence(args.settings.is_sequence),
      m_refcount(0),
      parent_reader(nullptr)
{
  m_name = m_iobject.getFullName();
  std::vector<std::string> parts;
  split(m_name, '/', parts);

  if (parts.size() >= 2) {
    m_object_name = parts[parts.size() - 2];
    m_data_name = parts[parts.size() - 1];
  }
  else {
    m_object_name = m_data_name = parts[parts.size() - 1];
  }

  determine_inherits_xform();
}

void AbcObjectReader::determine_inherits_xform()
{
  m_inherits_xform = false;

  IXform ixform = xform();
  if (!ixform) {
    return;
  }

  const IXformSchema &schema(ixform.getSchema());
  if (!schema.valid()) {
    std::cerr << "Alembic object " << ixform.getFullName() << " has an invalid schema."
              << std::endl;
    return;
  }

  m_inherits_xform = schema.getInheritsXforms();

  IObject ixform_parent = ixform.getParent();
  if (!ixform_parent.getParent()) {
    /* The archive top object certainly is not a transform itself, so handle
     * it as "no parent". */
    m_inherits_xform = false;
  }
  else {
    m_inherits_xform = ixform_parent && m_inherits_xform;
  }
}

const IObject &AbcObjectReader::iobject() const
{
  return m_iobject;
}

Object *AbcObjectReader::object() const
{
  return m_object;
}

void AbcObjectReader::object(Object *ob)
{
  m_object = ob;
}

static Imath::M44d blend_matrices(const Imath::M44d &m0,
                                  const Imath::M44d &m1,
                                  const double weight)
{
  float mat0[4][4], mat1[4][4], ret[4][4];

  /* Cannot use Imath::M44d::getValue() since this returns a pointer to
   * doubles and interp_m4_m4m4 expects pointers to floats. So need to convert
   * the matrices manually.
   */

  convert_matrix_datatype(m0, mat0);
  convert_matrix_datatype(m1, mat1);
  interp_m4_m4m4(ret, mat0, mat1, float(weight));
  return convert_matrix_datatype(ret);
}

Imath::M44d get_matrix(const IXformSchema &schema, const chrono_t time)
{
  Alembic::AbcGeom::ISampleSelector selector(time);

  const std::optional<SampleInterpolationSettings> interpolation_settings =
      get_sample_interpolation_settings(
          selector, schema.getTimeSampling(), schema.getNumSamples());

  if (!interpolation_settings.has_value()) {
    /* No interpolation, just read the current time. */
    Alembic::AbcGeom::XformSample s0;
    schema.get(s0, selector);
    return s0.getMatrix();
  }

  Alembic::AbcGeom::XformSample s0, s1;
  schema.get(s0, Alembic::AbcGeom::ISampleSelector(interpolation_settings->index));
  schema.get(s1, Alembic::AbcGeom::ISampleSelector(interpolation_settings->ceil_index));
  return blend_matrices(s0.getMatrix(), s1.getMatrix(), interpolation_settings->weight);
}

void AbcObjectReader::read_geometry(bke::GeometrySet & /*geometry_set*/,
                                    const Alembic::Abc::ISampleSelector & /*sample_sel*/,
                                    const AbcReadGeometryParams & /*read_params*/,
                                    const char ** /*r_err_str*/)
{
}

bool AbcObjectReader::topology_changed(const Mesh * /*existing_mesh*/,
                                       const Alembic::Abc::ISampleSelector & /*sample_sel*/)
{
  /* The default implementation of read_mesh() just returns the original mesh, so never changes the
   * topology. */
  return false;
}

void AbcObjectReader::setupObjectTransform(const chrono_t time)
{
  bool is_constant = false;
  float transform_from_alembic[4][4];

  /* If the parent is a camera, apply the inverse rotation to make up for the from-Maya rotation.
   * This assumes that the parent object also was imported from Alembic. */
  if (m_object->parent != nullptr && m_object->parent->type == OB_CAMERA) {
    axis_angle_to_mat4_single(m_object->parentinv, 'X', -M_PI_2);
  }

  this->read_matrix(transform_from_alembic, time, m_settings->scale, is_constant);

  /* Apply the matrix to the object. */
  BKE_object_apply_mat4(m_object, transform_from_alembic, true, false);
  BKE_object_to_mat4(m_object, m_object->runtime->object_to_world.ptr());

  if (!is_constant || m_settings->always_add_cache_reader) {
    bConstraint *con = BKE_constraint_add_for_object(
        m_object, nullptr, CONSTRAINT_TYPE_TRANSFORM_CACHE);
    bTransformCacheConstraint *data = static_cast<bTransformCacheConstraint *>(con->data);
    STRNCPY(data->object_path, m_iobject.getFullName().c_str());

    data->cache_file = m_settings->cache_file;
    id_us_plus(&data->cache_file->id);
  }
}

Alembic::AbcGeom::IXform AbcObjectReader::xform()
{
  /* Check that we have an empty object (locator, bone head/tail...). */
  if (IXform::matches(m_iobject.getMetaData())) {
    try {
      return IXform(m_iobject, Alembic::AbcGeom::kWrapExisting);
    }
    catch (Alembic::Util::Exception &ex) {
      printf("Alembic: error reading object transform for '%s': %s\n",
             m_iobject.getFullName().c_str(),
             ex.what());
      return IXform();
    }
  }

  /* Check that we have an object with actual data, in which case the
   * parent Alembic object should contain the transform. */
  IObject abc_parent = m_iobject.getParent();

  /* The archive's top object can be recognized by not having a parent. */
  if (abc_parent.getParent() && IXform::matches(abc_parent.getMetaData())) {
    try {
      return IXform(abc_parent, Alembic::AbcGeom::kWrapExisting);
    }
    catch (Alembic::Util::Exception &ex) {
      printf("Alembic: error reading object transform for '%s': %s\n",
             abc_parent.getFullName().c_str(),
             ex.what());
      return IXform();
    }
  }

  /* This can happen in certain cases. For example, MeshLab exports
   * point clouds without parent XForm. */
  return IXform();
}

void AbcObjectReader::read_matrix(float r_mat[4][4] /* local matrix */,
                                  const chrono_t time,
                                  const float scale,
                                  bool &r_is_constant)
{
  IXform ixform = xform();
  if (!ixform) {
    unit_m4(r_mat);
    r_is_constant = true;
    return;
  }

  const IXformSchema &schema(ixform.getSchema());
  if (!schema.valid()) {
    std::cerr << "Alembic object " << ixform.getFullName() << " has an invalid schema."
              << std::endl;
    return;
  }

  const Imath::M44d matrix = get_matrix(schema, time);
  convert_matrix_datatype(matrix, r_mat);
  copy_m44_axis_swap(r_mat, r_mat, ABC_ZUP_FROM_YUP);

  /* Convert from Maya to Blender camera orientation. Children of this camera
   * will have the opposite transform as their Parent Inverse matrix.
   * See AbcObjectReader::setupObjectTransform(). */
  if (m_object->type == OB_CAMERA) {
    float camera_rotation[4][4];
    axis_angle_to_mat4_single(camera_rotation, 'X', M_PI_2);
    mul_m4_m4m4(r_mat, r_mat, camera_rotation);
  }

  if (!m_inherits_xform) {
    /* Only apply scaling to root objects, parenting will propagate it. */
    float scale_mat[4][4];
    scale_m4_fl(scale_mat, scale);
    mul_m4_m4m4(r_mat, scale_mat, r_mat);
  }

  r_is_constant = schema.isConstant();
}

void AbcObjectReader::addCacheModifier()
{
  ModifierData *md = BKE_modifier_new(eModifierType_MeshSequenceCache);
  BLI_addtail(&m_object->modifiers, md);
  BKE_modifiers_persistent_uid_init(*m_object, *md);

  MeshSeqCacheModifierData *mcmd = reinterpret_cast<MeshSeqCacheModifierData *>(md);

  mcmd->cache_file = m_settings->cache_file;
  id_us_plus(&mcmd->cache_file->id);

  STRNCPY(mcmd->object_path, m_iobject.getFullName().c_str());
}

int AbcObjectReader::refcount() const
{
  return m_refcount;
}

void AbcObjectReader::incref()
{
  m_refcount++;
}

void AbcObjectReader::decref()
{
  m_refcount--;
  BLI_assert(m_refcount >= 0);
}

const ICompoundProperty AbcObjectReader::getArbGeomParams() const
{
  return {};
}

const ICompoundProperty AbcObjectReader::getUserProperties() const
{
  return {};
}

void AbcObjectReader::readIDProperties()
{
  readIDProperties(getArbGeomParams());
  readIDProperties(getUserProperties());
}

static std::optional<PropertySubType> get_property_subtype_for_interpretation(
    StringRef interpretation)
{
  if (interpretation == Alembic::Abc::C3fTPTraits::interpretation() ||
      interpretation == Alembic::Abc::C4fTPTraits::interpretation())
  {
    /* TODO(kevindietrich) : we might want to parameterize what space the colors are in.
     * For now, this can be changed in the UI, but it is not too convenient when we have
     * a lot of properties.
     */
    return PROP_COLOR;
  }
  if (interpretation == Alembic::Abc::V3fTPTraits::interpretation() ||
      interpretation == Alembic::Abc::P3fTPTraits::interpretation())
  {
    return PROP_XYZ;
  }
  if (interpretation == Alembic::Abc::N3fTPTraits::interpretation()) {
    return PROP_DIRECTION;
  }
  if (interpretation == Alembic::Abc::QuatfTPTraits::interpretation()) {
    return PROP_QUATERNION;
  }
  return {};
}

static void set_ui_data_from_interpretation(IDProperty *prop, StringRef interpretation)
{
  std::optional<PropertySubType> prop_subtype = get_property_subtype_for_interpretation(
      interpretation);
  if (prop_subtype.has_value()) {
    IDPropertyUIData *ui_data = IDP_ui_data_ensure(prop);
    ui_data->rna_subtype = prop_subtype.value();
  }
}

/* For now the maximum extent is that of a 4x4 matrix, so 16 values. */
#define ABC_MAX_POD_EXTENT 16

template<typename T>
static IDProperty *make_idprop_from_int_values(StringRef prop_name,
                                               const T *sample_values,
                                               size_t num_values,
                                               StringRef interpretation)
{
  BLI_assert(num_values <= ABC_MAX_POD_EXTENT);

  int int_values[ABC_MAX_POD_EXTENT];
  for (uint8_t i = 0; i < num_values; i++) {
    int_values[i] = int(sample_values[i]);
  }

  if (num_values == 1) {
    return bke::idprop::create(prop_name, int_values[0]).release();
  }

  if (num_values == 3 && interpretation == "rgb") {
    /* Convert to a color. We use the original sample values to avoid sign conversions. */
    float rgb[3] = {float(sample_values[0]) / 255.0f,
                    float(sample_values[1]) / 255.0f,
                    float(sample_values[2]) / 255.0f};
    IDProperty *result = bke::idprop::create(prop_name, Span(rgb, 3)).release();
    set_ui_data_from_interpretation(result, interpretation);
    return result;
  }

  if (num_values == 4 && interpretation == "rgba") {
    /* Convert to a color. We use the original sample values to avoid sign conversions. */
    float rgb[4] = {float(sample_values[0]) / 255.0f,
                    float(sample_values[1]) / 255.0f,
                    float(sample_values[2]) / 255.0f,
                    float(sample_values[3]) / 255.0f};
    IDProperty *result = bke::idprop::create(prop_name, Span(rgb, 4)).release();
    set_ui_data_from_interpretation(result, interpretation);
    return result;
  }

  return bke::idprop::create(prop_name, Span(int_values, num_values)).release();
}

template<typename T>
static IDProperty *make_idprop_from_floats(StringRef prop_name,
                                           const T *sample_values,
                                           size_t num_values,
                                           StringRef interpretation)
{
  if (num_values == 1) {
    return bke::idprop::create(prop_name, sample_values[0]).release();
  }

  IDProperty *result = bke::idprop::create(prop_name, Span(sample_values, num_values)).release();
  set_ui_data_from_interpretation(result, interpretation);
  return result;
}

static IDProperty *make_idprop_from_halves(StringRef prop_name,
                                           const Alembic::Abc::float16_t *sample_values,
                                           size_t num_values,
                                           StringRef interpretation)
{
  float values[ABC_MAX_POD_EXTENT];
  for (size_t i = 0; i < num_values; i++) {
    values[i] = sample_values[i];
  }

  return make_idprop_from_floats(prop_name, values, num_values, interpretation);
}

template<typename T>
static IDProperty *make_idprop_from_int_scalar_prop(StringRef prop_name,
                                                    IScalarProperty &prop,
                                                    const uint8_t extent,
                                                    StringRef interpretation)
{
  T values[ABC_MAX_POD_EXTENT];
  prop.get(values);
  return make_idprop_from_int_values(prop_name, values, extent, interpretation);
}

static IDProperty *abc_scalar_prop_to_idprop(ICompoundProperty compound_property,
                                             const PropertyHeader &prop_header)
{
  const std::string &prop_name = prop_header.getName();
  IScalarProperty prop = IScalarProperty(compound_property, prop_name);

  const DataType &data_type = prop_header.getDataType();
  const uint8_t extent = data_type.getExtent();

  BLI_assert_msg(extent <= ABC_MAX_POD_EXTENT,
                 "The maximum extent of an Alembic POD has been changed.");

  std::string interpretation = prop.getMetaData().get("interpretation");

  switch (data_type.getPod()) {
    case Alembic::AbcGeom::kBooleanPOD: {
      BLI_assert_msg(extent == 1, "Alembic seems to support arrays of bools now.");
      Alembic::Abc::bool_t value;
      prop.get(&value);
      return bke::idprop::create(prop_name, bool(value)).release();
    }
    case Alembic::AbcGeom::kUint8POD: {
      return make_idprop_from_int_scalar_prop<uint8_t>(prop_name, prop, extent, interpretation);
    }
    case Alembic::AbcGeom::kInt8POD: {
      return make_idprop_from_int_scalar_prop<int8_t>(prop_name, prop, extent, interpretation);
    }
    case Alembic::AbcGeom::kUint16POD: {
      return make_idprop_from_int_scalar_prop<uint16_t>(prop_name, prop, extent, interpretation);
    }
    case Alembic::AbcGeom::kInt16POD: {
      return make_idprop_from_int_scalar_prop<int16_t>(prop_name, prop, extent, interpretation);
    }
    case Alembic::AbcGeom::kUint32POD: {
      return make_idprop_from_int_scalar_prop<uint32_t>(prop_name, prop, extent, interpretation);
    }
    case Alembic::AbcGeom::kInt32POD: {
      return make_idprop_from_int_scalar_prop<int32_t>(prop_name, prop, extent, interpretation);
    }
    case Alembic::AbcGeom::kUint64POD: {
      return make_idprop_from_int_scalar_prop<uint64_t>(prop_name, prop, extent, interpretation);
    }
    case Alembic::AbcGeom::kInt64POD: {
      return make_idprop_from_int_scalar_prop<int64_t>(prop_name, prop, extent, interpretation);
    }
    case Alembic::AbcGeom::kFloat16POD: {
      Alembic::Abc::float16_t values[ABC_MAX_POD_EXTENT];
      prop.get(values);
      return make_idprop_from_halves(prop_name, values, extent, interpretation);
    }
    case Alembic::AbcGeom::kFloat32POD: {
      float values[ABC_MAX_POD_EXTENT];
      prop.get(values);
      return make_idprop_from_floats(prop_name, values, extent, interpretation);
    }
    case Alembic::AbcGeom::kFloat64POD: {
      double values[ABC_MAX_POD_EXTENT];
      prop.get(values);
      return make_idprop_from_floats(prop_name, values, extent, interpretation);
    }
    case Alembic::AbcGeom::kStringPOD: {
      BLI_assert_msg(extent == 1, "Alembic seems to support arrays of strings now.");
      std::string value;
      prop.get(&value);
      return bke::idprop::create(prop_name, value).release();
    }
    case Alembic::AbcGeom::kWstringPOD: {
      /* Unsupported at the moment, need examples. */
      break;
    }
    case Alembic::AbcGeom::kNumPlainOldDataTypes:
    case Alembic::AbcGeom::kUnknownPOD: {
      break;
    }
  }

  return nullptr;
}

template<typename T>
static IDProperty *make_idprop_from_int_array_prop(StringRef prop_name,
                                                   ArraySamplePtr sample,
                                                   const size_t num_values,
                                                   StringRef interpretation)
{
  const T *sample_values = static_cast<const T *>(sample->getData());
  return make_idprop_from_int_values(prop_name, sample_values, num_values, interpretation);
}

static IDProperty *abc_array_prop_to_idprop(ICompoundProperty compound_property,
                                            const PropertyHeader &prop_header)
{
  const std::string &prop_name = prop_header.getName();
  IArrayProperty prop = IArrayProperty(compound_property, prop_name);

  if (!prop.isScalarLike()) {
    return nullptr;
  }

  const DataType &data_type = prop_header.getDataType();
  const uint8_t extent = data_type.getExtent();

  /* Determine the number of values by multiplying the array dimensions by the POD's extent in case
   * the data is stored as a flat array instead of, e.g., a matrix or vec3. */
  Alembic::Abc::Dimensions dims;
  prop.getDimensions(dims);
  const size_t num_values = dims.numPoints() * extent;

  /* Some software export object level properties as array properties. We only load those
   * properties which would make sense to us. */
  if (num_values > ABC_MAX_POD_EXTENT) {
    return nullptr;
  }

  std::string interpretation = prop.getMetaData().get("interpretation");

  ArraySamplePtr sample;
  prop.get(sample);

  switch (data_type.getPod()) {
    case Alembic::AbcGeom::kBooleanPOD: {
      if (num_values == 1) {
        const Alembic::Abc::bool_t *sample_data = static_cast<const Alembic::Abc::bool_t *>(
            sample->getData());
        return bke::idprop::create(prop_name, bool(*sample_data)).release();
      }
      break;
    }
    case Alembic::AbcGeom::kUint8POD: {
      return make_idprop_from_int_array_prop<uint8_t>(
          prop_name, sample, num_values, interpretation);
    }
    case Alembic::AbcGeom::kInt8POD: {
      return make_idprop_from_int_array_prop<int8_t>(prop_name, sample, extent, interpretation);
    }
    case Alembic::AbcGeom::kUint16POD: {
      return make_idprop_from_int_array_prop<uint16_t>(prop_name, sample, extent, interpretation);
    }
    case Alembic::AbcGeom::kInt16POD: {
      return make_idprop_from_int_array_prop<int16_t>(prop_name, sample, extent, interpretation);
    }
    case Alembic::AbcGeom::kUint32POD: {
      return make_idprop_from_int_array_prop<uint32_t>(prop_name, sample, extent, interpretation);
    }
    case Alembic::AbcGeom::kInt32POD: {
      return make_idprop_from_int_array_prop<int32_t>(prop_name, sample, extent, interpretation);
    }
    case Alembic::AbcGeom::kUint64POD: {
      return make_idprop_from_int_array_prop<uint64_t>(prop_name, sample, extent, interpretation);
    }
    case Alembic::AbcGeom::kInt64POD: {
      return make_idprop_from_int_array_prop<int64_t>(prop_name, sample, extent, interpretation);
    }
    case Alembic::AbcGeom::kFloat16POD: {
      const Alembic::Abc::float16_t *sample_data = static_cast<const Alembic::Abc::float16_t *>(
          sample->getData());
      return make_idprop_from_halves(prop_name, sample_data, num_values, interpretation);
    }
    case Alembic::AbcGeom::kFloat32POD: {
      const float *sample_data = static_cast<const float *>(sample->getData());
      return make_idprop_from_floats(prop_name, sample_data, num_values, interpretation);
    }
    case Alembic::AbcGeom::kFloat64POD: {
      const double *sample_data = static_cast<const double *>(sample->getData());
      return make_idprop_from_floats(prop_name, sample_data, num_values, interpretation);
    }
    case Alembic::AbcGeom::kStringPOD: {
      if (num_values == 1) {
        const std::string *sample_data = static_cast<const std::string *>(sample->getData());
        return bke::idprop::create(prop_name, *sample_data).release();
      }
      break;
    }
    case Alembic::AbcGeom::kWstringPOD: {
      /* Unsupported at the moment, need examples. */
      break;
    }
    case Alembic::AbcGeom::kNumPlainOldDataTypes:
    case Alembic::AbcGeom::kUnknownPOD: {
      break;
    }
  }

  return nullptr;
}

void AbcObjectReader::readIDProperties(ICompoundProperty compound_property)
{
  if (!compound_property || !compound_property.valid()) {
    return;
  }

  ID *id = &m_object->id;
  /* Import the properties on the object's data, if any, since the compound property is set on the
   * object's data on the Alembic side.
   * TODO(kevindietrich) : import properties from the parent IXform on the object itself. This
   * needs to know if the parent is shared or not. */
  if (m_object->data) {
    id = m_object->data;
  }

  IDProperty *group = IDP_EnsureProperties(id);

  for (int i = 0; i < compound_property.getNumProperties(); i++) {
    auto prop_header = compound_property.getPropertyHeader(i);

    IDProperty *prop = nullptr;

    if (prop_header.isScalar()) {
      prop = abc_scalar_prop_to_idprop(compound_property, prop_header);
    }
    else if (prop_header.isArray()) {
      prop = abc_array_prop_to_idprop(compound_property, prop_header);
    }
    else {
      // TODO(kevindietrich) : recurse if CompoundProperty
      continue;
    }

    if (prop) {
      IDP_AddToGroup(group, prop);
    }
  }
}

}  // namespace io::alembic
}  // namespace blender
