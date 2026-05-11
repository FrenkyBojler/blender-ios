/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include "BLI_array.hh"
#include "BLI_function_ref.hh"
#include "BLI_implicit_sharing.hh"
#include "BLI_map.hh"
#include "BLI_mutex.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include <iostream>
#include <ostream>

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
  const ImplicitSharingInfo *sharing_info;
  int64_t version;

  Snapshot() = default;
  Snapshot(const SnapshotRef &ref)
      : sharing_info(ref.sharing_info), version(ref.sharing_info ? ref.sharing_info->version() : 0)
  {
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

struct CacheKey;

struct CacheKeyRef {
  Array<SnapshotRef> inputs;

  CacheKeyRef(Span<const ImplicitSharingInfo *> inputs) : inputs(inputs)
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
  friend bool operator==(const CacheKey &a, const CacheKeyRef &b);
  friend bool operator==(const CacheKeyRef &a, const CacheKey &b)
  {
    return b == a;
  }
};

struct CacheKey {
  Array<Snapshot> inputs;

  CacheKey() = default;
  CacheKey(const CacheKeyRef &other) : inputs(other.inputs.as_span()) {}

  uint64_t hash() const
  {
    return get_default_hash(this->inputs.as_span());
  }

  static uint64_t hash_as(const CacheKeyRef &key)
  {
    return get_default_hash(key.inputs.as_span());
  }

  friend bool operator==(const CacheKey &a, const CacheKey &b)
  {
    return a.inputs.as_span() == b.inputs.as_span();
  }
};

struct CacheBase {
  std::string debug_name_;

  CacheBase(const StringRef name) : debug_name_(name) {};

  virtual void clear_unused() = 0;
  virtual ~CacheBase() = default;
};

template<typename T> class Cache : public CacheBase {
  Mutex mutex_;
  Map<CacheKey, T> map_;

 public:
  Cache(const StringRef name);
  ~Cache();

  void clear_unused() override
  {
    std::lock_guard lock{mutex_};
    map_.remove_if([&](const auto &item) {
      bool found_expired_input = false;
      for (const Snapshot &snapshot : item.key.inputs) {
        if (snapshot.sharing_info->is_expired() ||
            snapshot.sharing_info->version() != snapshot.version)
        {
          found_expired_input = true;
        }
      }
      if (found_expired_input) {
        this->clear_key(item.key);
      }
      return found_expired_input;
    });
  }

  T &lookup_or_compute(const CacheKey &key, const FunctionRef<T()> create_fn)
  {
    std::lock_guard lock{mutex_};
    if (T *value = map_.lookup_ptr(key)) {
      return *value;
    }
    T new_value;
    threading::isolate_task([&]() { new_value = create_fn(); });
    map_.add_new(key, new_value);
    for (const Snapshot &snapshot : key.inputs) {
      snapshot.sharing_info->add_weak_user();
    }
    return map_.lookup(key);
  }

  void update(const CacheKey &key, const FunctionRef<void(T &)> update_fn)
  {
    std::lock_guard lock{mutex_};
    if (T *value = map_.lookup_ptr(key)) {
      update_fn(*value);
    }
  }

 private:
  void clear_key(const CacheKey &key)
  {
    std::cout << this->debug_name_ << " clearing key " << &key << std::endl;
    for (const Snapshot &snapshot : key.inputs) {
      snapshot.sharing_info->remove_weak_user_and_delete_if_last();
    }
  }
  void clear_all_keys()
  {
    for (const CacheKey &key : map_.keys()) {
      this->clear_key(key);
    }
  }
};

class CacheManager {
  Vector<CacheBase *> caches_;

 public:
  static CacheManager &instance()
  {
    static CacheManager manager;
    return manager;
  };

  CacheManager() = default;

  void register_cache(CacheBase *cache)
  {
    std::cout << "register cache " << cache->debug_name_ << std::endl;
    caches_.append(cache);
  }

  void unregister_cache(CacheBase *cache)
  {
    const int64_t index = caches_.first_index_of_try(cache);
    if (index == -1) {
      return;
    }
    std::cout << "unregister cache " << caches_[index]->debug_name_ << std::endl;
    caches_.remove_and_reorder(index);
  }

  void clear_unused_all()
  {
    for (CacheBase *cache : caches_) {

      cache->clear_unused();
    }
  }
};

template<typename T> Cache<T>::Cache(const StringRef name) : CacheBase{name}
{
  CacheManager::instance().register_cache(this);
}

template<typename T> Cache<T>::~Cache()
{
  this->clear_all_keys();
  CacheManager::instance().unregister_cache(this);
}

}  // namespace blender::implicit_sharing
