/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>
#include <sstream>

#include <fmt/format.h>

#include <fast_float.h>

#include "BLI_listbase.hh"
#include "BLI_resource_scope.hh"
#include "BLI_string.hh"
#include "BLT_translation.hh"

#include "NOD_expression_parse.hh"
#include "NOD_expression_to_nodes.hh"
#include "NOD_fn_format_string.hh"
#include "NOD_socket.hh"
#include "NOD_socket_items.hh"

#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_dot_export.hh"

#include "DNA_node_types.h"

namespace blender::nodes::expression {

class AstToNodeGroupBuilder;

struct NodeAndSocket {
  bNode *node = nullptr;
  bNodeSocket *socket = nullptr;

  operator bool() const
  {
    return this->socket != nullptr;
  }
};

/** A value type does not necessarily have to correspond to a single socket. */
enum class ValueType {
  Float,
  Vec3,
  String,
  Rgb,
  Rgba,
  Boolean,
  Integer,
};

static std::optional<ValueType> socket_to_value_type(const bke::bNodeTreeType &tree_type,
                                                     const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_FLOAT:
      return ValueType::Float;
    case SOCK_VECTOR:
      /* TODO: Use dimensions? */
      return ValueType::Vec3;
    case SOCK_STRING:
      return ValueType::String;
    case SOCK_RGBA:
      return tree_type.type == NTREE_SHADER ? ValueType::Rgb : ValueType::Rgba;
    case SOCK_BOOLEAN:
      return ValueType::Boolean;
    case SOCK_INT:
      return ValueType::Integer;
    default:
      return std::nullopt;
  }
}

static std::optional<eNodeSocketDatatype> value_to_closest_socket_type(const ValueType type)
{
  switch (type) {
    case ValueType::Float:
      return SOCK_FLOAT;
    case ValueType::Vec3:
      return SOCK_VECTOR;
    case ValueType::String:
      return SOCK_STRING;
    case ValueType::Rgb:
      return SOCK_RGBA;
    case ValueType::Rgba:
      return SOCK_RGBA;
    case ValueType::Boolean:
      return SOCK_BOOLEAN;
    case ValueType::Integer:
      return SOCK_INT;
  }
  return std::nullopt;
}

class Value {
 public:
  ValueType type;
  Vector<NodeAndSocket, 1> sockets;

  Value(ValueType type, bNode &node, bNodeSocket &socket) : type(type), sockets({{&node, &socket}})
  {
  }

  Value(ValueType type, Vector<NodeAndSocket> sockets) : type(type), sockets(std::move(sockets)) {}
};

struct TypeCheckCallParams {
  const bke::bNodeTreeType &tree_type;
  Vector<const bke::bNodeSocketType *> input_types;
};

static bNodeSocket *find_available_socket_by_index(ListBaseT<bNodeSocket> &sockets,
                                                   const int index)
{
  int remaining = index;
  for (bNodeSocket &socket : sockets) {
    if (!socket.is_available()) {
      continue;
    }
    if (remaining == 0) {
      return &socket;
    }
    remaining--;
  }
  return nullptr;
}

struct InsertCallParams {
  AstToNodeGroupBuilder &builder;
  const bNodeTree &tree;

  Vector<Value> inputs;
  std::optional<Value> output;

  bNode &add_node(const UString idname);
  void update_node_sockets(bNode &node);

  void add_input(Value value)
  {
    this->inputs.append(std::move(value));
  }

  void add_input(bNode &node, bNodeSocket &socket)
  {
    this->add_input(Value(*socket_to_value_type(*tree.typeinfo, socket), node, socket));
  }

  void add_input(bNode &node, const int index)
  {
    bNodeSocket *socket = find_available_socket_by_index(node.inputs, index);
    BLI_assert(socket);
    this->add_input(node, *socket);
  }

  void set_output(Value value)
  {
    /* Should only be set once. */
    BLI_assert(!this->output.has_value());
    this->output = std::move(value);
  }

