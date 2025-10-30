/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>
#include <sstream>

#include <fmt/format.h>

#include <fast_float.h>

#include "BLI_listbase.h"
#include "BLI_resource_scope.hh"
#include "BLI_string.h"
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

struct TypeCheckCallParams {
  const bke::bNodeTreeType &tree_type;
  Vector<const bke::bNodeSocketType *> input_types;
};

static bNodeSocket *find_available_socket_by_index(ListBase &sockets, const int index)
{
  int remaining = index;
  LISTBASE_FOREACH (bNodeSocket *, socket, &sockets) {
    if (!socket->is_available()) {
      continue;
    }
    if (remaining == 0) {
      return socket;
    }
    remaining--;
  }
  return nullptr;
}

struct InsertCallParams {
  AstToNodeGroupBuilder &builder;
  const bNodeTree &tree;

  Vector<NodeAndSocket> inputs;
  NodeAndSocket output;

  bNode &add_node(const StringRef idname);
  void update_node_sockets(bNode &node);

  void add_input(bNode &node, bNodeSocket &socket)
  {
    this->inputs.append({&node, &socket});
  }

  void add_input(bNode &node, const int index)
  {
    bNodeSocket *socket = find_available_socket_by_index(node.inputs, index);
    BLI_assert(socket);
    this->add_input(node, *socket);
  }

  void set_output(bNode &node, bNodeSocket &socket)
  {
    /* Should only be set once. */
    BLI_assert(!this->output.socket);
    this->output = {&node, &socket};
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
    LISTBASE_FOREACH (bNodeSocket *, socket, &node.inputs) {
      if (socket->is_available()) {
        this->add_input(node, *socket);
      }
    }
  }

  void use_node_output(bNode &node)
  {
    LISTBASE_FOREACH (bNodeSocket *, socket, &node.outputs) {
      if (socket->is_available()) {
        this->set_output(node, *socket);
      }
    }
  }
};

using InsertCallFn = std::function<void(InsertCallParams &params)>;
using TypeCheckCallFn = std::function<bool(TypeCheckCallParams &params)>;

class FunctionSymbol {
 public:
  std::string name;
  TypeCheckCallFn type_check;
  InsertCallFn insert;

  FunctionSymbol(std::string name, TypeCheckCallFn type_check, InsertCallFn insert)
      : name(std::move(name)), type_check(std::move(type_check)), insert(std::move(insert))
  {
  }
};

class SymbolTable {
 private:
  MultiValueMap<std::string, FunctionSymbol> symbols_;

  friend AstToNodeGroupBuilder;

 public:
  void add(FunctionSymbol function_symbol)
  {
    symbols_.add(function_symbol.name, std::move(function_symbol));
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

  Map<StringRef, NodeAndSocket> inputs_;

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

    bNode &group_input_node = this->add_node("NodeGroupInput");
    bNode &group_output_node = this->add_node("NodeGroupOutput");

    {
      bNodeSocket *group_input_socket = static_cast<bNodeSocket *>(group_input_node.outputs.first);
      for ([[maybe_unused]] const int i : IndexRange(bnode_storage_.input_items.items_num)) {
        const NodeExpressionInputItem &item = bnode_storage_.input_items.items[i];
        inputs_.add(item.name, {&group_input_node, group_input_socket});
        group_input_socket = group_input_socket->next;
      }
    }

    {
      bNodeSocket *group_output_socket = static_cast<bNodeSocket *>(
          group_output_node.inputs.first);
      for (const int i : expr_indices_.index_range()) {
        NodeAndSocket expr_result = this->build_expr(*root_exprs_[i]);
        if (!expr_result) {
          return;
        }
        this->add_link(expr_result, {&group_output_node, group_output_socket});
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
          item.name, "", stype->idname, NODE_INTERFACE_SOCKET_INPUT, nullptr);
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
          expr_item.name, "", output_stype->idname, NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
    }
  }

  NodeAndSocket build_expr(const ast::Expr &expr)
  {
    return std::visit([&](const auto &ast_node) { return this->build_expr(ast_node); }, expr.expr);
  }

  NodeAndSocket build_expr(const ast::NumberLiteral &ast_node)
  {
    float value;
    fast_float::from_chars_result result = fast_float::from_chars(
        ast_node.value.begin(), ast_node.value.end(), value);
    if (result.ec != std::errc()) {
      r_error_ = fmt::format("{}: {}", TIP_("Invalid number"), ast_node.value);
      return {};
    }
    bNode &node = this->add_node("ShaderNodeValue");
    bNodeSocket *socket = static_cast<bNodeSocket *>(node.outputs.first);
    socket->default_value_typed<bNodeSocketValueFloat>()->value = value;
    return {&node, socket};
  }

