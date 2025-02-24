/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_geometry_nodes_bundle.hh"
#include "BKE_geometry_nodes_closure_fwd.hh"

#include "BLI_resource_scope.hh"

#include "FN_lazy_function.hh"

namespace blender::bke {

class ClosureSignature {
 public:
  struct Item {
    SocketInterfaceKey key;
    const bNodeSocketType *type = nullptr;
  };

  Vector<Item> inputs;
  Vector<Item> outputs;

  std::optional<int> find_input_index(const SocketInterfaceKey &key) const;
  std::optional<int> find_output_index(const SocketInterfaceKey &key) const;
};

struct ClosureFunctionIndices {
  struct {
    IndexRange main;
    IndexRange output_usages;
    /** Output bsocket index -> input lf socket index. */
    Map<int, int> output_data_reference_sets;
  } inputs;
  struct {
    IndexRange main;
    IndexRange input_usages;
  } outputs;
};

class Closure : public ImplicitSharingMixin {
 private:
  std::shared_ptr<ClosureSignature> signature_;
  std::unique_ptr<ResourceScope> scope_;
  const fn::lazy_function::LazyFunction &function_;
  ClosureFunctionIndices indices_;
  Vector<const void *> default_input_values_;

 public:
  Closure(std::shared_ptr<ClosureSignature> signature,
          std::unique_ptr<ResourceScope> scope,
          const fn::lazy_function::LazyFunction &function,
          ClosureFunctionIndices indices,
          Vector<const void *> default_input_values)
      : signature_(signature),
        scope_(std::move(scope)),
        function_(function),
        indices_(indices),
        default_input_values_(std::move(default_input_values))
  {
  }

  const ClosureSignature &signature() const
  {
    return *signature_;
  }

  const ClosureFunctionIndices &indices() const
  {
    return indices_;
  }

  const fn::lazy_function::LazyFunction &function() const
  {
    return function_;
  }

  const void *default_input_value(const int index) const
  {
    return default_input_values_[index];
  }

  void delete_self() override
  {
    MEM_delete(this);
  }
};

}  // namespace blender::bke