  void set_output(bNode &node, bNodeSocket &socket)
  {
    this->set_output(Value(*socket_to_value_type(*tree.typeinfo, socket), node, socket));
  }

  void set_output(bNode &node, const int index)
  {
    bNodeSocket *socket = find_available_socket_by_index(node.outputs, index);
    BLI_assert(socket);
    this->set_output(node, *socket);
  }

  void use_node_sockets(bNode &node)
  {
    this->use_node_inputs(node);
    this->use_node_output(node);
  }

  void use_node_inputs(bNode &node)
  {
    for (bNodeSocket &socket : node.inputs) {
      if (socket.is_available()) {
        this->add_input(node, socket);
      }
    }
  }

  void use_node_output(bNode &node)
  {
    for (bNodeSocket &socket : node.outputs) {
      if (socket.is_available()) {
        this->set_output(node, socket);
      }
    }
  }
};

using InsertCallFn = std::function<void(InsertCallParams &params)>;
using TypeCheckCallFn = std::function<bool(TypeCheckCallParams &params)>;

struct FunctionSymbolParam {
  ValueType type;
  bool needs_exact = false;
};

class FunctionSymbol {
 public:
  std::string name;
  Vector<FunctionSymbolParam> params;
  std::optional<Vector<const bke::bNodeTreeType *>> allowed_tree_types;
  InsertCallFn insert;

  FunctionSymbol(std::string name,
                 Vector<FunctionSymbolParam> params,
                 InsertCallFn insert,
                 std::optional<Vector<const bke::bNodeTreeType *>> allowed_tree_types = {})
      : name(std::move(name)),
        params(std::move(params)),
        allowed_tree_types(allowed_tree_types),
        insert(std::move(insert))
  {
  }
};

class SymbolTable {
 private:
  MultiValueMap<std::string, FunctionSymbol> function_;

  enum class MatchingType {
    None = 0,
    WithImplicitConversions = 1,
    Exact = 2,
  };

 public:
  void add(FunctionSymbol function_symbol)
  {
    function_.add(function_symbol.name, std::move(function_symbol));
  }

  const FunctionSymbol *lookup_function(const StringRef name,
                                        const bke::bNodeTreeType &tree_type,
                                        const Span<ValueType> input_types,
                                        std::string &r_error) const
  {
    const Span<FunctionSymbol> candidates = function_.lookup(name);
    if (candidates.is_empty()) {
      r_error = fmt::format("{}: '{}'", TIP_("Unknown function"), name);
      return nullptr;
    }
    MatchingType best_matching_type = MatchingType::None;
    Vector<const FunctionSymbol *> best_matching_functions;
    for (const FunctionSymbol &function : candidates) {
      const MatchingType matching_type = this->compute_matching_type(
          function, tree_type, input_types);
      if (matching_type == MatchingType::None) {
        continue;
      }
      if (matching_type == best_matching_type) {
        best_matching_functions.append(&function);
      }
      else if (matching_type > best_matching_type) {
        best_matching_type = matching_type;
        best_matching_functions.clear();
        best_matching_functions.append(&function);
      }
    }

    if (best_matching_functions.is_empty()) {
      r_error = fmt::format("{}: '{}'", TIP_("No matching function"), name);
      return nullptr;
    }
    if (best_matching_functions.size() >= 2) {
      r_error = fmt::format("{}: '{}'", TIP_("Ambiguous function call"), name);
      return nullptr;
    }
    const FunctionSymbol *selected_function = best_matching_functions.first();
    return selected_function;
  }

