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
  Vector<const bke::bNodeSocketType *> input_types;
};

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
    bNodeSocket *socket = static_cast<bNodeSocket *>(BLI_findlink(&node.inputs, index));
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
    bNodeSocket *socket = static_cast<bNodeSocket *>(BLI_findlink(&node.outputs, index));
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

class AstToNodeGroupBuilder {
 private:
  const NodeExpression &bnode_storage_;
  const ast::Expr &root_expr_;
  const int expr_index_;
  const SymbolTable &symbol_table_;
  bNodeTree &r_tree_;
  Map<StringRef, NodeAndSocket> inputs_;

  std::string &r_error_;

  friend InsertCallParams;

 public:
  AstToNodeGroupBuilder(const bNode &expr_bnode,
                        const ast::Expr &root_expr,
                        const int expr_index,
                        const SymbolTable &symbol_table,
                        bNodeTree &r_tree,
                        std::string &r_error)
      : bnode_storage_(*static_cast<const NodeExpression *>(expr_bnode.storage)),
        root_expr_(root_expr),
        expr_index_(expr_index),
        symbol_table_(symbol_table),
        r_tree_(r_tree),
        r_error_(r_error)
  {
    std::cout << "\n\n" << root_expr.to_dot() << "\n\n";
  }

  void build()
  {
    this->add_interface_inputs();
    this->add_interface_outputs();

    bNode &group_input_node = this->add_node("NodeGroupInput");
    bNode &group_output_node = this->add_node("NodeGroupOutput");

    LISTBASE_FOREACH (bNodeSocket *, socket, &group_input_node.outputs) {
      if (socket == group_input_node.outputs.last) {
        continue;
      }
      inputs_.add(socket->name, {&group_input_node, socket});
    }

    NodeAndSocket expr_result = this->build_expr(root_expr_);
    if (!expr_result.socket) {
      return;
    }

    this->add_link(
        expr_result,
        {&group_output_node, static_cast<bNodeSocket *>(group_output_node.inputs.first)});

    std::cout << "\n\n" << bke::node_tree_to_dot(r_tree_) << "\n\n";
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
    const NodeExpressionItem &expr_item = bnode_storage_.expression_items.items[expr_index_];
    const bke::bNodeSocketType *output_stype = bke::node_socket_type_find_static(
        expr_item.socket_type);
    r_tree_.tree_interface.add_socket(
        expr_item.name, "", output_stype->idname, NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
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

  NodeAndSocket build_expr(const ast::MemberAccess &ast_node)
  {
    return this->build_generic_call("." + ast_node.identifier, {ast_node.expr});
  }

  NodeAndSocket build_expr(const ast::Call &ast_node)
  {
    return this->build_generic_call(ast_node.identifier, ast_node.args);
  }

  NodeAndSocket build_generic_call(const StringRef name, const Span<const ast::Expr *> args)
  {
    Array<NodeAndSocket> arg_sockets(args.size());
    TypeCheckCallParams type_check_params;
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
        return params.input_types.size() == 1 && params.input_types[0]->type == SOCK_VECTOR;
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

static void init_symbol_table(SymbolTable &symbols)
{
  symbols.add(float_math_function("+", NODE_MATH_ADD, 2));
  symbols.add(float_math_function("-", NODE_MATH_SUBTRACT, 2));
  symbols.add(float_math_function("*", NODE_MATH_MULTIPLY, 2));
  symbols.add(float_math_function("/", NODE_MATH_DIVIDE, 2));
  symbols.add(float_math_function("sin", NODE_MATH_SINE, 1));
  symbols.add(float_math_function("cos", NODE_MATH_COSINE, 1));
  symbols.add(vector_member_access(0));
  symbols.add(vector_member_access(1));
  symbols.add(vector_member_access(2));
  symbols.add(negate_float_function());
  symbols.add(string_concatenation());
  symbols.add(attribute_access("attrf", CD_PROP_FLOAT));
  symbols.add(attribute_access("attrv", CD_PROP_FLOAT3));
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
                                                              const StringRef expression,
                                                              const int expr_index)
{
  auto output = std::make_shared<ExpressionNodeGroup>();

  ResourceScope parse_scope;
  std::stringstream errors;
  ast::Expr *expr_ast = expression::parse(parse_scope, expression, errors);
  if (!expr_ast) {
    output->error = errors.str();
    if (output->error.empty()) {
      output->error = TIP_("Parse error");
    }
    return output;
  }

  const SymbolTable &symbols = get_symbol_table();

  bNodeTree *tree = BKE_id_new_nomain<bNodeTree>(node.name);
  output->tree = tree;
  AstToNodeGroupBuilder builder(node, *expr_ast, expr_index, symbols, *tree, output->error);
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
