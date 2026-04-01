/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_cache_mutex.hh"
#include "BLI_implicit_sharing_ptr.hh"

#include "FN_multi_function.hh"

namespace blender::fn {

class GField;
class FieldInput;
class FieldOperation;
class FieldInputs;
class FieldContext;

using FieldInputPtr = ImplicitSharingPtr<FieldInput>;
using FieldOperationPtr = ImplicitSharingPtr<FieldOperation>;
using FieldInputsPtr = ImplicitSharingPtr<FieldInputs>;
template<typename T> class Field;

class GField {
 public:
  struct Input {
    FieldInputPtr node;
  };

  struct MultiFn {
    FieldOperationPtr node;
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
    static constexpr int64_t inline_size = 16;
    static constexpr int64_t inline_alignment = 8;

    const CPPType *type = nullptr;
    AlignedBuffer<inline_size, inline_alignment> value;
  };

  struct GeneralConstant {
    const CPPType *type = nullptr;
    /* This value is owned by the #GField. */
    void *value = nullptr;
  };

  template<typename T>
  static constexpr bool is_constant_value_v =
      is_same_any_v<T, ConstantRef, TrivialInlineConstant, GeneralConstant>;

  using Variant =
      std::variant<Input, MultiFn, FieldRef, ConstantRef, TrivialInlineConstant, GeneralConstant>;

 private:
  Variant variant_;

  GField() = delete;

 public:
  explicit GField(const CPPType &type) noexcept;
  explicit GField(FieldInputPtr node) noexcept;
  explicit GField(FieldOperationPtr node, int output_i = 0) noexcept;
  explicit GField(Variant variant) noexcept;
  static GField from_non_owning_ref(const GField &field);
  static GField from_constant(const CPPType &type, const void *value);
  static GField from_non_owning_constant(const CPPType &type, const void *value);
  template<typename InputT, typename... Args> static GField from_input(Args &&...args);

  GField(const GField &other);
  GField(GField &&other) noexcept;
  GField &operator=(const GField &other);
  GField &operator=(GField &&other) noexcept;
  ~GField();

  const CPPType &cpp_type() const;

  const FieldInputsPtr &field_inputs() const;

  const GField &deref_field_ref() const;

  const Variant &variant() const;

  bool depends_on_input() const;

  template<typename InputT> const InputT *get_input_if() const;

  /**
   * Equality at this level is only checked in a shallow way. A more deep comparison could reveal
   * that two fields are semantically the same even if this comparison is false.
   */
  friend bool operator==(const GField &a, const GField &b);
  uint64_t hash() const;

  template<typename T> const Field<T> &typed() const;
  template<typename T> Field<T> &typed();
};

template<typename T> class Field {
 public:
  using base_type = T;

 private:
  GField field_;

  friend GField;

  Field(GField field);

 public:
  Field();
  explicit Field(FieldInputPtr node);
  explicit Field(FieldOperationPtr node, int output_i = 0);

  operator const GField &() const;

  bool depends_on_input() const;

  template<typename InputT, typename... Args> static Field from_input(Args &&...args);

  template<typename InputT> const InputT *get_input_if() const;

  uint64_t hash() const;
};

class GFieldRef {
 public:
  struct Value {
    const CPPType *type = nullptr;
    const void *value = nullptr;
  };
  struct Input {
    const FieldInput *node = nullptr;
  };
  struct MultiFn {
    const FieldOperation *node = nullptr;
    int output_i = 0;
  };

  using Variant = std::variant<Value, Input, MultiFn>;

 private:
  Variant variant_;

 public:
  GFieldRef(const GField &field);
  template<typename T> GFieldRef(const Field<T> &field);

  explicit GFieldRef(const FieldInput &field_input);
  explicit GFieldRef(const FieldOperation &field_multi_fn, int output_i = 0);

  const Variant &variant() const;

  const CPPType &cpp_type() const;

  const FieldInputsPtr &field_inputs() const;

  uint64_t hash() const;
};

class FieldContext {
 public:
  virtual ~FieldContext() = default;