  MatchingType compute_matching_type(const FunctionSymbol &function,
                                     const bke::bNodeTreeType &tree_type,
                                     const Span<ValueType> input_types) const
  {
    if (function.params.size() != input_types.size()) {
      return MatchingType::None;
    }
    bool all_exact = true;
    for (const int i : IndexRange(input_types.size())) {
      const ValueType input_type = input_types[i];
      const FunctionSymbolParam &param = function.params[i];
      if (input_type == param.type) {
        continue;
      }
      if (param.needs_exact) {
        return MatchingType::None;
        continue;
      }
      const std::optional<eNodeSocketDatatype> from_socket_type = value_to_closest_socket_type(
          input_type);
      const std::optional<eNodeSocketDatatype> to_socket_type = value_to_closest_socket_type(
          param.type);
      if (!from_socket_type || !to_socket_type) {
        return MatchingType::None;
      }
      if (!tree_type.validate_link(*from_socket_type, *to_socket_type)) {
        return MatchingType::None;
      }
      all_exact = false;
    }
    if (all_exact) {
      return MatchingType::Exact;
    }
    return MatchingType::WithImplicitConversions;
  }
};

struct BuildOptions {};

class AstToNodeGroupBuilder {
 private:
  const NodeExpression &bnode_storage_;
  const Span<ast::Expr *> root_exprs_;
  const Span<int> expr_indices_;
  const SymbolTable &symbol_table_;
  const BuildOptions &options_;

  bNodeTree &r_tree_;
  std::string &r_error_;

  Map<StringRef, Value> inputs_;

  friend InsertCallParams;

 public:
  AstToNodeGroupBuilder(const bNode &expr_bnode,
                        const Span<ast::Expr *> root_exprs,
                        const Span<int> expr_indices,
                        const SymbolTable &symbol_table,
                        const BuildOptions &options,
                        bNodeTree &r_tree,
                        std::string &r_error)
      : bnode_storage_(*static_cast<const NodeExpression *>(expr_bnode.storage)),
        root_exprs_(root_exprs),
        expr_indices_(expr_indices),
        symbol_table_(symbol_table),
        options_(options),
        r_tree_(r_tree),
        r_error_(r_error)
  {
  }

  void build()
  {
    this->add_interface_inputs();
    this->add_interface_outputs();

    bNode &group_input_node = this->add_node("NodeGroupInput"_ustr);
    bNode &group_output_node = this->add_node("NodeGroupOutput"_ustr);

    {
      bNodeSocket *group_input_socket = static_cast<bNodeSocket *>(group_input_node.outputs.first);
      for ([[maybe_unused]] const int i : IndexRange(bnode_storage_.input_items.items_num)) {
        const NodeExpressionInputItem &item = bnode_storage_.input_items.items[i];
        const std::optional<ValueType> input_type = socket_to_value_type(*r_tree_.typeinfo,
                                                                         *group_input_socket);
        if (!input_type) {
          r_error_ = TIP_("Unsupported socket_type for input");
          return;
        }
        inputs_.add(item.name, Value(*input_type, group_input_node, *group_input_socket));
        group_input_socket = group_input_socket->next;
      }
    }

    {
      bNodeSocket *group_output_socket = static_cast<bNodeSocket *>(
          group_output_node.inputs.first);
      for (const int i : expr_indices_.index_range()) {
        std::optional<Value> expr_result = this->build_expr(*root_exprs_[i]);
        if (!expr_result) {
          return;
        }
        Value output_value(*socket_to_value_type(*r_tree_.typeinfo, *group_output_socket),
                           group_output_node,
                           *group_output_socket);
        this->link_values(*expr_result, output_value);
        group_output_socket = group_output_socket->next;
      }
    }

    BKE_ntree_update_without_main(r_tree_);
  }

