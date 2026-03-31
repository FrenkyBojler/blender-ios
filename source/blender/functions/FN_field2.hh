/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_implicit_sharing.hh"
#include "BLI_implicit_sharing_ptr.hh"

#include "FN_multi_function.hh"
#include "FN_multi_function_builder.hh"

namespace blender::fn {

class GField;
class FieldInputNode;
class FieldMultiFunctionNode;
class FieldInputs;
class FieldContext;

using FieldInputNodePtr = ImplicitSharingPtr<FieldInputNode>;
using FieldMultiFunctionNodePtr = ImplicitSharingPtr<FieldMultiFunctionNode>;
using FieldInputsPtr = ImplicitSharingPtr<FieldInputs>;
template<typename T> class Field;

class GField {
 public:
  struct Input {
    FieldInputNodePtr node;
  };

  struct MultiFn {
    FieldMultiFunctionNodePtr node;
    int output_i = 0;
  };

  struct FieldRef {
    const GField *field_ref = nullptr;
  };

  struct ConstantRef {
    const CPPType *type = nullptr;
    const void *value = nullptr;
  };

  struct TrivialInlineConstant {
    const CPPType *type = nullptr;
    AlignedBuffer<16, 8> value;
  };

  struct GeneralConstant {
    /* TODO: Ownership handling. */
    const CPPType *type = nullptr;
    const void *value = nullptr;
  };

  template<typename T>
  static constexpr bool is_constant_v =
      is_same_any_v<T, ConstantRef, TrivialInlineConstant, GeneralConstant>;

  using GFieldVariant =
      std::variant<Input, MultiFn, FieldRef, ConstantRef, TrivialInlineConstant, GeneralConstant>;

 private:
  GFieldVariant variant_;

  GField() = default;

 public:
  explicit GField(FieldInputNodePtr node);
  explicit GField(FieldMultiFunctionNodePtr node, int output_i);
  static GField from_non_owning_ref(const GField &field);
  static GField from_constant(const CPPType &type, const void *value);
  static GField from_non_owning_constant(const CPPType &type, const void *value);

  const CPPType &cpp_type() const;

  const ImplicitSharingPtr<FieldInputs> &field_inputs() const;

  const GField &deref_field_ref() const;

  /**
   * Equality at this level is only checked in a shallow way. A more deep comparison could reveal
   * that two fields are semantically the same even if this comparison is false.
   */
  friend bool operator==(const GField &a, const GField &b);
  uint64_t hash() const;
};

template<typename T> class Field {
  /* TODO */
};

class FieldContext {
 public:
  virtual ~FieldContext() = default;

  virtual GVArray get_varray_for_input(const FieldInputNode &field_input,
                                       const IndexMask &mask,
                                       ResourceScope &scope) const;
};

class FieldInputs : public ImplicitSharingMixin {
 public:
  VectorSet<std::reference_wrapper<const FieldInputNode>> deduplicated_nodes;

  void delete_self() override;
};

class FieldInputNode : public ImplicitSharingMixin {
 protected:
  const CPPType *type_;
  FieldInputsPtr field_inputs_;
  std::string debug_name_;

 public:
  FieldInputNode(const CPPType &type, std::string debug_name_ = "")
      : type_(&type), debug_name_(std::move(debug_name_))
  {
  }

  StringRefNull debug_name() const;
  virtual std::string socket_inspection_name() const;

  const CPPType &cpp_type() const;

  const FieldInputsPtr &field_inputs() const;

  virtual uint64_t hash() const;
  virtual bool is_equal_to(const FieldInputNode &other) const;

  virtual void foreach_recursive_field(FunctionRef<void(const GField &)> fn) const;

  virtual GVArray get_varray_for_context(const FieldContext &context,
                                         const IndexMask &mask,
                                         ResourceScope &scope) const = 0;

  void delete_self() override;
};

class FieldMultiFunctionNode : public ImplicitSharingMixin {
 private:
  Vector<GField> inputs_;
  std::shared_ptr<const mf::MultiFunction> owned_fn_;
  const mf::MultiFunction *fn_;
  FieldInputsPtr field_inputs_;

 public:
  FieldMultiFunctionNode(std::shared_ptr<const mf::MultiFunction> fn, Vector<GField> inputs);
  FieldMultiFunctionNode(const mf::MultiFunction &fn, Vector<GField> inputs);

  static FieldMultiFunctionNodePtr from(std::shared_ptr<const mf::MultiFunction> fn,
                                        Vector<GField> inputs);
  static FieldMultiFunctionNodePtr from_non_owning(const mf::MultiFunction &fn,
                                                   Vector<GField> inputs);

  const CPPType &output_cpp_type(int output_i) const;

  const mf::MultiFunction &multi_function() const;