  virtual GVArray get_varray_for_input(const FieldInput &field_input,
                                       const IndexMask &mask,
                                       ResourceScope &scope) const;
};

class FieldInputs : public ImplicitSharingMixin {
 public:
  VectorSet<std::reference_wrapper<const FieldInput>> deduplicated_nodes;

  void delete_self() override;
};

class FieldInput : public ImplicitSharingMixin {
 protected:
  const CPPType *type_;
  std::string debug_name_;

  /**
   * Field inputs are initialized lazily because it can't be done in the constructor because the
   * derived class constructor has not run yet.
   */
  mutable CacheMutex field_inputs_mutex_;
  mutable FieldInputsPtr field_inputs_;

 public:
  FieldInput(const CPPType &type, std::string debug_name = "");

  StringRefNull debug_name() const;
  virtual std::string socket_inspection_name() const;

  const CPPType &cpp_type() const;

  const FieldInputsPtr &field_inputs() const;

  virtual uint64_t hash() const;
  virtual bool is_equal_to(const FieldInput &other) const;

  virtual void foreach_recursive_field(FunctionRef<void(const GField &)> fn) const;

  virtual GVArray get_varray_for_context(const FieldContext &context,
                                         const IndexMask &mask,
                                         ResourceScope &scope) const = 0;

  void delete_self() override;
};

class FieldOperation : public ImplicitSharingMixin {
 private:
  Vector<GField> inputs_;
  std::shared_ptr<const mf::MultiFunction> owned_fn_;
  const mf::MultiFunction *fn_;
  FieldInputsPtr field_inputs_;

 public:
  FieldOperation(std::shared_ptr<const mf::MultiFunction> fn, Vector<GField> inputs);
  FieldOperation(const mf::MultiFunction &fn, Vector<GField> inputs);

  static FieldOperationPtr from(std::shared_ptr<const mf::MultiFunction> fn,
                                Vector<GField> inputs);
  static FieldOperationPtr from_non_owning(const mf::MultiFunction &fn, Vector<GField> inputs);

  const CPPType &output_cpp_type(int output_i) const;

  const mf::MultiFunction &multi_function() const;

  const FieldInputsPtr &field_inputs() const;

  void delete_self() override;

