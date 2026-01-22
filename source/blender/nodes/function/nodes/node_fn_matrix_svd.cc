/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_matrix.hh"
#include "BLI_math_solvers.h"

#include "NOD_inverse_eval_params.hh"
#include "NOD_value_elem_eval.hh"

#include "node_function_util.hh"

namespace blender::nodes::node_fn_matrix_svd_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Matrix>("Matrix");
  b.add_output<decl::Rotation>("U").description("Left hand rotation");
  b.add_output<decl::Vector>("S").description("Singular values");
  b.add_output<decl::Rotation>("V").description("Right hand rotation");
}

class MatrixSVDFunction : public mf::MultiFunction {
 public:
  MatrixSVDFunction()
  {
    static mf::Signature signature_;
    mf::SignatureBuilder builder{"Matrix SVD", signature_};
    builder.single_input<float4x4>("Matrix");
    builder.single_output<math::Quaternion>("U");
    builder.single_output<float3>("S");
    builder.single_output<math::Quaternion>("V");
    this->set_signature(&signature_);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArraySpan<float4x4> matrices = params.readonly_single_input<float4x4>(0, "Matrix");
    MutableSpan<math::Quaternion> Us = params.uninitialized_single_output<math::Quaternion>(1,
                                                                                            "U");
    MutableSpan<float3> Ss = params.uninitialized_single_output<float3>(2, "S");
    MutableSpan<math::Quaternion> Vs = params.uninitialized_single_output<math::Quaternion>(3,
                                                                                            "V");
    mask.foreach_index([&](const int64_t i) {
      const float3x3 matrix = matrices[i].view<3, 3>();
      float3x3 matrix_U, matrix_V;
      BLI_svd_m3(matrix.ptr(), matrix_U.ptr(), Ss[i], matrix_V.ptr());
      Us[i] = math::to_quaternion(matrix_U);
      Vs[i] = math::to_quaternion(matrix_V);
    });
  }
};

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static MatrixSVDFunction fn;
  builder.set_matching_fn(fn);
}

static void node_eval_elem(value_elem::ElemEvalParams &params)
{
  using namespace value_elem;
  const MatrixElem matrix_elem = params.get_input_elem<MatrixElem>("Matrix");
  params.set_output_elem("U", matrix_elem.rotation);
  params.set_output_elem("S", matrix_elem.rotation);
  params.set_output_elem("V", matrix_elem.rotation);
}

static void node_eval_inverse_elem(value_elem::InverseElemEvalParams &params)
{
  using namespace value_elem;
  const RotationElem U_elem = params.get_output_elem<RotationElem>("U");
  const VectorElem S_elem = params.get_output_elem<VectorElem>("S");
  const RotationElem V_elem = params.get_output_elem<RotationElem>("V");

  RotationElem rotation_elem = U_elem;
  rotation_elem.merge(V_elem);

  const MatrixElem matrix_elem = {{}, rotation_elem, S_elem};
  params.set_input_elem("Matrix", matrix_elem);
}

static void node_eval_inverse(inverse_eval::InverseEvalParams &params)
{
  const math::Quaternion U = params.get_output<math::Quaternion>("U");
  const float3 S = params.get_output<float3>("S");
  const math::Quaternion V = params.get_output<math::Quaternion>("V");
  const math::Quaternion V_inv = math::Quaternion(V.w, -V.imaginary_part());
  const float3x3 matrix3 = math::from_rotation<float3x3>(U) * math::from_scale<float3x3>(S) *
                           math::from_rotation<float3x3>(V_inv);
  params.set_input("Matrix", float4x4(matrix3));
}

static void node_register()
{
  static bke::bNodeType ntype;
  fn_node_type_base(&ntype, "FunctionNodeMatrixSVD");
  ntype.ui_name = "Matrix SVD";
  ntype.ui_description = "Compute the singular value decomposition of the 3x3 part of a matrix";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  ntype.eval_elem = node_eval_elem;
  ntype.eval_inverse_elem = node_eval_inverse_elem;
  ntype.eval_inverse = node_eval_inverse;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_matrix_svd_cc
