/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include <array>

#include "BLI_function_ref.hh"
#include "BLI_implicit_sharing_ptr.hh"
#include "BLI_map.hh"
#include "BLI_mutex.hh"
#include "BLI_task.hh"

namespace blender::implicit_sharing {

template<int SnapshotsNum> struct CacheKey;

template<int SnapshotsNum> struct CacheKeyRef {
  std::array<const ImplicitSharingInfo *, SnapshotsNum> inputs;

  CacheKeyRef(std::array<const ImplicitSharingInfo *, SnapshotsNum> inputs) : inputs(inputs) {}

  uint64_t hash() const
  {
    return get_default_hash(Span(this->inputs));
  }

  friend bool operator==(const CacheKeyRef &a, const CacheKeyRef &b)
  {
    return a.inputs == b.inputs;
  }
  friend bool operator==(const CacheKey<SnapshotsNum> &a, const CacheKeyRef &b)
  {
    for (const int i : IndexRange(SnapshotsNum)) {
      if (!(a.inputs[i] == b.inputs[i])) {
        return false;
      }
    }
    return true;
  }
  friend bool operator==(const CacheKeyRef &a, const CacheKey<SnapshotsNum> &b)
  {
    return b == a;
  }
};

template<int SnapshotsNum> struct CacheKey {
  std::array<WeakImplicitSharingPtr, SnapshotsNum> inputs;

  CacheKey() = default;
  CacheKey(std::array<const ImplicitSharingInfo *, SnapshotsNum> input_ptrs)
  {
    for (const int64_t i : IndexRange(inputs.size())) {
      inputs[i] = WeakImplicitSharingPtr(input_ptrs[i]);
      /* WeakImplicitSharingPtr constructor does not add a user. */
      inputs[i]->add_weak_user();
    }
  }
  CacheKey(const CacheKeyRef<SnapshotsNum> &other) : CacheKey(other.inputs) {}

  uint64_t hash() const
  {
    return get_default_hash(Span(this->inputs));
  }

  static uint64_t hash_as(const CacheKeyRef<SnapshotsNum> &key)
  {
    return get_default_hash(Span(key.inputs));
  }

  friend bool operator==(const CacheKey &a, const CacheKey &b)
  {
    return a.inputs == b.inputs;
  }

  bool is_expired() const
  {
    return std::ranges::any_of(
        this->inputs, [&](const WeakImplicitSharingPtr &ptr) { return ptr->is_expired(); });
  }
};

struct CacheBase {
  std::string debug_name_;

  CacheBase(const StringRef name);

  virtual void clear_unused() = 0;
  virtual ~CacheBase();
};

template<int KeySnapShotsNum, typename Value> class Cache : public CacheBase {
 public:
  using Key = CacheKey<KeySnapShotsNum>;
  using KeyRef = CacheKeyRef<KeySnapShotsNum>;

 private:
  struct Entry {
    Value value;
    Mutex mutex;
    std::array<std::atomic<int64_t>, KeySnapShotsNum> versions;
    Entry()
    {
      for (const int64_t i : IndexRange(versions.size())) {
        versions[i] = -1;
      }
    }
    void update_versions(const KeyRef &key)
    {
      for (const int i : IndexRange(key.inputs.size())) {
        versions[i] = key.inputs[i]->version();
      }
    }
    bool versions_match(const KeyRef &key) const
    {
      for (const int i : IndexRange(key.inputs.size())) {
        if (versions[i] != key.inputs[i]->version()) {
          return false;
        }
      }
      return true;
    }
  };

  Mutex global_mutex_;
  Map<Key, std::shared_ptr<Entry>> map_;

 public:
  Cache(const StringRef name) : CacheBase(name) {}
  ~Cache() {}

  void clear_unused() override
  {
    std::lock_guard lock{global_mutex_};
    map_.remove_if([&](const auto &item) {
      if (item.key.is_expired()) {
        return true;
      }
      return false;
    });
  }

  Value &lookup_or_compute(const KeyRef &key, const FunctionRef<Value()> create_fn)
  {
    Entry &value = *this->ensure_entry(key);
    if (value.versions_match(key)) {
      return value.value;
    }
    std::lock_guard lock{value.mutex};
    if (value.versions_match(key)) {
      return value.value;
    }
    threading::isolate_task([&]() { value.value = create_fn(); });
    value.update_versions(key);
    return value.value;
  }

  /**
   * This function provides a way to update existing cache entries even when they have different
   * implicit sharing info. This is useful for example if some operation makes a small logical
   * change to the source data but it must be reallocated if it's shared.
   *
   * \param orig_key: The cache key used to find the existing cache data to update.
   * \param key: A key reflecting the state of the data after the operation.
   * \param update_fn: This function either updates existing cached data from the old state to the
   * new state or fills in data from scratch if there is no existing cache. As an important
   * optimization, if the existing cache is no longer accessible (i.e. only used by freed data) it
   * will be moved rather than copied.
   */
  void reuse_and_update(const Key &orig_key,
                        const KeyRef &key,
                        const FunctionRef<void(Value &)> update_fn)
  {
    std::shared_ptr<Entry> *existing_entry = this->lookup_entry_ptr(orig_key);
    if (!existing_entry) {
      std::shared_ptr<Entry> &value = this->ensure_entry(key);
      if (value->versions_match(key)) {
        return;
      }
      std::lock_guard lock{value->mutex};
      if (value->versions_match(key)) {
        return;
      }
      threading::isolate_task([&]() { update_fn(value->value); });
      value->update_versions(key);
      return;
    }

    if (orig_key.is_expired() && existing_entry->use_count() == 1) {
      Entry &value = **existing_entry;
      {
        std::lock_guard lock(global_mutex_);
        map_.add_new(key, std::move(*existing_entry));
      }
      threading::isolate_task([&]() { update_fn(value.value); });
      value.update_versions(key);
      return;
    }

    std::shared_ptr<Entry> &value = this->ensure_entry(key);
    if (value->versions_match(key)) {
      return;
    }
    std::lock_guard lock(value->mutex);
    if (value->versions_match(key)) {
      return;
    }
    value->value = (*existing_entry)->value;
    threading::isolate_task([&]() { update_fn(value->value); });
    value->update_versions(key);
  }

 private:
  std::shared_ptr<Entry> *lookup_entry_ptr(const KeyRef &key)
  {
    std::lock_guard lock{global_mutex_};
    return map_.lookup_ptr_as(key);
  }
  std::shared_ptr<Entry> *lookup_entry_ptr(const Key &key)
  {
    std::lock_guard lock{global_mutex_};
    return map_.lookup_ptr(key);
  }

  std::shared_ptr<Entry> &ensure_entry(const KeyRef &key)
  {
    std::lock_guard lock{global_mutex_};
    return map_.lookup_or_add_cb_as(key, [&]() { return std::make_shared<Entry>(); });
  }

  void clear_all_keys()
  {
    map_.clear();
  }
};

void clear_unused_caches_all();

}  // namespace blender::implicit_sharing
