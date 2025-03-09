/* SPDX-FileCopyrightText: 2011-2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_OPENSUBDIV

#  include "subd/patch.h"

#  include "util/unique_ptr.h"
#  include "util/vector.h"

#  include <opensubdiv/far/patchMap.h>
#  include <opensubdiv/far/patchTableFactory.h>
#  include <opensubdiv/far/primvarRefiner.h>
#  include <opensubdiv/far/topologyRefinerFactory.h>

// Directly use some OpenSubdiv namespaces for brevity
namespace Far = OpenSubdiv::Far;
namespace Sdc = OpenSubdiv::Sdc;

CCL_NAMESPACE_BEGIN

class Attribute;
class Mesh;

/* OpenSubdiv interface for vertex and attribute values. */

template<typename T> struct OsdValue {
  T value;

  OsdValue() = default;

  void Clear(void *unused = nullptr)
  {
    (void)unused;
    memset((void *)&value, 0, sizeof(T));
  }

  void AddWithWeight(OsdValue<T> const &src, float weight)
  {
    value += src.value * weight;
  }
};

/* Wrapper around Mesh for TopologyRefinerFactory. */

class OsdMesh {
 public:
  /* Types */
  struct MergedFVar {
    const Attribute &attr;
    int channel = -1;
    vector<char> values;
  };

  /* Members */
  Mesh &mesh;
  vector<MergedFVar> merged_fvars;

  /* Functions */
  OsdMesh(Mesh &mesh) : mesh(mesh) {}

  Sdc::Options sdc_options();
  bool use_smooth_fvar(const Attribute &attr) const;
  bool use_smooth_fvar() const;
};

/* OpenSubdiv refiner and patch data structures. */

struct OsdData {
  unique_ptr<Far::TopologyRefiner> refiner;
  unique_ptr<Far::PatchTable> patch_table;
  unique_ptr<Far::PatchMap> patch_map;
  vector<OsdValue<float3>> refined_verts;

  void build(OsdMesh &osd_mesh);
  void subdivide_attribute(Attribute &attr);
};

/* Patch with OpenSubdiv evaluation. */

struct OsdPatch final : Patch {
  OsdData &osd_data;

  OsdPatch(OsdData &data) : osd_data(data) {}
  void eval(float3 *P, float3 *dPdu, float3 *dPdv, float3 *N, const float u, const float v)
      const override;
};

CCL_NAMESPACE_END

#endif
