/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup fn
 */

#include <concepts>

#include "BLI_math_base.hh"

#include "FN_field.hh"

namespace blender::fn {

template<typename T>
struct Polynom {
  Vector<T> factors;

  static Polynom<T> from_degree_variable(const int degree, T value)
  {
    BLI_assert(degree >= 0);
    Vector<T> factors(degree + 1, T(0));
    factors[degree] = value;
    return {std::move(factors)};
  }

  static Polynom<T> from_const(T value)
  {
    return Polynom<T>::from_degree_variable(0, std::move(value));
  }

  T value_at(const T &value) const
  {
    T sum_value = factors.first();
    T exponent = value;
    for (const int i : factors.index_range().drop_front(1)) {
      sum_value += factors[i] * exponent;
      exponent *= value;
    }
    return sum_value;
  }

  int size() const
  {
    return this->factors.size();
  }

  bool is_const() const
  {
    return this->factors.size() == 1;
  }

  int as_const() const
  {
    BLI_assert(this->is_const());
    return this->factors.first();
  }

  bool is_unit_line() const
  {
    if (this->factors.size() != 2) {
      return false;
    }
    
    return math::abs(this->factors[1]) == 1;
  }

  std::pair<int, bool> as_unit_line() const
  {
    BLI_assert(this->is_unit_line());
    return {this->factors[0], this->factors[1] < 0};
  }

  friend Polynom<T> operator +(const Polynom<T> &a, const Polynom<T> &b)
  {
    Vector<T> factors(std::max(a.factors.size(), b.factors.size()), T(0));
    for (const int i : a.factors.index_range()) {
      factors[i] = a.factors[i];
    }
    for (const int i : b.factors.index_range()) {
      factors[i] += b.factors[i];
    }
    return {std::move(factors)};
  }

  friend Polynom<T> operator -(const Polynom<T> &a, const Polynom<T> &b)
  {
    Vector<T> factors(std::max(a.factors.size(), b.factors.size()), T(0));
    for (const int i : a.factors.index_range()) {
      factors[i] = a.factors[i];
    }
    for (const int i : b.factors.index_range()) {
      factors[i] -= b.factors[i];
    }
    return {std::move(factors)};
  }

  friend Polynom<T> operator -(const Polynom<T> &value)
  {
    Vector<T> factors = value.factors;
    for (const int i : factors.index_range()) {
      factors[i] = -factors[i];
    }
    return {std::move(factors)};
  }

  friend Polynom<T> operator *(const Polynom<T> &a, const Polynom<T> &b)
  {
    Vector<T> factors(a.factors.size() + b.factors.size(), T(0));

    for (const int result_degree : factors.index_range()) {
      for (const int i : a.factors.index_range()) {
        if (b.factors.size() <= result_degree - i) {
          continue;
        }
        if (result_degree < i) {
          continue;
        }

        factors[result_degree] += a.factors[i] * b.factors[result_degree - i];
      }
    }

    return {std::move(factors)};
  }
};

std::optional<Polynom<int>> field_as_polynom_try(const Field<int> &field);

}  // namespace blender::fn
