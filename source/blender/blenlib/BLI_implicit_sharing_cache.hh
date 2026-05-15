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

struct Snapshot;

struct SnapshotRef {
  const ImplicitSharingInfo *sharing_info;

  SnapshotRef() = default;
  SnapshotRef(const ImplicitSharingInfo *sharing_info) : sharing_info(sharing_info) {}
  SnapshotRef(const ImplicitSharingInfo &sharing_info) : sharing_info(&sharing_info) {}

  int64_t hash() const
  {
    return get_default_hash(this->sharing_info,
                            this->sharing_info ? this->sharing_info->version() : 0);
  }

  friend bool operator==(const SnapshotRef &a, const SnapshotRef &b)
  {
    return a.sharing_info == b.sharing_info;
  }

  friend bool operator==(const SnapshotRef &a, const Snapshot &b);
};

struct Snapshot {
  WeakImplicitSharingPtr sharing_info;
  int64_t version;

  Snapshot() = default;
  Snapshot(const SnapshotRef &ref)
      : sharing_info(ref.sharing_info), version(ref.sharing_info ? ref.sharing_info->version() : 0)
  {
    /* The constructor of WeakImplicitSharingPtr doesn't do this. */
    ref.sharing_info->add_weak_user();
  }

  uint64_t hash() const
  {
    return get_default_hash(this->sharing_info, this->version);
  }

  friend bool operator==(const Snapshot &a, const Snapshot &b)
  {
    return a.sharing_info == b.sharing_info && a.version == b.version;
  }
};

template<int SnapshotsNum> struct CacheKey;

template<int SnapshotsNum> struct CacheKeyRef {
  std::array<SnapshotRef, SnapshotsNum> inputs;

  CacheKeyRef(std::array<SnapshotRef, SnapshotsNum> inputs) : inputs(inputs)
  {
    // for (const int i : inputs.index_range()) {
    //   std::cout << inputs[i] << std::endl;
    // }
  }

  uint64_t hash() const
  {
    return get_default_hash(this->inputs.as_span());
  }

  friend bool operator==(const CacheKeyRef &a, const CacheKeyRef &b)
  {
    return a.inputs.as_span() == b.inputs.as_span();
  }
  friend bool operator==(const CacheKey<SnapshotsNum> &a, const CacheKeyRef &b)
  {
    if (a.inputs.size() != b.inputs.size()) {
      return false;
    }
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
  std::array<Snapshot, SnapshotsNum> inputs;

  CacheKey() = default;
  CacheKey(const CacheKeyRef<SnapshotsNum> &other)
  {
    for (const int64_t i : IndexRange(other.inputs.size())) {
      inputs[i] = other.inputs[i];
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
    std::atomic<bool> created = false;
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
      return std::ranges::any_of(item.key.inputs, [&](const Snapshot &snapshot) {
        return snapshot.sharing_info->is_expired() ||
               snapshot.sharing_info->version() != snapshot.version;
      });
    });
  }

  Value &lookup_or_compute(const KeyRef &key, const FunctionRef<Value()> create_fn)
  {
    Entry *value;
    {
      std::lock_guard lock{global_mutex_};
      value = map_.lookup_or_add_cb(key, [&]() { return std::make_unique<Entry>(); }).get();
    }
    if (value->created) {
      return value->value;
    }
    std::lock_guard lock{value->mutex};
    if (value->created) {
      return value->value;
    }
    threading::isolate_task([&]() { value->value = create_fn(); });
    return value->value;
  }

  void update(const KeyRef &key, const FunctionRef<void(Value &)> update_fn)
  {
    Entry *value;
    {
      std::lock_guard lock{global_mutex_};
      value = map_.lookup_or_add_cb(key, [&]() { return std::make_unique<Entry>(); }).get();
    }
    // TODO: Is a lock here necessary?
    // Yes: Another thread might be referencing the cache at the same time? What if the cache data
    // is reallocated and that reference becomes invalid?
    // No: We should just have one unique owner of data that's actively being changed, so the
    // cached data shouldn't be referenced elsewhere. If the source data is actually changed, the
    // key will be different anyway. Basically, higher level "unsharing" (copy-on-write) keeps this
    // working.
    // TODO: Convinced by "No", got to write it in a comment though.
    std::lock_guard lock{value->mutex};
    threading::isolate_task([&]() { update_fn(value->value); });
  }

 private:
  void clear_all_keys()
  {
    map_.clear();
  }
};

void clear_unused_caches_all();

}  // namespace blender::implicit_sharing