  const FieldInputsPtr &field_inputs() const;

  void delete_self() override;
};

/* -------------------------------------------------------------------- */
/** \name Inline Methods
 * \{ */

inline GField::GField(FieldInputNodePtr node) : variant_(Input{std::move(node)}) {}
inline GField::GField(FieldMultiFunctionNodePtr node, const int output_i)
    : variant_(MultiFn{std::move(node), output_i})
{
}

inline GField GField::from_non_owning_ref(const GField &field)
{
  GField ret;
  ret.variant_ = FieldRef{&field};
  return ret;
}

inline GField GField::from_constant(const CPPType &type, const void *value)
{
  /* TODO: Avoid allocation if possible. */
  return GField(FieldMultiFunctionNode::from(
                    std::make_shared<mf::CustomMF_GenericConstant>(type, value, true), {}),
                0);
}

inline GField GField::from_non_owning_constant(const CPPType &type, const void *value)
{
  GField ret;
  ret.variant_ = ConstantRef{&type, value};
  return ret;
}

inline const CPPType &GField::cpp_type() const
{
  return std::visit(
      []<typename T>(const T &v) -> const CPPType & {
        if constexpr (std::is_same_v<T, Input>) {
          return v.node->cpp_type();
        }
        else if constexpr (std::is_same_v<T, MultiFn>) {
          return v.node->output_cpp_type(v.output_i);
        }
        else if constexpr (std::is_same_v<T, FieldRef>) {
          return v.field_ref->cpp_type();
        }
        else if constexpr (is_same_any_v<T, ConstantRef, TrivialInlineConstant, GeneralConstant>) {
          return *v.type;
        }
      },
      this->variant_);
}

inline const ImplicitSharingPtr<FieldInputs> &GField::field_inputs() const
{
  static const ImplicitSharingPtr<FieldInputs> empty_inputs;
  return std::visit(
      []<typename T>(const T &v) -> const ImplicitSharingPtr<FieldInputs> & {
        if constexpr (is_same_any_v<T, Input, MultiFn>) {
          return v.node->field_inputs();
        }
        else if constexpr (std::is_same_v<T, FieldRef>) {
          return v.field_ref->field_inputs();
        }
        else if constexpr (is_same_any_v<T, ConstantRef, TrivialInlineConstant, GeneralConstant>) {
          return empty_inputs;
        }
      },
      this->variant_);
}

const GField &GField::deref_field_ref() const
{
  if (const auto *field_ref = std::get_if<FieldRef>(&this->variant_)) {
    return field_ref->field_ref->deref_field_ref();
  }
  return *this;
}

inline bool operator==(const GField &a, const GField &b)
{
  const GField &a_ref = a.deref_field_ref();
  const GField &b_ref = b.deref_field_ref();

  return std::visit(
      [&]<typename T>(const T &v_a) -> bool {
        if constexpr (std::is_same_v<T, GField::Input>) {
          if (const auto *v_b = std::get_if<GField::Input>(&b_ref.variant_)) {
            return v_a.node == v_b->node;
          }
          return false;
        }
        else if constexpr (std::is_same_v<T, GField::MultiFn>) {
          if (const auto *v_b = std::get_if<GField::MultiFn>(&b_ref.variant_)) {
            return v_a.node == v_b->node && v_a.output_i == v_b->output_i;
          }
          return false;
        }
        else if constexpr (std::is_same_v<T, GField::FieldRef>) {
          /* Should not exist due to #deref_field_ref above. */
          BLI_assert_unreachable();
          return false;
        }
        else if constexpr (GField::is_constant_v<T>) {
          const CPPType &type_a = *v_a.type;
          const void *constant_a = v_a.value;
          return std::visit(
              [&]<typename U>(const U &v_b) -> bool {
                if constexpr (GField::is_constant_v<U>) {
                  const CPPType &type_b = *v_b.type;
                  if (type_a != type_b) {
                    return false;
                  }
                  const void *constant_b = v_b.value;
                  return type_a.is_equal_or_false(constant_a, constant_b);
                }
                else {
                  return false;
                }
              },
              b_ref.variant_);
        }
        return false;
      },
      a_ref.variant_);
}

inline uint64_t GField::hash() const
{
  const GField &ref = this->deref_field_ref();
  return std::visit(
      [&]<typename T>(const T &v) -> uint64_t {
        if constexpr (std::is_same_v<T, GField::Input>) {
          return get_default_hash(v.node);
        }
        else if constexpr (std::is_same_v<T, GField::MultiFn>) {
          return get_default_hash(v.node, v.output_i);
        }
        else if constexpr (std::is_same_v<T, GField::FieldRef>) {
          /* Should not exist due to #deref_field_ref above. */
          BLI_assert_unreachable();
          return 0;
        }
        else if constexpr (GField::is_constant_v<T>) {
          return v.type->hash_or_fallback(v.value, uint64_t(v.type));
        }
      },
      ref.variant_);
}

inline const FieldInputsPtr &FieldInputNode::field_inputs() const
{
  return field_inputs_;
}

inline const CPPType &FieldInputNode::cpp_type() const
{
  return *this->type_;
}

inline uint64_t FieldInputNode::hash() const
{
  return get_default_hash(*this);
}

inline bool FieldInputNode::is_equal_to(const FieldInputNode &other) const
{
  return this == &other;
}

inline void FieldInputNode::foreach_recursive_field(FunctionRef<void(const GField &)> /*fn*/) const
{
}

inline const FieldInputsPtr &FieldMultiFunctionNode::field_inputs() const
{
  return field_inputs_;
}

inline void FieldInputNode::delete_self()
{
  MEM_delete(this);
}

inline void FieldMultiFunctionNode::delete_self()
{
  MEM_delete(this);
}

inline void FieldInputs::delete_self()
{
  MEM_delete(this);
}

inline const CPPType &FieldMultiFunctionNode::output_cpp_type(const int output_i) const
{
  int count = 0;
  for (const int param_index : fn_->param_indices()) {
    const mf::ParamType param_type = fn_->param_type(param_index);
    if (param_type.is_output()) {
      if (count == output_i) {
        return param_type.data_type().single_type();
      }
      count++;
    }
  }
  BLI_assert_unreachable();
  return CPPType::get<float>();
}

inline FieldMultiFunctionNodePtr FieldMultiFunctionNode::from(
    std::shared_ptr<const mf::MultiFunction> fn, Vector<GField> inputs)
{
  return FieldMultiFunctionNodePtr(
      MEM_new<FieldMultiFunctionNode>(__func__, std::move(fn), std::move(inputs)));
}

inline FieldMultiFunctionNodePtr FieldMultiFunctionNode::from_non_owning(
    const mf::MultiFunction &fn, Vector<GField> inputs)
{
  return FieldMultiFunctionNodePtr(MEM_new<FieldMultiFunctionNode>(__func__, fn, inputs));
}

inline FieldInputsPtr combine_field_inputs(const Span<GField> &fields)
{
  bool candidate_valid = true;
  const FieldInputsPtr *candidate = nullptr;
  for (const GField &field : fields) {
    const FieldInputsPtr &field_inputs_ptr = field.field_inputs();
    if (!field_inputs_ptr) {
      continue;
    }
    if (!candidate) {
      candidate = &field_inputs_ptr;
      continue;
    }
    if (field_inputs_ptr == *candidate) {
      continue;
    }
    const FieldInputsPtr *smaller_candidate = candidate;
    const FieldInputsPtr *larger_candidate = &field_inputs_ptr;
    if ((*smaller_candidate)->deduplicated_nodes.size() >
        (*larger_candidate)->deduplicated_nodes.size())
    {
      std::swap(smaller_candidate, larger_candidate);
    }
    for (const FieldInputNode &field_input : (*smaller_candidate)->deduplicated_nodes) {
      if (!(*larger_candidate)->deduplicated_nodes.contains(field_input)) {
        candidate_valid = false;
        break;
      }
    }
    if (!candidate_valid) {
      break;
    }
    candidate = larger_candidate;
  }
  if (candidate_valid) {
    return *candidate;
  }
  FieldInputs *new_field_inputs = MEM_new<FieldInputs>(__func__);
  for (const GField &field : fields) {
    const FieldInputsPtr &field_inputs_ptr = field.field_inputs();
    if (!field_inputs_ptr) {
      continue;
    }
    for (const FieldInputNode &field_input : field_inputs_ptr->deduplicated_nodes) {
      new_field_inputs->deduplicated_nodes.add(field_input);
    }
  }
  return FieldInputsPtr(new_field_inputs);
}

inline FieldMultiFunctionNode::FieldMultiFunctionNode(std::shared_ptr<const mf::MultiFunction> fn,
                                                      Vector<GField> inputs)
    : FieldMultiFunctionNode(*fn, std::move(inputs))
{
  owned_fn_ = std::move(fn);
}

inline FieldMultiFunctionNode::FieldMultiFunctionNode(const mf::MultiFunction &fn,
                                                      Vector<GField> inputs)
    : inputs_(inputs), fn_(&fn)
{
  field_inputs_ = combine_field_inputs(inputs_);
}

inline StringRefNull FieldInputNode::debug_name() const
{
  return debug_name_;
}

inline std::string FieldInputNode::socket_inspection_name() const
{
  return debug_name_;
}

/** \} */

}  // namespace blender::fn
