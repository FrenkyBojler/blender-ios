/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <iterator>

#include "DNA_listBase.h"

template<typename T> struct ListBaseTIterator {
 public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = T *;
  using reference = T &;

 private:
  T *data_ = nullptr;

 public:
  ListBaseTIterator(T *data) : data_(data) {}

  ListBaseTIterator &operator++()
  {
    data_ = static_cast<T *>(data_->next);
    return *this;
  }

  ListBaseTIterator operator++(int)
  {
    ListBaseTIterator tmp = *this;
    ++(*this);
    return tmp;
  }

  ListBaseTIterator &operator--()
  {
    data_ = static_cast<T *>(data_->prev);
    return *this;
  }

  ListBaseTIterator operator--(int)
  {
    ListBaseTIterator tmp = *this;
    --(*this);
    return tmp;
  }

  friend bool operator==(const ListBaseTIterator &a, const ListBaseTIterator &b)
  {
    return a.data_ == b.data_;
  }

  friend bool operator!=(const ListBaseTIterator &a, const ListBaseTIterator &b)
  {
    return a.data_ != b.data_;
  }

  T &operator*() const
  {
    return *data_;
  }
};

/**
 * This is a thin wrapper around #ListBase to make it type-safe. It's designed to be used in DNA
 * structs. It is written as untyped #ListBase in .blend files for compatibility.
 */
template<typename T> struct ListBaseT : public ListBase {
  ListBaseTIterator<const T> begin() const
  {
    return ListBaseTIterator<const T>{static_cast<const T *>(this->first)};
  }

  ListBaseTIterator<const T> end() const
  {
    /* Don't use `this->last` because this iterator has to point to one-past-the-end. */
    return ListBaseTIterator<const T>{nullptr};
  }

  ListBaseTIterator<T> begin()
  {
    return ListBaseTIterator<T>{static_cast<T *>(this->first)};
  }

  ListBaseTIterator<T> end()
  {
    /* Don't use `this->last` because this iterator has to point to one-past-the-end. */
    return ListBaseTIterator<T>{nullptr};
  }
};
