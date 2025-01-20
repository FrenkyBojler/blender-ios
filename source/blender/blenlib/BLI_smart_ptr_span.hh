/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * An `blender::SmartPtrSpan<SmartPtrType>` references an array of smart managed
 * T pointers owned by someone else, and provides an non owning access to the pointer that each
 * `SmartPtrType` array entry references.
 *
 * This mostly like `Blender::Span<SmartPtrType>` however this avoids to use smart pointer
 * semantics where cannot be used or are not required. As example, with Span for loop must be
 * written as follows:
 *
 * - for (const std::unique_ptr<int> &val : blender::Span<std::unique_ptr<int>>())
 *
 * Including here the complete unique_ptr type don't bring any benefits, since in the first place
 * they are inmutable, we can't pass or acquire ownership, we can just use the referenced pointer,
 * so instead with SmartPtrSpan the previous for loop could be simplified as:
 *
 * - for (int *val : blender::SmartPtrSpan<std::unique_ptr<int>>())
 *
 * Since the arrays are only referenced, it is generally unsafe to store a SmartPtrSpan. When you
 * store one, you should know who owns the memory.
 *
 */

#include <algorithm>
#include <memory>

#include "BLI_index_range.hh"
#include "BLI_span.hh"

namespace blender {

/**
 * References an array of smart managed T pointers owned by someone else and provides non
 * owning access to each pointer. Smart pointers cannot be modified, but referenced `T` values can
 * be modified if T is non-const.
 */
template<typename SmartPtrType> class SmartPtrSpan {

 public:
  using element_type = typename SmartPtrType::element_type;
  using T = element_type;

  using smart_ptr_type = SmartPtrType;

  struct SmartPtrWrapper {
    const SmartPtrType *ptr_;
    constexpr SmartPtrWrapper(const SmartPtrType *ptr) : ptr_{ptr}
    {
      BLI_assert(ptr_);
    }

    constexpr operator T *() const
    {
      return ptr_->get();
    };

    constexpr operator T &() const
    {
      return **ptr_;
    };

    constexpr operator const SmartPtrType &() const
    {
      return *ptr_;
    };

    constexpr T *operator->() const
    {
      return ptr_->get();
    }
  };

  class Iterator {
   private:
    const SmartPtrType *ptr_;

   public:
    constexpr Iterator() : ptr_{nullptr} {}

    constexpr Iterator(const Iterator &other) : ptr_{other.ptr_} {}

    constexpr Iterator(const SmartPtrType *ptr) : ptr_{ptr} {}

    constexpr bool operator!=(const Iterator &other) const
    {
      return ptr_ != other.ptr_;
    }

    constexpr bool operator==(const Iterator &other) const
    {
      return ptr_ == other.ptr_;
    }

    constexpr Iterator &operator++()
    {
      ++ptr_;
      return *this;
    }

    constexpr Iterator operator++(int)
    {
      Iterator copy = this;
      ptr_++;
      return copy;
    }

    constexpr SmartPtrWrapper operator*()
    {
      return ptr_;
    };
  };

 private:
  const SmartPtrType *begin_;
  const SmartPtrType *end_;

 public:
  constexpr SmartPtrSpan() : begin_{nullptr}, end_{nullptr} {}

  constexpr SmartPtrSpan(const SmartPtrType *data, uint64_t size) : begin_{data}, end_{data + size}
  {
    BLI_assert(end_ >= begin_);
  }

  constexpr SmartPtrSpan(Span<SmartPtrType> span) : SmartPtrSpan(span.begin(), span.size()) {}

  constexpr Iterator begin() const
  {
    return begin_;
  }

  constexpr Iterator end() const
  {
    return end_;
  }

  /**
   * Returns the pointer at the index.
   */
  constexpr SmartPtrWrapper operator[](int64_t index) const
  {
    BLI_assert(index >= 0);
    BLI_assert(index < size());
    return begin_ + index;
  }

  /**
   * Get the amount of managed pointers in the array.
   */
  constexpr int64_t size() const
  {
    return end_ - begin_;
  }

  /**
   * Return if the array has zero elements.
   */
  constexpr bool is_empty() const
  {
    return begin_ == end_;
  }

  /**
   * Return the index of the pointer in the array, if `SmartPtrType` can share
   * pointers, returns the first index of the pointer in the array. This can cause undefined
   * behavior if the pointer is not in the array.
   */
  constexpr int64_t index_of(const T *ptr) const
  {
    const SmartPtrType *itr = std::find_if(
        begin_, end_, [ptr](const SmartPtrType &element) { return element.get() == ptr; });
    BLI_assert(itr < end_);
    return itr - begin_;
  }

  /**
   * Return the first pointer in the array or null if empty.
   */
  constexpr T *first() const
  {
    return this->is_empty() ? nullptr : begin_->get();
  }

  /**
   * Return the last pointer in the array or null if empty.
   */
  constexpr T *last() const
  {
    return this->is_empty() ? nullptr : (end_ - 1)->get();
  }

  constexpr IndexRange index_range() const
  {
    return IndexRange(size());
  }
};

}  // namespace blender
