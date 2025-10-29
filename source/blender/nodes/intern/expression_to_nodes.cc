/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <sstream>

#include <fmt/format.h>

#include <fast_float.h>

#include "BLI_listbase.h"
#include "BLI_resource_scope.hh"
#include "BLT_translation.hh"

#include "NOD_expression_parse.hh"
#include "NOD_expression_to_nodes.hh"
#include "NOD_socket.hh"

#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "DNA_node_types.h"

namespace blender::nodes::expression {

class AstToNodeGroupBuilder;

struct NodeAndSocket {
  bNode *node = nullptr;
  bNodeSocket *socket = nullptr;
};

struct TypeCheckCallParams {
  Vector<const bke::bNodeSocketType *> input_types;
};

struct InsertCallParams {
  AstToNodeGroupBuilder &builder;

  Vector<NodeAndSocket> inputs;
  NodeAndSocket output;

  bNode &add_node(const StringRef idname);
  void update_node_sockets(bNode &node);

  void add_input(bNode &node, bNodeSocket &socket)
  {
    this->inputs.append({&node, &socket});
  }

  void set_output(bNode &node, bNodeSocket &socket)
  {
    /* Should only be set once. */
    BLI_assert(!this->output.socket);
    this->output = {&node, &socket};
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

  NodeAndSocket build_expr(const ast::Number &ast_node)
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
    const StringRef op = ast_node.op;
    NodeAndSocket a = this->build_expr(*ast_node.a);
    if (!a.socket) {
      return {};
    }
    NodeAndSocket b = this->build_expr(*ast_node.b);
    if (!b.socket) {
      return {};
    }
    const bke::bNodeSocketType &a_type = *a.socket->typeinfo;
    const bke::bNodeSocketType &b_type = *b.socket->typeinfo;

    TypeCheckCallParams type_check_params;
    type_check_params.input_types = {&a_type, &b_type};

    const Span<FunctionSymbol> candidates = symbol_table_.symbols_.lookup(op);
    if (candidates.is_empty()) {
      r_error_ = fmt::format("{}: \"{}\"", TIP_("Unknown binary operator"), op);
      return {};
    }
    Vector<const FunctionSymbol *> filtered_candidates;
    for (const FunctionSymbol &function : candidates) {
      if (function.type_check(type_check_params)) {
        filtered_candidates.append(&function);
      }
    }
    if (filtered_candidates.is_empty()) {
      r_error_ = fmt::format("{}: \"{}\"", TIP_("No matching binary operator"), op);
      return {};
    }
    if (filtered_candidates.size() > 1) {
      r_error_ = fmt::format("{}: \"{}\"", TIP_("Ambiguous binary operator"), op);
      return {};
    }
    const FunctionSymbol &function = *filtered_candidates[0];
    InsertCallParams insert_params{*this};
    function.insert(insert_params);
    BLI_assert(insert_params.inputs.size() == 2);
    BLI_assert(insert_params.output.socket);

    this->add_link(a, insert_params.inputs[0]);
    this->add_link(b, insert_params.inputs[1]);
    return insert_params.output;
  }

  NodeAndSocket build_expr(const ast::UnaryOp & /*ast_node*/)
  {
    r_error_ = "Unary operators are not supported yet";
    return {};
  }

  NodeAndSocket build_expr(const ast::MemberAccess & /*ast_node*/)
  {
    r_error_ = "Member access is not supported yet";
    return {};
  }

  NodeAndSocket build_expr(const ast::Call & /*ast_node*/)
  {
    r_error_ = "Function calls are not supported yet";
    return {};
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

static InsertCallFn float_math_node(const NodeMathOperation op)
{
  return [op](InsertCallParams &params) {
    bNode &math_node = params.add_node(StringRef("ShaderNodeMath"));
    math_node.custom1 = op;
    params.update_node_sockets(math_node);
    params.use_node_sockets(math_node);
  };
}

static void init_symbol_table(SymbolTable &symbols)
{
  symbols.add(FunctionSymbol("+", all_inputs_1d, float_math_node(NODE_MATH_ADD)));
  symbols.add(FunctionSymbol("-", all_inputs_1d, float_math_node(NODE_MATH_SUBTRACT)));
  symbols.add(FunctionSymbol("*", all_inputs_1d, float_math_node(NODE_MATH_MULTIPLY)));
  symbols.add(FunctionSymbol("/", all_inputs_1d, float_math_node(NODE_MATH_DIVIDE)));
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