  Span<GField> inputs() const;
};

template<typename T> constexpr bool is_field_v = false;
template<typename T> constexpr bool is_field_v<Field<T>> = true;

/* -------------------------------------------------------------------- */
/** \name Inline Methods
 * \{ */

inline FieldInput::FieldInput(const CPPType &type, std::string debug_name)
    : type_(&type), debug_name_(std::move(debug_name))
{
}

inline GField::GField(const CPPType &type) noexcept
    : variant_(ConstantRef{&type, type.default_value()})
{
}
inline GField::GField(FieldInputPtr node) noexcept : variant_(Input{std::move(node)}) {}
inline GField::GField(Variant variant) noexcept : variant_(std::move(variant)) {}
inline GField::GField(FieldOperationPtr node, const int output_i) noexcept
    : variant_(MultiFn{std::move(node), output_i})
{
}

inline GField GField::from_non_owning_ref(const GField &field)
{
  return GField(FieldRef{&field});
}

inline GField GField::from_constant(const CPPType &type, const void *value)
{
  if (type.is_trivial && type.size <= TrivialInlineConstant::inline_size &&
      type.alignment <= TrivialInlineConstant::inline_alignment)
  {
    TrivialInlineConstant constant;
    constant.type = &type;
    type.copy_construct(value, constant.value.ptr());
    return GField(constant);
  }
  void *new_value = MEM_new_uninitialized_aligned(type.size, type.alignment, __func__);
  type.copy_construct(value, new_value);
  return GField(GeneralConstant{&type, new_value});
}

inline GField GField::from_non_owning_constant(const CPPType &type, const void *value)
{
  return GField(ConstantRef{&type, value});
}

template<typename InputT, typename... Args> inline GField GField::from_input(Args &&...args)
{
  FieldInputPtr input{MEM_new<InputT>(__func__, std::forward<Args>(args)...)};
  return GField(Input{std::move(input)});
}

template<typename T>
template<typename InputT, typename... Args>
inline Field<T> Field<T>::from_input(Args &&...args)
{
  return GField::from_input<InputT>(std::forward<Args>(args)...).template typed<T>();
}

template<typename T> inline bool Field<T>::depends_on_input() const
{
  return field_.depends_on_input();
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

inline const FieldInputsPtr &GField::field_inputs() const
{
  static const ImplicitSharingPtr<FieldInputs> empty_inputs;
  return std::visit(
      []<typename T>(const T &v) -> const FieldInputsPtr & {
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

inline const GField &GField::deref_field_ref() const
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
        else if constexpr (GField::is_constant_value_v<T>) {
          const CPPType &type_a = *v_a.type;
          const void *constant_a = v_a.value;
          return std::visit(
              [&]<typename U>(const U &v_b) -> bool {
                if constexpr (GField::is_constant_value_v<U>) {
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
        if constexpr (std::is_same_v<T, Input>) {
          return get_default_hash(v.node);
        }
        else if constexpr (std::is_same_v<T, MultiFn>) {
          return get_default_hash(v.node, v.output_i);
        }
        else if constexpr (std::is_same_v<T, FieldRef>) {
          /* Should not exist due to #deref_field_ref above. */
          BLI_assert_unreachable();
          return 0;
        }
        else if constexpr (is_constant_value_v<T>) {
          return v.type->hash_or_fallback(v.value, uint64_t(v.type));
        }
      },
      ref.variant_);
}

template<typename T> inline bool operator==(const Field<T> &a, const Field<T> &b)
{
  return static_cast<const GField &>(a) == static_cast<const GField &>(b);
}

template<typename T> inline uint64_t Field<T>::hash() const
{
  return field_.hash();
}

inline const FieldInputsPtr &FieldInput::field_inputs() const
{
  field_inputs_mutex_.ensure([&]() {
    FieldInputs *inputs = MEM_new<FieldInputs>(__func__);
    inputs->deduplicated_nodes.add(*this);
    field_inputs_ = FieldInputsPtr(inputs);
  });
  return field_inputs_;
}

inline const CPPType &FieldInput::cpp_type() const
{
  return *this->type_;
}

inline uint64_t FieldInput::hash() const
{
  return get_default_hash(this);
}

inline bool FieldInput::is_equal_to(const FieldInput &other) const
{
  return this == &other;
}

inline void FieldInput::foreach_recursive_field(FunctionRef<void(const GField &)> /*fn*/) const {}

inline const FieldInputsPtr &FieldOperation::field_inputs() const
{
  return field_inputs_;
}

inline void FieldInput::delete_self()
{
  MEM_delete(this);
}

inline void FieldOperation::delete_self()
{
  MEM_delete(this);
}

inline void FieldInputs::delete_self()
{
  MEM_delete(this);
}

inline const CPPType &FieldOperation::output_cpp_type(const int output_i) const
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

inline FieldOperationPtr FieldOperation::from(std::shared_ptr<const mf::MultiFunction> fn,
                                              Vector<GField> inputs)
{
  return FieldOperationPtr(MEM_new<FieldOperation>(__func__, std::move(fn), std::move(inputs)));
}

inline FieldOperationPtr FieldOperation::from_non_owning(const mf::MultiFunction &fn,
                                                         Vector<GField> inputs)
{
  return FieldOperationPtr(MEM_new<FieldOperation>(__func__, fn, inputs));
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
    for (const FieldInput &field_input : (*smaller_candidate)->deduplicated_nodes) {
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
    if (candidate) {
      return *candidate;
    }
    return {};
  }
  FieldInputs *new_field_inputs = MEM_new<FieldInputs>(__func__);
  for (const GField &field : fields) {
    const FieldInputsPtr &field_inputs_ptr = field.field_inputs();
    if (!field_inputs_ptr) {
      continue;
    }
    for (const FieldInput &field_input : field_inputs_ptr->deduplicated_nodes) {
      new_field_inputs->deduplicated_nodes.add(field_input);
    }
  }
  return FieldInputsPtr(new_field_inputs);
}

inline FieldOperation::FieldOperation(std::shared_ptr<const mf::MultiFunction> fn,
                                      Vector<GField> inputs)
    : FieldOperation(*fn, std::move(inputs))
{
  owned_fn_ = std::move(fn);
}

inline FieldOperation::FieldOperation(const mf::MultiFunction &fn, Vector<GField> inputs)
    : inputs_(inputs), fn_(&fn)
{
  field_inputs_ = combine_field_inputs(inputs_);
}

inline StringRefNull FieldInput::debug_name() const
{
  return debug_name_;
}

inline std::string FieldInput::socket_inspection_name() const
{
  return debug_name_;
}

template<typename T> inline Field<T>::operator const GField &() const
{
  return field_;
}

template<typename T> inline const Field<T> &GField::typed() const
{
  static_assert(sizeof(GField) == sizeof(Field<T>));
  BLI_assert(this->cpp_type().is<T>());
  return reinterpret_cast<const Field<T> &>(*this);
}

template<typename T> inline Field<T> &GField::typed()
{
  static_assert(sizeof(GField) == sizeof(Field<T>));
  BLI_assert(this->cpp_type().is<T>());
  return reinterpret_cast<Field<T> &>(*this);
}

inline const GField::Variant &GField::variant() const
{
  return variant_;
}

inline GField::GField(const GField &other) : variant_(other.variant_)
{
  std::visit(
      [&]<typename T>(T &v) {
        if constexpr (std::is_same_v<T, GeneralConstant>) {
          void *new_value = MEM_new_uninitialized_aligned(
              v.type->size, v.type->alignment, __func__);
          v.type->copy_construct(v.value, new_value);
          v.value = new_value;
        }
      },
      variant_);
}

inline GField::GField(GField &&other) noexcept : variant_(std::move(other.variant_))
{
  const CPPType &type = this->cpp_type();
  other.variant_ = ConstantRef{&type, type.default_value()};
}

inline GField &GField::operator=(const GField &other)
{
  if (this == &other) {
    return *this;
  }
  this->~GField();
  new (this) GField(other);
  return *this;
}

inline GField &GField::operator=(GField &&other) noexcept
{
  if (this == &other) {
    return *this;
  }
  this->~GField();
  new (this) GField(std::move(other));
  return *this;
}

inline GField::~GField()
{
  std::visit(
      [&]<typename T>(T &v) {
        if constexpr (std::is_same_v<T, GeneralConstant>) {
          v.type->destruct(v.value);
          MEM_delete_void(v.value);
        }
      },
      variant_);
}

template<typename T> inline Field<T>::Field(GField field) : field_(std::move(field)) {}
template<typename T> inline Field<T>::Field() : field_(CPPType::get<T>()) {}

template<typename T> inline Field<T>::Field(FieldInputPtr node) : Field(GField(std::move(node))) {}
template<typename T>
inline Field<T>::Field(FieldOperationPtr node, const int output_i)
    : Field(GField(std::move(node), output_i))
{
}

inline bool GField::depends_on_input() const
{
  const FieldInputsPtr &inputs = this->field_inputs();
  if (!inputs) {
    return false;
  }
  return !inputs->deduplicated_nodes.is_empty();
}

template<typename InputT> inline const InputT *GField::get_input_if() const
{
  const GField &deref_field = this->deref_field_ref();
  if (const auto *input = std::get_if<Input>(&deref_field.variant())) {
    return dynamic_cast<const InputT *>(input->node.get());
  }
  return nullptr;
}

template<typename T> template<typename InputT> inline const InputT *Field<T>::get_input_if() const
{
  return field_.get_input_if<InputT>();
}

inline Span<GField> FieldOperation::inputs() const
{
  return inputs_;
}

inline GFieldRef::GFieldRef(const FieldInput &field_input) : variant_(Input{&field_input}) {}

inline GFieldRef::GFieldRef(const FieldOperation &field_multi_fn, int output_i)
    : variant_(MultiFn{&field_multi_fn, output_i})
{
}

inline GFieldRef::GFieldRef(const GField &field)
    : variant_(std::visit(
          []<typename T>(const T &v) -> Variant {
            if constexpr (std::is_same_v<T, GField::Input>) {
              return Input{v.node.get()};
            }
            else if constexpr (std::is_same_v<T, GField::MultiFn>) {
              return MultiFn{v.node.get(), v.output_i};
            }
            else if constexpr (std::is_same_v<T, GField::FieldRef>) {
              /* Should not exist due to #deref_field_ref. */
              BLI_assert_unreachable();
              return Value{};
            }
            else if constexpr (GField::is_constant_value_v<T>) {
              return Value{v.type, v.value};
            }
          },
          field.deref_field_ref().variant()))
{
}

template<typename T>
inline GFieldRef::GFieldRef(const Field<T> &field) : GFieldRef(static_cast<const GField &>(field))
{
}

inline const GFieldRef::Variant &GFieldRef::variant() const
{
  return variant_;
}

inline const CPPType &GFieldRef::cpp_type() const
{
  return std::visit(
      []<typename T>(const T &v) -> const CPPType & {
        if constexpr (std::is_same_v<T, Value>) {
          return *v.type;
        }
        else if constexpr (std::is_same_v<T, Input>) {
          return v.node->cpp_type();
        }
        else if constexpr (std::is_same_v<T, MultiFn>) {
          return v.node->output_cpp_type(v.output_i);
        }
      },
      variant_);
}

inline const FieldInputsPtr &GFieldRef::field_inputs() const
{
  static const ImplicitSharingPtr<FieldInputs> empty_inputs;
  return std::visit(
      [&]<typename T>(const T &v) -> const FieldInputsPtr & {
        if constexpr (std::is_same_v<T, Input>) {
          return v.node->field_inputs();
        }
        else if constexpr (std::is_same_v<T, MultiFn>) {
          return v.node->field_inputs();
        }
        else if constexpr (std::is_same_v<T, Value>) {
          return empty_inputs;
        }
      },
      variant_);
}

inline bool operator==(const GFieldRef &a, const GFieldRef &b)
{
  return std::visit(
      [&]<typename T>(const T &v_a) -> bool {
        if constexpr (std::is_same_v<T, GFieldRef::Value>) {
          if (const auto *v_b = std::get_if<GFieldRef::Value>(&b.variant())) {
            if (v_a.type != v_b->type) {
              return false;
            }
            return v_a.type->is_equal_or_false(v_a.value, v_b->value);
          }
          return false;
        }
        else if constexpr (std::is_same_v<T, GFieldRef::Input>) {
          if (const auto *v_b = std::get_if<GFieldRef::Input>(&b.variant())) {
            return v_a.node == v_b->node;
          }
          return false;
        }
        else if constexpr (std::is_same_v<T, GFieldRef::MultiFn>) {
          if (const auto *v_b = std::get_if<GFieldRef::MultiFn>(&b.variant())) {
            return v_a.node == v_b->node && v_a.output_i == v_b->output_i;
          }
          return false;
        }
      },
      a.variant());
}

inline uint64_t GFieldRef::hash() const
{
  return std::visit(
      [&]<typename T>(const T &v) -> uint64_t {
        if constexpr (std::is_same_v<T, Value>) {
          return v.type->hash_or_fallback(v.value, uint64_t(v.type));
        }
        else if constexpr (std::is_same_v<T, Input>) {
          return get_default_hash(v.node);
        }
        else if constexpr (std::is_same_v<T, MultiFn>) {
          return get_default_hash(v.node, v.output_i);
        }
      },
      variant_);
}

inline bool operator==(const FieldInput &a, const FieldInput &b)
{
  return a.is_equal_to(b);
}

inline const mf::MultiFunction &FieldOperation::multi_function() const
{
  return *this->fn_;
}

/** \} */

}  // namespace blender::fn