 private:
  void add_interface_inputs()
  {
    for (const int i : IndexRange(bnode_storage_.input_items.items_num)) {
      const NodeExpressionInputItem &item = bnode_storage_.input_items.items[i];
      const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.socket_type);
      r_tree_.tree_interface.add_socket(
          item.name, "", stype->idname.ref(), NODE_INTERFACE_SOCKET_INPUT, nullptr);
    }
  }

  void add_interface_outputs()
  {
    for (const int i : expr_indices_.index_range()) {
      const NodeExpressionItem &expr_item =
          bnode_storage_.expression_items.items[expr_indices_[i]];
      const bke::bNodeSocketType *output_stype = bke::node_socket_type_find_static(
          expr_item.socket_type);
      r_tree_.tree_interface.add_socket(
          expr_item.name, "", output_stype->idname.ref(), NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
    }
  }

  std::optional<Value> build_expr(const ast::Expr &expr)
  {
    return std::visit([&](const auto &ast_node) { return this->build_expr(ast_node); }, expr.expr);
  }

  std::optional<Value> build_expr(const ast::NumberLiteral &ast_node)
  {
    /* TODO: Handle floats vs. integers. */
    float value;
    fast_float::from_chars_result result = fast_float::from_chars(
        ast_node.value.begin(), ast_node.value.end(), value);
    if (result.ec != std::errc()) {
      r_error_ = fmt::format("{}: {}", TIP_("Invalid number"), ast_node.value);
      return {};
    }
    bNode &node = this->add_node("ShaderNodeValue"_ustr);
    bNodeSocket *socket = static_cast<bNodeSocket *>(node.outputs.first);
    socket->default_value_typed<bNodeSocketValueFloat>()->value = value;
    return Value(ValueType::Float, node, *socket);
  }

  std::optional<Value> build_expr(const ast::StringLiteral &ast_node)
  {
    bNode &node = this->add_node("FunctionNodeInputString"_ustr);
    auto &storage = *static_cast<NodeInputString *>(node.storage);
    const StringRef str = ast_node.value.drop_known_prefix("\"").drop_known_suffix("\"");
    storage.string = BLI_strdupn(str.data(), str.size());
    return Value(ValueType::String, node, *static_cast<bNodeSocket *>(node.outputs.first));
  }

  std::optional<Value> build_expr(const ast::Identifier &ast_node)
  {
    std::optional<Value> input = inputs_.lookup_try(ast_node.identifier);
    if (!input) {
      r_error_ = fmt::format("{}: {}", TIP_("Unknown variable"), ast_node.identifier);
      return {};
    }
    return input;
  }

  std::optional<Value> build_expr(const ast::BinaryOp &ast_node)
  {
    return this->build_generic_call(ast_node.op, {ast_node.a, ast_node.b});
  }

  std::optional<Value> build_expr(const ast::UnaryOp &ast_node)
  {
    return this->build_generic_call(ast_node.op, {ast_node.expr});
  }

  std::optional<Value> build_expr(const ast::ConditionalOp &ast_node)
  {
    return this->build_generic_call("?:",
                                    {ast_node.condition, ast_node.true_expr, ast_node.false_expr});
  }

  std::optional<Value> build_expr(const ast::MemberAccess &ast_node)
  {
    return this->build_generic_call("." + ast_node.identifier, {ast_node.expr});
  }

  std::optional<Value> build_expr(const ast::Call &ast_node)
  {
    if (const ast::Identifier *identifier = std::get_if<ast::Identifier>(&ast_node.function->expr))
    {
      return this->build_generic_call(identifier->identifier, ast_node.args);
    }
    r_error_ = TIP_("Unexpected function call");
    return {};
  }

  std::optional<Value> build_generic_call(const StringRef name, const Span<const ast::Expr *> args)
  {
    Vector<Value> arg_values;
    Vector<ValueType> arg_types(args.size());
    for (const int i : args.index_range()) {
      std::optional<Value> arg_value = this->build_expr(*args[i]);
      if (!arg_value) {
        /* There is an error in the argument. */
        return {};
      }
      arg_types[i] = arg_value->type;
      arg_values.append(std::move(*arg_value));
    }

    const FunctionSymbol *function = symbol_table_.lookup_function(
        name, *r_tree_.typeinfo, arg_types, r_error_);
    if (!function) {
      BLI_assert(!r_error_.empty());
      return {};
    }
    InsertCallParams insert_params{*this, r_tree_};
    function->insert(insert_params);
    BLI_assert(insert_params.inputs.size() == args.size());
    BLI_assert(insert_params.output.has_value());

    for (const int i : args.index_range()) {
      this->link_values(arg_values[i], insert_params.inputs[i]);
    }
    return insert_params.output;
  }

  bNode &add_node(const UString idname)
  {
    return *bke::node_add_node(nullptr, r_tree_, idname);
  }

  void link_values(const Value &from, const Value &to)
  {
    BLI_assert(from.sockets.size() == to.sockets.size());
    for (const int i : from.sockets.index_range()) {
      const NodeAndSocket &from_socket = from.sockets[i];
      const NodeAndSocket &to_socket = to.sockets[i];
      bke::node_add_link(
          r_tree_, *from_socket.node, *from_socket.socket, *to_socket.node, *to_socket.socket);
    }
  }
};