  NodeAndSocket build_expr(const ast::StringLiteral &ast_node)
  {
    bNode &node = this->add_node("FunctionNodeInputString");
    auto &storage = *static_cast<NodeInputString *>(node.storage);
    const StringRef str = ast_node.value.drop_known_prefix("\"").drop_known_suffix("\"");
    storage.string = BLI_strdupn(str.data(), str.size());
    return {&node, static_cast<bNodeSocket *>(node.outputs.first)};
  }

  NodeAndSocket build_expr(const ast::Identifier &ast_node)
  {
    NodeAndSocket input = inputs_.lookup_default(ast_node.identifier, {});
    if (!input.socket) {
      r_error_ = fmt::format("{}: {}", TIP_("Unknown variable"), ast_node.identifier);
      return {};
    }
    return input;
  }

  NodeAndSocket build_expr(const ast::BinaryOp &ast_node)
  {
    return this->build_generic_call(ast_node.op, {ast_node.a, ast_node.b});
  }

  NodeAndSocket build_expr(const ast::UnaryOp &ast_node)
  {
    return this->build_generic_call(ast_node.op, {ast_node.expr});
  }

  NodeAndSocket build_expr(const ast::ConditionalOp &ast_node)
  {
    return this->build_generic_call("?:",
                                    {ast_node.condition, ast_node.true_expr, ast_node.false_expr});
  }

  NodeAndSocket build_expr(const ast::MemberAccess &ast_node)
  {
    return this->build_generic_call("." + ast_node.identifier, {ast_node.expr});
  }

  NodeAndSocket build_expr(const ast::Call &ast_node)
  {
    if (const ast::Identifier *identifier = std::get_if<ast::Identifier>(&ast_node.function->expr))
    {
      return this->build_generic_call(identifier->identifier, ast_node.args);
    }
    r_error_ = TIP_("Unexpected function call");
    return {};
  }

  NodeAndSocket build_generic_call(const StringRef name, const Span<const ast::Expr *> args)
  {
    Array<NodeAndSocket> arg_sockets(args.size());
    TypeCheckCallParams type_check_params{*r_tree_.typeinfo};
    type_check_params.input_types.resize(args.size());
    for (const int i : args.index_range()) {
      NodeAndSocket arg_socket = this->build_expr(*args[i]);
      if (!arg_socket) {
        /* There is an error in the argument. */
        return {};
      }
      type_check_params.input_types[i] = arg_socket.socket->typeinfo;
      arg_sockets[i] = arg_socket;
    }

    const Span<FunctionSymbol> candidates = symbol_table_.symbols_.lookup(name);
    if (candidates.is_empty()) {
      r_error_ = fmt::format("{}: \"{}\"", TIP_("Unknown function"), name);
      return {};
    }
    Vector<const FunctionSymbol *> filtered_candidates;
    for (const FunctionSymbol &function : candidates) {
      if (function.type_check(type_check_params)) {
        filtered_candidates.append(&function);
      }
    }
    if (filtered_candidates.is_empty()) {
      r_error_ = fmt::format("{}: \"{}\"", TIP_("No matching function"), name);
      return {};
    }
    if (filtered_candidates.size() > 1) {
      r_error_ = fmt::format("{}: \"{}\"", TIP_("Ambiguous function call"), name);
      return {};
    }
    const FunctionSymbol &function = *filtered_candidates[0];
    InsertCallParams insert_params{*this, r_tree_};
    function.insert(insert_params);
    BLI_assert(insert_params.inputs.size() == args.size());
    BLI_assert(insert_params.output.socket);

    for (const int i : args.index_range()) {
      this->add_link(arg_sockets[i], insert_params.inputs[i]);
    }
    return insert_params.output;
  }

  bNode &add_node(const StringRef idname)
  {
    return *bke::node_add_node(nullptr, r_tree_, idname);
  }

  bNodeLink &add_link(const NodeAndSocket &from, const NodeAndSocket &to)
  {
    return bke::node_add_link(r_tree_, *from.node, *from.socket, *to.node, *to.socket);
  }
};

static bool all_inputs_1d(TypeCheckCallParams &params)
{
  return std::all_of(
      params.input_types.begin(), params.input_types.end(), [](const bke::bNodeSocketType *stype) {
        return ELEM(stype->type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN);
      });
}

