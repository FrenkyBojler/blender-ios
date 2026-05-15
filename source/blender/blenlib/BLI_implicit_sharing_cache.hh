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
    for (const int i : a.inputs.index_range()) {
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
  CacheKey(const CacheKeyRef<SnapshotsNum> &other)
  {
    for (const int64_t i : IndexRange(other.inputs.size())) {
      inputs[i] = WeakImplicitSharingPtr(other.inputs[i]);
      /* WeakImplicitSharingPtr constructor does not add a user. */
      inputs[i]->add_weak_user();
    }
  }

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
  Map<Key, std::unique_ptr<Entry>> map_;

 public:
  Cache(const StringRef name) : CacheBase(name) {}
  ~Cache() {}

  void clear_unused() override
  {
    std::lock_guard lock{global_mutex_};
    map_.remove_if([&](const auto &item) {
      if (std::ranges::any_of(item.key.inputs, [&](const WeakImplicitSharingPtr &ptr) {
            return ptr->is_expired();
          }))
      {
        return true;
      }
      for (const int i : IndexRange(item.key.inputs.size())) {
        if (item.value->versions[i] != item.key.inputs[i]->version()) {
          return true;
        }
      }
      return false;
    });
  }

  Value &lookup_or_compute(const KeyRef &key, const FunctionRef<Value()> create_fn)
  {
    Entry &value = this->ensure_entry(key);
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

  void update(const KeyRef &key, const FunctionRef<void(Value &)> update_fn)
  {
    Entry &value = this->ensure_entry(key);
    if (value.versions_match(key)) {
      return;
    }
    std::lock_guard lock{value.mutex};
    if (value.versions_match(key)) {
      return;
    }
    threading::isolate_task([&]() { update_fn(value.value); });
    value.update_versions(key);
  }

 private:
  Entry &ensure_entry(const KeyRef &key)
  {
    std::lock_guard lock{global_mutex_};
    return *map_.lookup_or_add_cb(key, [&]() { return std::make_unique<Entry>(); });
  }

  void clear_all_keys()
  {
    map_.clear();
  }
};

void clear_unused_caches_all();

}  // namespace blender::implicit_sharing
