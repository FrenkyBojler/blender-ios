/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_any.hh"

namespace blender {

namespace detail {

template<typename T>
  requires std::is_polymorphic_v<T>
struct AnyVirtualExtraInfo {

  T *(*get_impl)(const void *buffer);

  template<typename StorageT> static constexpr AnyVirtualExtraInfo get()
  {
    /* These are the only allowed types in the #Any. */
    static_assert(std::is_base_of_v<T, StorageT> ||
                  is_same_any_v<StorageT, T *, std::shared_ptr<T>>);

    /* Depending on how the implementation is stored in the #Any, a different #get_varray function
     * is required. */
    if constexpr (std::is_base_of_v<T, StorageT>) {
      return {[](const void *buffer) {
        return static_cast<T *>(const_cast<StorageT *>(static_cast<const StorageT *>(buffer)));
      }};
    }
    else if constexpr (std::is_same_v<StorageT, T *>) {
      return {[](const void *buffer) {
        return *const_cast<StorageT *>(static_cast<const StorageT *>(buffer));
      }};
    }
    else if constexpr (std::is_same_v<StorageT, std::shared_ptr<T>>) {
      return {[](const void *buffer) {
        return (const_cast<StorageT *>(static_cast<const StorageT *>(buffer)))->get();
      }};
    }
    else {
      BLI_assert_unreachable();
      return {};
    }
  }
};

}  // namespace detail

template<typename T, int64_t InlineBufferCapacity = 24>
  requires std::is_polymorphic_v<T>
struct AnyVirtual {
 private:
  using Storage = Any<detail::AnyVirtualExtraInfo<T>, InlineBufferCapacity, alignof(T)>;

  /** Pointer to the currently contained implementation. This may be null. */
  T *impl_ = nullptr;

  /**
   * Does the memory management for the implementation. It contains one of the following:
   * - Inline subclass of T.
   * - Non-owning pointer to a T.
   * - Shared pointer to a T.
   */
  Storage storage_;

 public:
  AnyVirtual() = default;

  AnyVirtual(const AnyVirtual &other) : storage_(other.storage_)
  {
    impl_ = this->impl_from_storage();
  }

  AnyVirtual(AnyVirtual &&other) noexcept : storage_(std::move(other.storage_))
  {
    impl_ = this->impl_from_storage();
    other.storage_.reset();
    other.impl_ = nullptr;
  }

  explicit AnyVirtual(T *impl) : impl_(impl)
  {
    storage_ = impl_;
  }

  explicit AnyVirtual(std::shared_ptr<T> impl) : impl_(impl.get())
  {
    if (impl_) {
      storage_ = std::move(impl);
    }
  }

  AnyVirtual &operator=(const AnyVirtual<T> &other)
  {
    if (this == &other) {
      return *this;
    }
    storage_ = other.storage_;
    impl_ = this->impl_from_storage();
    return *this;
  }

  AnyVirtual &operator=(AnyVirtual<T> &&other) noexcept
  {
    if (this == &other) {
      return *this;
    }
    storage_ = std::move(other.storage_);
    impl_ = this->impl_from_storage();
    other.storage_.reset();
    other.impl_ = nullptr;
    return *this;
  }

  template<typename ImplT, typename... Args>
    requires std::is_base_of_v<T, ImplT>
  void emplace(Args &&...args)
  {
    if constexpr (std::is_copy_constructible_v<ImplT> && Storage::template is_inline_v<ImplT>) {
      /* Only inline the implementation when it is copyable and when it fits into the inline
       * buffer of the storage. */
      impl_ = &storage_.template emplace<ImplT>(std::forward<Args>(args)...);
    }
    else {
      /* If it can't be inlined, create a new #std::shared_ptr instead and store that in the
       * storage. */
      std::shared_ptr<T> ptr = std::make_shared<ImplT>(std::forward<Args>(args)...);
      impl_ = &*ptr;
      storage_ = std::move(ptr);
    }
  }

  operator bool() const
  {
    return impl_ != nullptr;
  }

  T &operator*() const
  {
    return *impl_;
  }

  T *operator->() const
  {
    return impl_;
  }

  T *get() const
  {
    return impl_;
  }

 protected:
  T *impl_from_storage() const
  {
    if (!storage_.has_value()) {
      return nullptr;
    }
    return storage_.extra_info().get_impl(storage_.get());
  }
};

}  // namespace blender