static FunctionSymbol float_math_function(const StringRef name,
                                          const NodeMathOperation op,
                                          const int inputs_num)
{
  return FunctionSymbol(
      name,
      [inputs_num](TypeCheckCallParams &params) {
        return params.input_types.size() == inputs_num && all_inputs_1d(params);
      },
      [op](InsertCallParams &params) {
        bNode &math_node = params.add_node("ShaderNodeMath");
        math_node.custom1 = op;
        params.update_node_sockets(math_node);
        params.use_node_sockets(math_node);
      });
}

static FunctionSymbol vector_math_function(const StringRef name,
                                           const NodeVectorMathOperation op,
                                           const int inputs_num)
{
  return FunctionSymbol(
      name,
      [inputs_num](TypeCheckCallParams &params) {
        if (params.input_types.size() != inputs_num) {
          return false;
        }
        bool any_input_is_vector = false;
        for (const bke::bNodeSocketType *stype : params.input_types) {
          if (stype->type == SOCK_VECTOR) {
            any_input_is_vector = true;
          }
          if (!ELEM(stype->type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_VECTOR)) {
            return false;
          }
        }
        if (!any_input_is_vector) {
          return false;
        }
        return true;
      },
      [op](InsertCallParams &params) {
        bNode &math_node = params.add_node("ShaderNodeVectorMath");
        math_node.custom1 = op;
        params.update_node_sockets(math_node);
        params.use_node_sockets(math_node);
      }

  );
}

static FunctionSymbol negate_float_function()
{
  return FunctionSymbol(
      "-",
      [](TypeCheckCallParams &params) {
        return params.input_types.size() == 1 && all_inputs_1d(params);
      },
      [](InsertCallParams &params) {
        bNode &math_node = params.add_node("ShaderNodeMath");
        math_node.custom1 = NODE_MATH_SUBTRACT;
        params.update_node_sockets(math_node);
        static_cast<bNodeSocket *>(math_node.inputs.first)
            ->default_value_typed<bNodeSocketValueFloat>()
            ->value = 0.0f;
        params.add_input(math_node, 1);
        params.use_node_output(math_node);
      });
}

static FunctionSymbol vector_member_access(const int index)
{
  BLI_assert(index >= 0 && index <= 2);
  return FunctionSymbol(
      fmt::format(".{}", char('x' + index)),
      [](TypeCheckCallParams &params) {
        return params.input_types.size() == 1 &&
               ELEM(params.input_types[0]->type, SOCK_VECTOR, SOCK_RGBA);
      },
      [index](InsertCallParams &params) {
        bNode &node = params.add_node("ShaderNodeSeparateXYZ");
        params.use_node_inputs(node);
        params.set_output(node, index);
      });
}

static FunctionSymbol attribute_access(const StringRef name, const eCustomDataType type)
{
  return FunctionSymbol(
      name,
      [](TypeCheckCallParams &params) {
        return params.input_types.size() == 1 && params.input_types[0]->type == SOCK_STRING;
      },
      [type](InsertCallParams &params) {
        bNode &node = params.add_node("GeometryNodeInputNamedAttribute");
        auto &storage = *static_cast<NodeGeometryInputNamedAttribute *>(node.storage);
        storage.data_type = type;
        params.update_node_sockets(node);
        params.use_node_inputs(node);
        params.set_output(node, 0);
      });
}