static FunctionSymbol float_math_function(const StringRef name,
                                          const NodeMathOperation op,
                                          const int inputs_num)
{
  Vector<FunctionSymbolParam> param_types(inputs_num, {ValueType::Float});
  return FunctionSymbol(name, std::move(param_types), [op](InsertCallParams &params) {
    bNode &math_node = params.add_node("ShaderNodeMath"_ustr);
    math_node.custom1 = op;
    params.update_node_sockets(math_node);
    params.use_node_sockets(math_node);
  });
}

static FunctionSymbol vector_math_function(const StringRef name,
                                           const NodeVectorMathOperation op,
                                           const int inputs_num)
{
  Vector<FunctionSymbolParam> param_types(inputs_num, {ValueType::Vec3});
  return FunctionSymbol(name,
                        std::move(param_types),
                        [op](InsertCallParams &params) {
                          bNode &math_node = params.add_node("ShaderNodeVectorMath"_ustr);
                          math_node.custom1 = op;
                          params.update_node_sockets(math_node);
                          params.use_node_sockets(math_node);
                        }

  );
}

static FunctionSymbol negate_float_function()
{
  return FunctionSymbol("-", {{ValueType::Float}}, [](InsertCallParams &params) {
    bNode &math_node = params.add_node("ShaderNodeMath"_ustr);
    math_node.custom1 = NODE_MATH_SUBTRACT;
    params.update_node_sockets(math_node);
    static_cast<bNodeSocket *>(math_node.inputs.first)
        ->default_value_typed<bNodeSocketValueFloat>()
        ->value = 0.0f;
    params.add_input(math_node, 1);
    params.use_node_output(math_node);
  });
}

static FunctionSymbol vector_member_access(const ValueType type, const int index)
{
  BLI_assert(index >= 0 && index <= 2);
  return FunctionSymbol(
      fmt::format(".{}", char('x' + index)), {{type, true}}, [index](InsertCallParams &params) {
        bNode &node = params.add_node("ShaderNodeSeparateXYZ"_ustr);
        params.use_node_inputs(node);
        params.set_output(node, index);
      });
}

static FunctionSymbol string_concatenation()
{
  return FunctionSymbol(
      "+", {{ValueType::String}, {ValueType::String}}, [](InsertCallParams &params) {
        bNode &node = params.add_node("FunctionNodeFormatString"_ustr);
        auto &storage = *static_cast<NodeFunctionFormatString *>(node.storage);
        storage.items = MEM_new_array<NodeFunctionFormatStringItem>(2, "string_concatenation");
        NodeFunctionFormatStringItem &item0 = storage.items[0];
        NodeFunctionFormatStringItem &item1 = storage.items[1];
        item0.identifier = storage.next_identifier++;
        item1.identifier = storage.next_identifier++;
        item0.name = BLI_strdup("a");
        item1.name = BLI_strdup("b");
        item0.socket_type = SOCK_STRING;
        item1.socket_type = SOCK_STRING;
        storage.items_num = 2;
        params.update_node_sockets(node);
        STRNCPY(static_cast<bNodeSocket *>(node.inputs.first)
                    ->default_value_typed<bNodeSocketValueString>()
                    ->value,
                "{a}{b}");
        params.add_input(node, 1);
        params.add_input(node, 2);
        params.use_node_output(node);
      });
}

