/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#pragma once

#include <algorithm>

#include "BLI_vector.hh"

namespace blender {

/**
 * A VectorList is a vector of vectors.
 *
 * VectorList can be used when:
 *
 * 1) Don't know up front the number of elements that will be added to the list. Use array or
 * vector.reserve when known up front.
 *
 * 2) Number of reads/writes doesn't require sequential access
 * of the whole list. A vector ensures memory is sequential which is fast when reading, writing can
 * have overhead when the reserved memory is full.
 *
 * When a VectorList reserved memory is full it will allocate memory for the new items, breaking
 * the sequential access. Within each allocated memory block the elements are ordered sequentially.
 */
template<typename T, int64_t CapacityStart = 32, int64_t CapacitySoftLimit = 4096>
class VectorList {
  using SelfT = VectorList<T, CapacityStart, CapacitySoftLimit>;
  using UsedVector = Vector<T, 0>;

  /**
   * Contains the individual vectors. There must always be at least one vector
   */
  Vector<UsedVector> vectors_;
  /* Total element count accross all vectors_. */
  int64_t size_ = 0;

 public:
  VectorList()
  {
    this->append_vector();
  }

  void append(const T &value)
  {
    this->append_as(value);
  }

  void append(T &&value)
  {
    this->append_as(std::move(value));
  }

  template<typename ForwardT> void append_as(ForwardT &&value)
  {
    UsedVector &vector = this->ensure_space_for_one();
    vector.append_unchecked_as(std::forward<ForwardT>(value));
    size_++;
  }

  T &last()
  {
    return vectors_.last().last();
  }

  int64_t size() const
  {
    return size_;
  }

 private:
  UsedVector &ensure_space_for_one()
  {
    UsedVector &vector = vectors_.last();
    if (LIKELY(!vector.is_at_capacity())) {
      return vector;
    }
    this->append_vector();
    return vectors_.last();
  }

  void append_vector()
  {
    const int64_t new_vector_capacity = this->get_next_vector_capacity();
    vectors_.append({});
    vectors_.last().reserve(new_vector_capacity);
  }

  int64_t get_next_vector_capacity()
  {
    if (vectors_.is_empty()) {
      return CapacityStart;
    }
    return std::min(vectors_.last().capacity() * 2, CapacitySoftLimit);
  }

  template<typename IterableT, typename ElemT> struct Iterator {
    IterableT &vector_list;
    int64_t index_a = 0;
    int64_t index_b = 0;

    Iterator(IterableT &vector_list, int64_t index_a = 0, int64_t index_b = 0)
        : vector_list(vector_list), index_a(index_a), index_b(index_b)
    {
    }

    ElemT &operator*() const
    {
      return vector_list.vectors_[index_a][index_b];
    }

    Iterator &operator++()
    {
      if (vector_list.vectors_[index_a].capacity() == index_b + 1) {
        index_a++;
        index_b = 0;
      }
      else {
        index_b++;
      }
      return *this;
    }

    bool operator==(const Iterator &other) const
    {
      return &other.vector_list == &vector_list && other.index_a == index_a &&
             other.index_b == index_b;
    }

    bool operator!=(const Iterator &other) const
    {
      return !(other == *this);
    }
  };

  using MutIterator = Iterator<SelfT, T>;
  using ConstIterator = Iterator<const SelfT, const T>;

 public:
  MutIterator begin()
  {
    return MutIterator(*this, 0, 0);
  }
  MutIterator end()
  {
    return MutIterator(*this, vectors_.size(), vectors_.last().size());
  }

  ConstIterator begin() const
  {
    return ConstIterator(*this, 0, 0);
  }
  ConstIterator end() const
  {
    return ConstIterator(*this, vectors_.size(), vectors_.last().size());
  }
};

}  // namespace blender