static FunctionSymbol string_concatenation()
{
  return FunctionSymbol(
      "+",
      [](TypeCheckCallParams &params) {
        return params.input_types.size() == 2 && params.input_types[0]->type == SOCK_STRING &&
               params.input_types[1]->type == SOCK_STRING;
      },
      [](InsertCallParams &params) {
        bNode &node = params.add_node("FunctionNodeFormatString");
        auto &storage = *static_cast<NodeFunctionFormatString *>(node.storage);
        storage.items = MEM_calloc_arrayN<NodeFunctionFormatStringItem>(2, "string_concatenation");
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

static FunctionSymbol ternary_conditional_operator(const eNodeSocketDatatype type)
{
  return FunctionSymbol(
      "?:",
      [type](TypeCheckCallParams &params) {
        if (params.input_types.size() != 3) {
          return false;
        }
        if (params.input_types[0]->type != SOCK_BOOLEAN) {
          return false;
        }
        if (params.input_types[1]->type != type) {
          return false;
        }
        if (params.input_types[2]->type != type) {
          return false;
        }
        if (params.tree_type.type != NTREE_GEOMETRY &&
            !ELEM(type, SOCK_FLOAT, SOCK_VECTOR, SOCK_RGBA, SOCK_INT, SOCK_BOOLEAN))
        {
          return false;
        }
        return true;
      },
      [type](InsertCallParams &params) {
        if (params.tree.type == NTREE_GEOMETRY) {
          bNode &node = params.add_node("GeometryNodeSwitch");
          auto &storage = *static_cast<NodeSwitch *>(node.storage);
          storage.input_type = type;
          params.update_node_sockets(node);
          params.add_input(node, 0);
          params.add_input(node, 2);
          params.add_input(node, 1);
          params.use_node_output(node);
          return;
        }
        bNode &node = params.add_node("ShaderNodeMix");
        NodeShaderMix &storage = *static_cast<NodeShaderMix *>(node.storage);
        storage.clamp_factor = false;
        if (ELEM(type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN)) {
          storage.data_type = type;
        }
        else if (type == SOCK_VECTOR) {
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

static FunctionSymbol create_vec_function()
{
  return FunctionSymbol(
      "vec",
      [](TypeCheckCallParams &params) {
        if (params.input_types.size() != 3) {
          return false;
        }
        return all_inputs_1d(params);
      },
      [](InsertCallParams &params) {
        bNode &node = params.add_node("ShaderNodeCombineXYZ");
        params.use_node_sockets(node);
      });
}

static StringRef get_combine_color_node_idname(const int tree_type)
{
  switch (tree_type) {
    case NTREE_GEOMETRY:
      return "FunctionNodeCombineColor";
    case NTREE_COMPOSIT:
      return "CompositorNodeCombineColor";
    case NTREE_SHADER:
      return "ShaderNodeCombineColor";
  }
  BLI_assert_unreachable();
  return {};
}

static FunctionSymbol create_rgb_function()
{
  return FunctionSymbol(
      "rgb",
      [](TypeCheckCallParams &params) {
        if (params.input_types.size() != 3) {
          return false;
        }
        return all_inputs_1d(params);
      },
      [](InsertCallParams &params) {
        const StringRef idname = get_combine_color_node_idname(params.tree.type);
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
      [](TypeCheckCallParams &params) {
        if (params.input_types.size() != 4) {
          return false;
        }
        if (!ELEM(params.tree_type.type, NTREE_COMPOSIT, NTREE_GEOMETRY)) {
          return false;
        }
        return all_inputs_1d(params);
      },
      [](InsertCallParams &params) {
        const StringRef idname = get_combine_color_node_idname(params.tree.type);
        bNode &node = params.add_node(idname);
        params.use_node_sockets(node);
      });
}

static StringRef get_separate_color_node_idname(const int tree_type)
{
  switch (tree_type) {
    case NTREE_GEOMETRY:
      return "FunctionNodeSeparateColor";
    case NTREE_COMPOSIT:
      return "CompositorNodeSeparateColor";
    case NTREE_SHADER:
      return "ShaderNodeSeparateColor";
  }
  BLI_assert_unreachable();
  return {};
}

static FunctionSymbol create_color_member_access(const int index)
{
  BLI_assert(index >= 0 && index < 4);
  return FunctionSymbol(
      fmt::format(".{}", char("rgba"[index])),
      [index](TypeCheckCallParams &params) {
        if (params.input_types.size() != 1) {
          return false;
        }
        if (index == 3 && params.tree_type.type == NTREE_SHADER) {
          return false;
        }
        if (!ELEM(params.input_types[0]->type, SOCK_RGBA, SOCK_VECTOR)) {
          return false;
        }
        return true;
      },
      [index](InsertCallParams &params) {
        const StringRef node_idname = get_separate_color_node_idname(params.tree.type);
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

  symbols.add(create_vec_function());
  symbols.add(create_rgb_function());
  symbols.add(create_rgba_function());

  symbols.add(vector_member_access(0));
  symbols.add(vector_member_access(1));
  symbols.add(vector_member_access(2));

  symbols.add(create_color_member_access(0));
  symbols.add(create_color_member_access(1));
  symbols.add(create_color_member_access(2));
  symbols.add(create_color_member_access(3));

  symbols.add(string_concatenation());

  symbols.add(attribute_access("attrf", CD_PROP_FLOAT));
  symbols.add(attribute_access("attrv", CD_PROP_FLOAT3));

  for (const eNodeSocketDatatype type : {SOCK_FLOAT,
                                         SOCK_INT,
                                         SOCK_BOOLEAN,
                                         SOCK_VECTOR,
                                         SOCK_STRING,
                                         SOCK_ROTATION,
                                         SOCK_MATRIX,
                                         SOCK_RGBA})
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

bNode &InsertCallParams::add_node(const StringRef idname)
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
