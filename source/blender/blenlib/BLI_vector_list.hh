/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#pragma once

#include <algorithm>
#include <cmath>

#include "BLI_math_bits.h"
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

  static constexpr bool is_power_of_2(int64_t value)
  {
    return (value > 0) && ((value & (value - 1)) == 0);
  }
  static_assert(is_power_of_2(CapacityStart));
  static_assert(is_power_of_2(CapacitySoftLimit));

  /* Contains the individual vectors. There must always be at least one vector. */
  Vector<UsedVector> vectors_;
  /* Number of vectors in use. */
  int64_t used_vectors_ = 0;
  /* Total element count accross all vectors_. */
  int64_t size_ = 0;

 public:
  VectorList()
  {
    this->append_vector();
    used_vectors_ = 1;
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

  T &first()
  {
    BLI_assert(size() > 0);
    return vectors_.first().first();
  }

  T &last()
  {
    BLI_assert(size() > 0);
    return vectors_[used_vectors_ - 1].last();
  }

  int64_t size() const
  {
    return size_;
  }

  bool is_empty() const
  {
    return size_ == 0;
  }

  void clear()
  {
    for (UsedVector &vector : vectors_) {
      vector.clear();
    }
    used_vectors_ = 1;
    size_ = 0;
  }

  /**
   * Get the value at the given index. This invokes undefined behavior when the index is out of
   * bounds.
   */
  const T &operator[](int64_t index) const
  {
    BLI_assert(index >= 0);
    BLI_assert(index < this->size());
    std::pair<int64_t, int64_t> index_pair = global_index_to_index_pair(index);
    return vectors_[index_pair.first][index_pair.second];
  }

  T &operator[](int64_t index)
  {
    BLI_assert(index >= 0);
    BLI_assert(index < this->size());
    std::pair<int64_t, int64_t> index_pair = global_index_to_index_pair(index);
    return vectors_[index_pair.first][index_pair.second];
  }

 private:
  std::pair<int64_t, int64_t> global_index_to_index_pair(int64_t index)
  {
    auto log2 = [](int64_t value) -> int64_t {
      return 31 - bitscan_reverse_uint(uint32_t(value));
    };
    auto geometric_sum = [](int64_t index) -> int64_t {
      return CapacityStart * ((2 << index) - 1);
    };
    auto index_from_sum = [log2](int64_t sum) -> int64_t {
      return log2((sum / CapacityStart) + 1);
    };
    static const int64_t start_log2 = log2(CapacityStart);
    static const int64_t end_log2 = log2(CapacitySoftLimit);
    /* The number of vectors until CapacitySoftLimit size is reached. */
    static const int64_t geometric_steps = end_log2 - start_log2 + 1;
    /* The number of elements until CapacitySoftLimit size is reached. */
    static const int64_t geometric_total = geometric_sum(geometric_steps - 1);

    int64_t index_a, index_b;
    if (index < geometric_total) {
      index_a = index_from_sum(index);
      index_b = index_a > 0 ? index - geometric_sum(index_a - 1) : index;
    }
    else {
      int64_t linear_start = index - geometric_total;
      index_a = geometric_steps + linear_start / CapacitySoftLimit;
      index_b = linear_start % CapacitySoftLimit;
    }
    return {index_a, index_b};
  }

  UsedVector &ensure_space_for_one()
  {
    if (vectors_[used_vectors_ - 1].is_at_capacity()) {
      size_t capacity = vectors_.size();
      if (used_vectors_ == capacity) {
        append_vector();
      }
      used_vectors_++;
    }
    return vectors_[used_vectors_ - 1];
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
      if (vector_list.vectors_[index_a].size() == index_b + 1) {
        if (index_a + 1 == vector_list.vectors_.size()) {
          /* Reached the end. */
          index_b++;
        }
        else {
          index_a++;
          index_b = 0;
        }
      }
      else {
        index_b++;
      }
      return *this;
    }

    bool operator==(const Iterator &other) const
    {
      BLI_assert(&other.vector_list == &vector_list);
      return other.index_a == index_a && other.index_b == index_b;
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
    return MutIterator(*this, used_vectors_ - 1, vectors_[used_vectors_ - 1].size());
  }

  ConstIterator begin() const
  {
    return ConstIterator(*this, 0, 0);
  }
  ConstIterator end() const
  {
    return ConstIterator(*this, used_vectors_ - 1, vectors_[used_vectors_ - 1].size());
  }
};

}  // namespace blender
