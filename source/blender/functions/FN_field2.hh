/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_implicit_sharing.hh"
#include "BLI_implicit_sharing_ptr.hh"

#include "FN_multi_function.hh"

namespace blender::fn {

class FieldInputNode;
class FieldMultiFunctionNode;

class GField {
 public:
  struct Input {
    ImplicitSharingPtr<FieldInputNode> node;

    const CPPType &cpp_type() const;
  };

  struct MultiFn {
    ImplicitSharingPtr<FieldMultiFunctionNode> node;
    int output_i = 0;

    const CPPType &cpp_type() const;
  };

  struct FieldRef {
    const GField *field_ref = nullptr;

    const CPPType &cpp_type() const;
  };

  struct ConstantRef {
    const CPPType *type = nullptr;
    const void *value = nullptr;

    const CPPType &cpp_type() const;
  };

  struct TrivialInlineConstant {
    const CPPType *type = nullptr;
    AlignedBuffer<16, 8> buffer;

    const CPPType &cpp_type() const;
  };

  struct GeneralConstant {
    const CPPType *type = nullptr;
    const void *value = nullptr;

    const CPPType &cpp_type() const;
  };

  using GFieldVariant =
      std::variant<Input, MultiFn, FieldRef, ConstantRef, TrivialInlineConstant, GeneralConstant>;

 private:
  GFieldVariant variant_;

 public:
  const CPPType &cpp_type() const;
};

class FieldInputNode : public ImplicitSharingMixin {
 protected:
  const CPPType *type_;

 public:
  FieldInputNode(const CPPType &type) : type_(&type) {}

  const CPPType &cpp_type() const
  {
    return *this->type_;
  }
};

class FieldMultiFunctionNode : public ImplicitSharingMixin {
 private:
  Vector<GField> inputs_;
  std::shared_ptr<const mf::MultiFunction> owned_fn_;
  const mf::MultiFunction *fn_;

 public:
  const CPPType &output_cpp_type(int output_i) const;

  const mf::MultiFunction &multi_function() const;
};

/* -------------------------------------------------------------------- */
/** \name Inline Methods
 * \{ */

inline const CPPType &GField::cpp_type() const
{
  return std::visit([](const auto &v) -> const CPPType & { return v.cpp_type(); }, this->variant_);
}

inline const CPPType &GField::Input::cpp_type() const
{
  return this->node->cpp_type();
}

inline const CPPType &GField::MultiFn::cpp_type() const
{
  return this->node->output_cpp_type(this->output_i);
}

inline const CPPType &GField::FieldRef::cpp_type() const
{
  return field_ref->cpp_type();
}

inline const CPPType &GField::ConstantRef::cpp_type() const
{
  return *this->type;
}

inline const CPPType &GField::TrivialInlineConstant::cpp_type() const
{
  return *this->type;
}

inline const CPPType &GField::GeneralConstant::cpp_type() const
{
  return *this->type;
}

/** \} */

}  // namespace blender::fn