static FunctionSymbol ternary_conditional_operator(const ValueType type)
{
  return FunctionSymbol(
      "?:", {{ValueType::Boolean}, {type}, {type}}, [type](InsertCallParams &params) {
        const eNodeSocketDatatype socket_type = *value_to_closest_socket_type(type);
        if (params.tree.type == NTREE_GEOMETRY) {
          bNode &node = params.add_node("GeometryNodeSwitch"_ustr);
          auto &storage = *static_cast<NodeSwitch *>(node.storage);
          storage.input_type = socket_type;
          params.update_node_sockets(node);
          params.add_input(node, 0);
          params.add_input(node, 2);
          params.add_input(node, 1);
          params.use_node_output(node);
          return;
        }
        bNode &node = params.add_node("ShaderNodeMix"_ustr);
        NodeShaderMix &storage = *static_cast<NodeShaderMix *>(node.storage);
        storage.clamp_factor = false;
        if (ELEM(socket_type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN)) {
          storage.data_type = socket_type;
        }
        else if (socket_type == SOCK_VECTOR) {
          storage.data_type = SOCK_VECTOR;
        }
        else {
          storage.data_type = SOCK_RGBA;
        }
        params.update_node_sockets(node);
        params.add_input(node, 0);
        params.add_input(node, 1);
        params.add_input(node, 2);
        params.use_node_output(node);
      });
}

static FunctionSymbol create_vec3_function()
{
  return FunctionSymbol("vec3",
                        {{ValueType::Float}, {ValueType::Float}, {ValueType::Float}},
                        [](InsertCallParams &params) {
                          bNode &node = params.add_node("ShaderNodeCombineXYZ"_ustr);
                          params.use_node_sockets(node);
                        });
}

static UString get_combine_color_node_idname(const int tree_type)
{
  switch (tree_type) {
    case NTREE_GEOMETRY:
      return "FunctionNodeCombineColor"_ustr;
    case NTREE_COMPOSIT:
      return "CompositorNodeCombineColor"_ustr;
    case NTREE_SHADER:
      return "ShaderNodeCombineColor"_ustr;
  }
  BLI_assert_unreachable();
  return {};
}

static FunctionSymbol create_rgb_function()
{
  return FunctionSymbol("rgb",
                        {{ValueType::Float}, {ValueType::Float}, {ValueType::Float}},
                        [](InsertCallParams &params) {
                          const UString idname = get_combine_color_node_idname(params.tree.type);
                          bNode &node = params.add_node(idname);
                          params.add_input(node, 0);
                          params.add_input(node, 1);
                          params.add_input(node, 2);
                          params.use_node_output(node);
                        });
}

static FunctionSymbol create_rgba_function()
{
  return FunctionSymbol(
      "rgba",
      {{ValueType::Float}, {ValueType::Float}, {ValueType::Float}, {ValueType::Float}},
      [](InsertCallParams &params) {
        const UString idname = get_combine_color_node_idname(params.tree.type);
        bNode &node = params.add_node(idname);
        params.use_node_sockets(node);
      });
}

static UString get_separate_color_node_idname(const int tree_type)
{
  switch (tree_type) {
    case NTREE_GEOMETRY:
      return "FunctionNodeSeparateColor"_ustr;
    case NTREE_COMPOSIT:
      return "CompositorNodeSeparateColor"_ustr;
    case NTREE_SHADER:
      return "ShaderNodeSeparateColor"_ustr;
  }
  BLI_assert_unreachable();
  return {};
}

static FunctionSymbol create_color_member_access(const ValueType type, const int index)
{
  BLI_assert(index >= 0 && index < 4);
  return FunctionSymbol(
      fmt::format(".{}", char("rgba"[index])), {{type, true}}, [index](InsertCallParams &params) {
        const UString node_idname = get_separate_color_node_idname(params.tree.type);
        bNode &node = params.add_node(node_idname);
        params.use_node_inputs(node);
        params.set_output(node, index);
      });
}

