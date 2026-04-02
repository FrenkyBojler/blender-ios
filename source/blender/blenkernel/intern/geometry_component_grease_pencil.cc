/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_set.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_lib_id.hh"

#include "DNA_grease_pencil_types.h"

namespace blender {

namespace bke {

/* -------------------------------------------------------------------- */
/** \name Geometry Component Implementation
 * \{ */

GreasePencilComponent::GreasePencilComponent() : GeometryComponent(Type::GREASE_PENCIL) {}

GreasePencilComponent::~GreasePencilComponent()
{
  this->clear();
}

GeometryComponentPtr GreasePencilComponent::copy() const
{
  GreasePencilComponent *new_component = new GreasePencilComponent();
  if (grease_pencil_ != nullptr) {
    new_component->grease_pencil_ = BKE_grease_pencil_copy_for_eval(grease_pencil_);
    new_component->ownership_ = GeometryOwnershipType::OWNED;
  }
  return GeometryComponentPtr(new_component);
}

void GreasePencilComponent::clear()
{
  BLI_assert(this->is_mutable() || this->is_expired());
  if (grease_pencil_ != nullptr) {
    if (ownership_ == GeometryOwnershipType::OWNED) {
      BKE_id_free(nullptr, grease_pencil_);
    }
    grease_pencil_ = nullptr;
  }
}

bool GreasePencilComponent::has_grease_pencil() const
{
  return grease_pencil_ != nullptr;
}

void GreasePencilComponent::replace(GreasePencil *grease_pencil, GeometryOwnershipType ownership)
{
  BLI_assert(this->is_mutable());
  this->clear();
  grease_pencil_ = grease_pencil;
  ownership_ = ownership;
}

GreasePencil *GreasePencilComponent::release()
{
  BLI_assert(this->is_mutable());
  GreasePencil *grease_pencil = grease_pencil_;
  grease_pencil_ = nullptr;
  return grease_pencil;
}

const GreasePencil *GreasePencilComponent::get() const
{
  return grease_pencil_;
}

GreasePencil *GreasePencilComponent::get_for_write()
{
  BLI_assert(this->is_mutable());
  if (ownership_ == GeometryOwnershipType::READ_ONLY) {
    grease_pencil_ = BKE_grease_pencil_copy_for_eval(grease_pencil_);
    ownership_ = GeometryOwnershipType::OWNED;
  }
  return grease_pencil_;
}

bool GreasePencilComponent::is_empty() const
{
  return grease_pencil_ == nullptr;
}

bool GreasePencilComponent::owns_direct_data() const
{
  return ownership_ == GeometryOwnershipType::OWNED;
}

void GreasePencilComponent::ensure_owns_direct_data()
{
  BLI_assert(this->is_mutable());
  if (ownership_ != GeometryOwnershipType::OWNED) {
    if (grease_pencil_) {
      grease_pencil_ = BKE_grease_pencil_copy_for_eval(grease_pencil_);
    }
    ownership_ = GeometryOwnershipType::OWNED;
  }
}

}  // namespace bke

namespace bke {

std::optional<AttributeAccessor> GreasePencilComponent::attributes() const
{
  return AttributeAccessor(grease_pencil_, greasepencil::get_attribute_accessor_functions());
}

std::optional<MutableAttributeAccessor> GreasePencilComponent::attributes_for_write()
{
  GreasePencil *grease_pencil = this->get_for_write();
  return MutableAttributeAccessor(grease_pencil, greasepencil::get_attribute_accessor_functions());
}

}  // namespace bke
}  // namespace blender
