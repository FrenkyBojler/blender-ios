/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_listBase.h"

template<typename T> struct ListBaseTIterator {
 private:
  T *data_ = nullptr;

 public:
  ListBaseTIterator(T *data) : data_(data) {}

  ListBaseTIterator &operator++()
  {
    data_ = static_cast<T *>(data_->next);
    return *this;
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

template<typename T> struct ListBaseT : public ListBase {
  ListBaseTIterator<const T> begin() const
  {
    return ListBaseTIterator<const T>{static_cast<const T *>(this->first)};
  }

  ListBaseTIterator<const T> end() const
  {
    return ListBaseTIterator<const T>{static_cast<const T *>(this->last)};
  }

  ListBaseTIterator<T> begin()
  {
    return ListBaseTIterator<T>{static_cast<T *>(this->first)};
  }

  ListBaseTIterator<T> end()
  {
    return ListBaseTIterator<T>{static_cast<T *>(this->last)};
  }
};
