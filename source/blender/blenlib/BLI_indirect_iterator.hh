/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * An `blender::IndirectIterator<T>` references a range T contigous elements owned by someone else, and
 * provides a dereferenced access of this elements, this dereferenced type can be used in for loop
 * as variable type.
 *
 * For an array of std::unique_ptr<int>, this mostly like `Blender::Span<SmartPtrType>` however
 * this avoids to use unique pointer semantics where cannot be used or are not required. As
 * example, with Span a for loop must be written as follows:
 *
 * - for (const std::unique_ptr<int> &val : blender::Span<std::unique_ptr<int>>())
 *
 * Including here the complete unique_ptr type don't bring any benefits, since in the first place
 * they are inmutable, we can't pass or acquire ownership, we can just use the referenced pointer,
 * so instead with IndirectIterator the previous for loop could be simplified as:
 *
 * - for (int &val : blender::IndirectIterator<std::unique_ptr<int>>())
 *
 * Since T elements are only referenced, it is generally unsafe to store a IndirectIterator. When
 * you store one, you should know who owns the memory.
 *
 */

#include <algorithm>
#include <memory>
#include <type_traits>

#include "BLI_index_range.hh"
#include "BLI_span.hh"

namespace blender {

template<typename T> struct DereferencedType {
  using Type = T;
};

template<typename T> struct DereferencedType<T *> {
  using Type = T;
};

template<typename T> struct DereferencedType<std::unique_ptr<T>> {
  using Type = T;
};

template<typename T> struct DereferencedType<std::shared_ptr<T>> {
  using Type = T;
};

/**
 * References a range of `T` contigous elements and provides a dereferenced access to each element.
 */

template<
    /** Source type of iterable values. */
    typename T,
    /** Value type of the dereferencing `T`. */
    typename Reference = typename DereferencedType<T>::Type &>
class IndirectIterator {

 public:
   using Pointer = typename std::remove_reference_t<Reference> *;

  class Iterator {
   private:
    const T *itr_;

   public:
    constexpr Iterator() : itr_{} {}

    constexpr Iterator(const Iterator &other) : itr_{other.itr_} {}

    constexpr Iterator(const T *itr) : itr_{itr} {}

    constexpr bool operator!=(const Iterator &other) const
    {
      return itr_ != other.itr_;
    }

    constexpr bool operator==(const Iterator &other) const
    {
      return itr_ == other.itr_;
    }

    constexpr Iterator &operator++()
    {
      ++itr_;
      return *this;
    }

    constexpr Iterator operator++(int)
    {
      Iterator copy = this;
      itr_++;
      return copy;
    }

    constexpr Reference operator*()
    {
      return **itr_;
    };
  };

 private:
  const T *begin_;
  const T *end_;

 public:
  constexpr IndirectIterator() : begin_{nullptr}, end_{nullptr} {}

  IndirectIterator(const T *begin, uint64_t size) : begin_{begin}, end_{begin + size}
  {
    BLI_assert(end_ >= begin_);
  }

  constexpr IndirectIterator(const T *begin, const T *end) : begin_{begin}, end_{end}
  {
    BLI_assert(end_ >= begin_);
  }

  constexpr IndirectIterator(Span<T> span) : IndirectIterator(span.begin(), span.size()) {}

  constexpr Iterator begin() const
  {
    return begin_;
  }

  constexpr Iterator end() const
  {
    return end_;
  }

  /**
   * Returns the reference at the index.
   */
  constexpr Reference operator[](int64_t index) const
  {
    BLI_assert(index >= 0);
    BLI_assert(index < size());
    return **(begin_ + index);
  }

  /**
   * Get the amount of elements in the range.
   */
  constexpr int64_t size() const
  {
    return end_ - begin_;
  }

  /**
   * Return if the in the range has zero elements.
   */
  constexpr bool is_empty() const
  {
    return begin_ == end_;
  }

  /**
   * Return the first index of the dereferenced pointer in the range.
   */
  constexpr int64_t index_of(const Reference ref) const
  {
    const T *itr = std::find_if(begin_, end_, [&](const T &element) { return &*element == &ref; });
    BLI_assert(itr < end_);
    return itr - begin_;
  }

  /**
   * Return the pointer to the first element in the range, returns null if empty.
   */
  constexpr Pointer first() const
  {
    return this->is_empty() ? nullptr : &*begin_;
  }

  /**
   * Return the pointer to the last element in the range, returns null if empty.
   */
  constexpr Pointer last() const
  {
    return this->is_empty() ? nullptr : &*(end_ - 1);
  }

  constexpr IndexRange index_range() const
  {
    return IndexRange(size());
  }
};

}  // namespace blender
