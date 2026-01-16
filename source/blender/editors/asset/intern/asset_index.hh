/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#pragma once

#include "ED_asset_indexer.hh"

#include <memory>
#include <variant>

struct AssetMetaData;
namespace blender {
class StringRefNull;
}  // namespace blender
namespace blender::io::serialize {
class DictionaryValue;
class Value;
}  // namespace blender::io::serialize

namespace blender::ed::asset::index {

struct RemoteListingAssetEntry;

std::unique_ptr<io::serialize::Value> read_contents(StringRefNull filepath);

AssetMetaData *asset_metadata_from_dictionary(const io::serialize::DictionaryValue &entry);

/**
 * Result of reading a remote listing.
 *
 * A succesful result can have a value of type T. A failure can have a 'reason' string.
 */
template<typename T = std::monostate> class ReadingResult {
 public:
  enum class Type {
    Success,
    Failure,
    Cancelled,
  };
  Type type;
  std::string failure_reason;
  std::optional<T> success_value;

  /**
   * Construct a valueless success result.
   * Only enabled if T == std::monostate.
   */
  template<typename U = T, typename = std::enable_if_t<std::is_same_v<U, std::monostate>>>
  static ReadingResult Success()
  {
    return ReadingResult(Type::Success);
  }

  /**
   * Construct a valued success result.
   * Only enabled if T != std::monostate.
   */
  template<typename U = T, typename = std::enable_if_t<!std::is_same_v<U, std::monostate>>>
  static ReadingResult Success(T value)
  {
    ReadingResult result(Type::Success);
    result.success_value = std::move(value);
    return result;
  }

  /**
   * Construct a failure result.
   * The ReadingResult copies the failure reason, so the StringRef can refer to temporary data.
   *
   * NOTE: Don't forget to wrap the string in N_(...) for translation tagging.
   */
  static ReadingResult Failure(const StringRef failure_reason)
  {
    ReadingResult result(Type::Failure);
    result.failure_reason = failure_reason;
    return result;
  }
  /**
   * Construct a cancelled result.
   */
  static ReadingResult Cancelled()
  {
    return ReadingResult(Type::Cancelled);
  }

  bool is_success() const
  {
    return this->type == Type::Success;
  }
  bool is_failure() const
  {
    return this->type == Type::Failure;
  }
  bool is_cancelled() const
  {
    return this->type == Type::Cancelled;
  }

  /**
   * Get the result's success value, as if this is an `std::optional`.
   * Only valid if this result is successful.
   */
  T &operator*()
  {
    if (!is_success() || !success_value.has_value()) {
      throw std::runtime_error("Attempted to access value of non-success ReadingResult");
    }
    return *success_value;
  }

  /**
   * Get the result's success value, as if this is an `std::optional`.
   * Only valid if this result is successful.
   */
  const T &operator*() const
  {
    if (!is_success() || !success_value.has_value()) {
      throw std::runtime_error("Attempted to access value of non-success ReadingResult");
    }
    return *success_value;
  }

  /**
   * Get the result's success value, as if this is an `std::optional`.
   * Only valid if this result is successful.
   */
  T *operator->()
  {
    return &**this;
  }

  /**
   * Get the result's success value, as if this is an `std::optional`.
   * Only valid if this result is successful.
   */
  const T *operator->() const
  {
    return &**this;
  }

  /**
   * Conversion constructor from any other ReadingResult.
   */
  template<typename U> ReadingResult(const ReadingResult<U> &other)
  {
    this->type = static_cast<ReadingResult<T>::Type>(other.type);
    if (this->type == Type::Success) {
      // Only allow if U is std::monostate.
      static_assert(std::is_same<U, std::monostate>::value,
                    "Cannot convert a valued success to another type");
      success_value.reset();
    }
    else {
      // Failure or Cancelled can convert freely.
      failure_reason = other.failure_reason;
    }
  }

 private:
  explicit ReadingResult(Type type) : type(type) {}
};

std::optional<bool> file_older_than_timestamp(const char *filepath, Timestamp timestamp);

/**
 * Reading of API schema version 1. See #read_remote_listing() on \a process_fn.
 * \param version_root_dirpath: Absolute path to the remote listing root directory.
 */
ReadingResult<Vector<std::string>> read_remote_listing_v1(
    StringRefNull listing_root_dirpath,
    RemoteListingEntryProcessFn process_fn,
    RemoteListingWaitForPagesFn wait_fn = nullptr,
    const std::optional<Timestamp> ignore_before_timestamp = std::nullopt);

}  // namespace blender::ed::asset::index
