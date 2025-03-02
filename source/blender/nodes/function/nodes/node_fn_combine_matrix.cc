/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_inverse_eval_params.hh"
#include "NOD_value_elem_eval.hh"

#include "node_function_util.hh"

namespace blender::nodes::node_fn_combine_matrix_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.use_custom_socket_order();

  b.add_output<decl::Matrix>("Matrix");

  PanelDeclarationBuilder &column_a = b.add_panel("Column 1").default_closed(true);
  column_a.add_input<decl::Float>("Column 1 Row 1").default_value(1.0f);
  column_a.add_input<decl::Float>("Column 1 Row 2");
  column_a.add_input<decl::Float>("Column 1 Row 3");
  column_a.add_input<decl::Float>("Column 1 Row 4");

  PanelDeclarationBuilder &column_b = b.add_panel("Column 2").default_closed(true);
  column_b.add_input<decl::Float>("Column 2 Row 1");
  column_b.add_input<decl::Float>("Column 2 Row 2").default_value(1.0f);
  column_b.add_input<decl::Float>("Column 2 Row 3");
  column_b.add_input<decl::Float>("Column 2 Row 4");

  PanelDeclarationBuilder &column_c = b.add_panel("Column 3").default_closed(true);
  column_c.add_input<decl::Float>("Column 3 Row 1");
  column_c.add_input<decl::Float>("Column 3 Row 2");
  column_c.add_input<decl::Float>("Column 3 Row 3").default_value(1.0f);
  column_c.add_input<decl::Float>("Column 3 Row 4");

  PanelDeclarationBuilder &column_d = b.add_panel("Column 4").default_closed(true);
  column_d.add_input<decl::Float>("Column 4 Row 1");
  column_d.add_input<decl::Float>("Column 4 Row 2");
  column_d.add_input<decl::Float>("Column 4 Row 3");
  column_d.add_input<decl::Float>("Column 4 Row 4").default_value(1.0f);
}

static void copy_with_stride(const IndexMask &mask,
                             const VArray<float> &src,
                             const int64_t src_step,
                             const int64_t src_begin,
                             const int64_t dst_step,
                             const int64_t dst_begin,
                             MutableSpan<float> dst)
{
  BLI_assert(src_begin < src_step);
  BLI_assert(dst_begin < dst_step);
  devirtualize_varray(src, [&](const auto src) {
    mask.foreach_index_optimized<int>([&](const int64_t index) {
      dst[dst_begin + dst_step * index] = src[src_begin + src_step * index];
    });
  });
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  using namespace blender::fn::multi_function;
  constexpr auto param_tags = TypeSequence<float,
                                           float,
                                           float,
                                           float,
                                           float,
                                           float,
                                           float,
                                           float,
                                           float,
                                           float,
                                           float,
                                           float,
                                           float,
                                           float,
                                           float,
                                           float>();
  static auto element_fn = [](float M11,
                              float M21,
                              float M31,
                              float M41,
                              float M12,
                              float M22,
                              float M32,
                              float M42,
                              float M13,
                              float M23,
                              float M33,
                              float M43,
                              float M14,
                              float M24,
                              float M34,
                              float M44) {
    float4x4 mat;
    mat[0] = float4(M11, M21, M31, M41);
    mat[1] = float4(M12, M22, M32, M42);
    mat[2] = float4(M13, M23, M33, M43);
    mat[3] = float4(M14, M24, M34, M44);
    return mat;
  };
  static auto call_fn = build::detail::build_multi_function_with_n_inputs_one_output<
                 float4x4>(
      "Combine Matrix", element_fn, mf::build::exec_presets::Materialized(), param_tags);
  builder.set_matching_fn(call_fn);
}

static void node_eval_elem(value_elem::ElemEvalParams &params)
{
  using namespace value_elem;

  std::array<std::array<FloatElem, 4>, 4> input_elems;
  for (const int col : IndexRange(4)) {
    for (const int row : IndexRange(4)) {
      const bNodeSocket &socket = params.node.input_socket(col * 4 + row);
      input_elems[col][row] = params.get_input_elem<FloatElem>(socket.identifier);
    }
  }

  MatrixElem matrix_elem;
  matrix_elem.translation.x = input_elems[3][0];
  matrix_elem.translation.y = input_elems[3][1];
  matrix_elem.translation.z = input_elems[3][2];

  bool any_inner_3x3 = false;
  for (const int col : IndexRange(3)) {
    for (const int row : IndexRange(3)) {
      any_inner_3x3 |= input_elems[col][row];
    }
  }
  if (any_inner_3x3) {
    matrix_elem.rotation = RotationElem::all();
    matrix_elem.scale = VectorElem::all();
  }

  const bool any_non_transform = input_elems[0][3] || input_elems[1][3] || input_elems[2][3] ||
                                 input_elems[3][3];
  if (any_non_transform) {
    matrix_elem.any_non_transform = FloatElem::all();
  }

  params.set_output_elem("Matrix", matrix_elem);
}

static void node_eval_inverse_elem(value_elem::InverseElemEvalParams &params)
{
  using namespace value_elem;

  const MatrixElem matrix_elem = params.get_output_elem<MatrixElem>("Matrix");
  std::array<std::array<FloatElem, 4>, 4> input_elems;

  input_elems[3][0] = matrix_elem.translation.x;
  input_elems[3][1] = matrix_elem.translation.y;
  input_elems[3][2] = matrix_elem.translation.z;

  if (matrix_elem.rotation || matrix_elem.scale) {
    for (const int col : IndexRange(3)) {
      for (const int row : IndexRange(3)) {
        input_elems[col][row] = FloatElem::all();
      }
    }
  }

  if (matrix_elem.any_non_transform) {
    for (const int col : IndexRange(4)) {
      input_elems[col][3] = FloatElem::all();
    }
  }

  for (const int col : IndexRange(4)) {
    for (const int row : IndexRange(4)) {
      const bNodeSocket &socket = params.node.input_socket(col * 4 + row);
      params.set_input_elem(socket.identifier, input_elems[col][row]);
    }
  }
}

static void node_eval_inverse(inverse_eval::InverseEvalParams &params)
{
  const float4x4 matrix = params.get_output<float4x4>("Matrix");
  for (const int col : IndexRange(4)) {
    for (const int row : IndexRange(4)) {
      const bNodeSocket &socket = params.node.input_socket(col * 4 + row);
      params.set_input(socket.identifier, matrix[col][row]);
    }
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  fn_node_type_base(&ntype, "FunctionNodeCombineMatrix", FN_NODE_COMBINE_MATRIX);
  ntype.ui_name = "Combine Matrix";
  ntype.ui_description = "Construct a 4x4 matrix from its individual values";
  ntype.enum_name_legacy = "COMBINE_MATRIX";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  ntype.eval_elem = node_eval_elem;
  ntype.eval_inverse_elem = node_eval_inverse_elem;
  ntype.eval_inverse = node_eval_inverse;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_combine_matrix_cc
