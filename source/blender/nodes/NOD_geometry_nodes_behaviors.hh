/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector_set.hh"

#include "NOD_geometry_nodes_behaviors_fwd.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_bundle_signature.hh"
#include "NOD_node_declaration.hh"

namespace blender::nodes {

class BehaviorDef {
 public:
  struct Item {
    std::string name;
    std::unique_ptr<SocketDeclaration> decl;
  };

  struct ItemNameGetter {
    std::string operator()(const Item &item)
    {
      return item.name;
    }
  };

  std::string type;
  CustomIDVectorSet<Item, ItemNameGetter> items;

  Vector<std::unique_ptr<BaseSocketDeclarationBuilder>> builders;

  BundleSignature to_bundle_signature() const;

  const SocketDeclaration *find_decl(const StringRef name) const;

  template<typename DeclType> typename DeclType::Builder &add(std::string name)
  {
    static_assert(std::is_base_of_v<SocketDeclaration, DeclType>);
    using SocketBuilder = typename DeclType::Builder;

    auto decl_ptr = std::make_unique<DeclType>();
    DeclType &decl = *decl_ptr;

    auto decl_builder_ptr = std::make_unique<SocketBuilder>();
    SocketBuilder &decl_builder = *decl_builder_ptr;

    decl_builder.decl_ = &decl;
    decl_builder.decl_base_ = &decl;

    decl.name = name;
    decl.identifier = name;
    decl.in_out = SOCK_IN;
    decl.socket_type = DeclType::static_socket_type;

    this->items.add_new(Item{std::move(name), std::move(decl_ptr)});
    this->builders.append(std::move(decl_builder_ptr));
    return decl_builder;
  }

  BundleSignature signature;
};

class BehaviorListDef {
 public:
  struct BehaviorDefTypeGetter {
    StringRef operator()(const std::shared_ptr<BehaviorDef> &def) const
    {
      return def->type;
    }
  };

  CustomIDVectorSet<std::shared_ptr<BehaviorDef>, BehaviorDefTypeGetter> behaviors;

  BehaviorDef &add(std::string name);
};

class BehaviorParseErrors {
 public:
  Vector<std::string> wrong_members;
  Vector<std::string> other_errors;

  bool has_error() const
  {
    return !this->wrong_members.is_empty();
  }
};

class BehaviorCommon {
 public:
  std::string self_path;
};

class BehaviorRegistry {

 private:
  Set<std::shared_ptr<const BehaviorListDef>> behavior_list_defs_;

 public:
  void add(std::shared_ptr<const BehaviorListDef> behavior_list_def);

  VectorSet<std::string> get_all_behavior_names() const;
  VectorSet<std::shared_ptr<const BehaviorDef>> get_behaviors_by_type(StringRef type) const;
};

BehaviorRegistry &get_behavior_registry();

namespace behaviors {

void foreach_behavior_in_bundle(
    const Bundle &behaviors_bundle,
    FunctionRef<void(StringRef type, const Bundle &behavior_bundle, Span<StringRef> path)> fn);

bool behavior_path_is_selected(StringRef self_path, StringRef filter, StringRef other);

template<typename T>
inline void parse_member(const Bundle &bundle,
                         const StringRef name,
                         T &r_value,
                         BehaviorParseErrors &r_errors)
{
  if (const std::optional<T> value = bundle.lookup<T>(name)) {
    r_value = *value;
  }
  else {
    r_errors.wrong_members.append(name);
  }
}

}  // namespace behaviors

}  // namespace blender::nodes