static void init_symbol_table(SymbolTable &symbols)
{
  symbols.add(float_math_function("+", NODE_MATH_ADD, 2));
  symbols.add(float_math_function("-", NODE_MATH_SUBTRACT, 2));
  symbols.add(float_math_function("*", NODE_MATH_MULTIPLY, 2));
  symbols.add(float_math_function("/", NODE_MATH_DIVIDE, 2));
  symbols.add(float_math_function("sin", NODE_MATH_SINE, 1));
  symbols.add(float_math_function("cos", NODE_MATH_COSINE, 1));
  symbols.add(negate_float_function());

  symbols.add(vector_math_function("+", NODE_VECTOR_MATH_ADD, 2));
  symbols.add(vector_math_function("-", NODE_VECTOR_MATH_SUBTRACT, 2));
  symbols.add(vector_math_function("*", NODE_VECTOR_MATH_MULTIPLY, 2));
  symbols.add(vector_math_function("/", NODE_VECTOR_MATH_DIVIDE, 2));

  symbols.add(create_vec3_function());
  symbols.add(create_rgb_function());
  symbols.add(create_rgba_function());

  for (const ValueType type : {ValueType::Vec3, ValueType::Rgb, ValueType::Rgba}) {
    symbols.add(vector_member_access(type, 0));
    symbols.add(vector_member_access(type, 1));
    symbols.add(vector_member_access(type, 2));

    symbols.add(create_color_member_access(type, 0));
    symbols.add(create_color_member_access(type, 1));
    symbols.add(create_color_member_access(type, 2));
  }
  symbols.add(create_color_member_access(ValueType::Rgba, 3));

  symbols.add(string_concatenation());

  for (const ValueType type : {ValueType::Float,
                               ValueType::Vec3,
                               ValueType::String,
                               ValueType::Rgb,
                               ValueType::Rgba,
                               ValueType::Boolean,
                               ValueType::Integer})
  {
    symbols.add(ternary_conditional_operator(type));
  }
}

static SymbolTable &get_symbol_table()
{
  static SymbolTable symbol_table = []() {
    SymbolTable symbols;
    init_symbol_table(symbols);
    return symbols;
  }();
  return symbol_table;
}

std::shared_ptr<ExpressionNodeGroup> expression_node_to_group(const bNode &node,
                                                              const StringRef tree_idname,
                                                              const Span<StringRef> expressions,
                                                              const Span<int> expr_indices)
{
  BLI_assert(expressions.size() == expr_indices.size());
  auto output = std::make_shared<ExpressionNodeGroup>();

  ResourceScope parse_scope;
  Vector<ast::Expr *> expr_asts;
  for (const int i : expressions.index_range()) {
    ParseResult parse_result = expression::parse(parse_scope, expressions[i]);
    if (const std::string *error = std::get_if<std::string>(&parse_result)) {
      output->error = *error;
      return output;
    }
    expr_asts.append(std::get<ast::Expr *>(parse_result));
  }

  const SymbolTable &symbols = get_symbol_table();

  bNodeTree *tree = bke::node_tree_add_tree(nullptr, node.name, tree_idname);

  output->tree = tree;
  BuildOptions options;
  AstToNodeGroupBuilder builder(
      node, expr_asts, expr_indices, symbols, options, *tree, output->error);
  builder.build();
  if (!output->error.empty()) {
    BKE_id_free(nullptr, &tree->id);
    output->tree = nullptr;
  }
  return output;
}

ExpressionNodeGroup::~ExpressionNodeGroup()
{
  if (this->tree) {
    BKE_id_free(nullptr, const_cast<ID *>(&this->tree->id));
  }
}

bNode &InsertCallParams::add_node(const UString idname)
{
  return this->builder.add_node(idname);
}

void InsertCallParams::update_node_sockets(bNode &node)
{
  update_node_declaration_and_sockets(this->builder.r_tree_, node);
  if (node.typeinfo->updatefunc) {
    /* Ensure socket availability is up to date. */
    node.typeinfo->updatefunc(&this->builder.r_tree_, &node);
  }
}

}  // namespace blender::nodes::expression
