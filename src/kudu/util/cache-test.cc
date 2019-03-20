// Some portions Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "kudu/util/cache.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/gutil/ref_counted.h"
#include "kudu/util/block_cache_metrics.h"
#include "kudu/util/cache_metrics.h"
#include "kudu/util/coding.h"
#include "kudu/util/env.h"
#include "kudu/util/faststring.h"
#include "kudu/util/mem_tracker.h"
#include "kudu/util/metrics.h"
#include "kudu/util/slice.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

DECLARE_bool(cache_force_single_shard);
#if defined(HAVE_LIB_VMEM)
DECLARE_string(nvm_cache_path);
#endif // #if defined(HAVE_LIB_VMEM)

DECLARE_double(cache_memtracker_approximation_ratio);

using std::tuple;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;

namespace kudu {

// Conversions between numeric keys/values and the types expected by Cache.
static std::string EncodeInt(int k) {
  faststring result;
  PutFixed32(&result, k);
  return result.ToString();
}
static int DecodeInt(const Slice& k) {
  CHECK_EQ(4, k.size());
  return DecodeFixed32(k.data());
}

// Cache composition type: some test scenarios assume cache is single-sharded
// to keep the logic simpler.
enum class CacheComposition {
  MultiShard,
  SingleShard,
};

class CacheBaseTest : public KuduTest,
                      public Cache::EvictionCallback {
 public:
  explicit CacheBaseTest(size_t cache_size)
      : cache_size_(cache_size) {
  }

  size_t cache_size() const {
    return cache_size_;
  }

  // Implementation of the EvictionCallback interface.
  void EvictedEntry(Slice key, Slice val) override {
    evicted_keys_.push_back(DecodeInt(key));
    evicted_values_.push_back(DecodeInt(val));
  }

  int Lookup(int key) {
    Cache::Handle* handle = cache_->Lookup(EncodeInt(key), Cache::EXPECT_IN_CACHE);
    const int r = (handle == nullptr) ? -1 : DecodeInt(cache_->Value(handle));
    if (handle != nullptr) {
      cache_->Release(handle);
    }
    return r;
  }

  void Insert(int key, int value, int charge = 1) {
    std::string key_str = EncodeInt(key);
    std::string val_str = EncodeInt(value);
    Cache::PendingHandle* handle = CHECK_NOTNULL(
        cache_->Allocate(key_str, val_str.size(), charge));
    memcpy(cache_->MutableValue(handle), val_str.data(), val_str.size());

    cache_->Release(cache_->Insert(handle, this));
  }

  void Erase(int key) {
    cache_->Erase(EncodeInt(key));
  }

 protected:
  void SetupWithParameters(Cache::MemoryType mem_type,
                           CacheComposition cache_composition) {
    // Disable approximate tracking of cache memory since we make specific
    // assertions on the MemTracker in this test.
    FLAGS_cache_memtracker_approximation_ratio = 0;

    // Using single shard makes the logic of scenarios simple for capacity-
    // and eviction-related behavior.
    FLAGS_cache_force_single_shard =
        (cache_composition == CacheComposition::SingleShard);

#if defined(HAVE_LIB_VMEM)
    if (google::GetCommandLineFlagInfoOrDie("nvm_cache_path").is_default) {
      FLAGS_nvm_cache_path = GetTestPath("nvm-cache");
      ASSERT_OK(Env::Default()->CreateDir(FLAGS_nvm_cache_path));
    }
#endif // #if defined(HAVE_LIB_VMEM)

    cache_.reset(NewLRUCache(mem_type, cache_size(), "cache_test"));
    MemTracker::FindTracker("cache_test-sharded_lru_cache", &mem_tracker_);

    // Since nvm cache does not have memtracker due to the use of
    // tcmalloc for this we only check for it in the DRAM case.
    if (mem_type == Cache::MemoryType::DRAM) {
      ASSERT_TRUE(mem_tracker_.get());
    }

    scoped_refptr<MetricEntity> entity = METRIC_ENTITY_server.Instantiate(
        &metric_registry_, "test");
    unique_ptr<BlockCacheMetrics> metrics(new BlockCacheMetrics(entity));
    cache_->SetMetrics(std::move(metrics));
  }

  const size_t cache_size_;
  vector<int> evicted_keys_;
  vector<int> evicted_values_;
  shared_ptr<MemTracker> mem_tracker_;
  unique_ptr<Cache> cache_;
  MetricRegistry metric_registry_;
};

class CacheTest :
    public CacheBaseTest,
    public ::testing::WithParamInterface<tuple<Cache::MemoryType,
                                               CacheComposition>> {
 public:
  CacheTest()
      : CacheBaseTest(14 * 1024 * 1024) {
  }

  void SetUp() override {
    const auto& param = GetParam();
    SetupWithParameters(std::get<0>(param),
                        std::get<1>(param));
  }
};

#if defined(HAVE_LIB_VMEM)
INSTANTIATE_TEST_CASE_P(
    CacheTypes, CacheTest,
    ::testing::Combine(::testing::Values(Cache::MemoryType::DRAM,
                                         Cache::MemoryType::NVM),
                       ::testing::Values(CacheComposition::MultiShard,
                                         CacheComposition::SingleShard)));
#else
INSTANTIATE_TEST_CASE_P(
    CacheTypes, CacheTest,
    ::testing::Combine(::testing::Values(Cache::MemoryType::DRAM),
                       ::testing::Values(CacheComposition::MultiShard,
                                         CacheComposition::SingleShard)));
#endif // #if defined(HAVE_LIB_VMEM) ... #else ...

TEST_P(CacheTest, TrackMemory) {
  if (mem_tracker_) {
    Insert(100, 100, 1);
    ASSERT_EQ(1, mem_tracker_->consumption());
    Erase(100);
    ASSERT_EQ(0, mem_tracker_->consumption());
    ASSERT_EQ(1, mem_tracker_->peak_consumption());
  }
}

TEST_P(CacheTest, HitAndMiss) {
  ASSERT_EQ(-1, Lookup(100));

  Insert(100, 101);
  ASSERT_EQ(101, Lookup(100));
  ASSERT_EQ(-1,  Lookup(200));
  ASSERT_EQ(-1,  Lookup(300));

  Insert(200, 201);
  ASSERT_EQ(101, Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(-1,  Lookup(300));

  Insert(100, 102);
  ASSERT_EQ(102, Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(-1,  Lookup(300));

  ASSERT_EQ(1, evicted_keys_.size());
  ASSERT_EQ(100, evicted_keys_[0]);
  ASSERT_EQ(101, evicted_values_[0]);
}

TEST_P(CacheTest, Erase) {
  Erase(200);
  ASSERT_EQ(0, evicted_keys_.size());

  Insert(100, 101);
  Insert(200, 201);
  Erase(100);
  ASSERT_EQ(-1,  Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(1, evicted_keys_.size());
  ASSERT_EQ(100, evicted_keys_[0]);
  ASSERT_EQ(101, evicted_values_[0]);

  Erase(100);
  ASSERT_EQ(-1,  Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(1, evicted_keys_.size());
}

TEST_P(CacheTest, EntriesArePinned) {
  Insert(100, 101);
  Cache::Handle* h1 = cache_->Lookup(EncodeInt(100), Cache::EXPECT_IN_CACHE);
  ASSERT_EQ(101, DecodeInt(cache_->Value(h1)));

  Insert(100, 102);
  Cache::Handle* h2 = cache_->Lookup(EncodeInt(100), Cache::EXPECT_IN_CACHE);
  ASSERT_EQ(102, DecodeInt(cache_->Value(h2)));
  ASSERT_EQ(0, evicted_keys_.size());

  cache_->Release(h1);
  ASSERT_EQ(1, evicted_keys_.size());
  ASSERT_EQ(100, evicted_keys_[0]);
  ASSERT_EQ(101, evicted_values_[0]);

  Erase(100);
  ASSERT_EQ(-1, Lookup(100));
  ASSERT_EQ(1, evicted_keys_.size());

  cache_->Release(h2);
  ASSERT_EQ(2, evicted_keys_.size());
  ASSERT_EQ(100, evicted_keys_[1]);
  ASSERT_EQ(102, evicted_values_[1]);
}

// Add a bunch of light and heavy entries and then count the combined
// size of items still in the cache, which must be approximately the
// same as the total capacity.
TEST_P(CacheTest, HeavyEntries) {
  const int kLight = cache_size() / 1000;
  const int kHeavy = cache_size() / 100;
  int added = 0;
  int index = 0;
  while (added < 2 * cache_size()) {
    const int weight = (index & 1) ? kLight : kHeavy;
    Insert(index, 1000+index, weight);
    added += weight;
    index++;
  }

  int cached_weight = 0;
  for (int i = 0; i < index; i++) {
    const int weight = (i & 1 ? kLight : kHeavy);
    int r = Lookup(i);
    if (r >= 0) {
      cached_weight += weight;
      ASSERT_EQ(1000+i, r);
    }
  }
  ASSERT_LE(cached_weight, cache_size() + cache_size() / 10);
}

// This class is dedicated for scenarios specific for LRUCache.
class LRUCacheTest :
    public CacheBaseTest,
    public ::testing::WithParamInterface<tuple<Cache::MemoryType,
                                               CacheComposition>> {
 public:
  LRUCacheTest()
      : CacheBaseTest(14 * 1024 * 1024) {
  }

  void SetUp() override {
    const auto& param = GetParam();
    SetupWithParameters(std::get<0>(param),
                        std::get<1>(param));
  }
};

#if defined(HAVE_LIB_VMEM)
INSTANTIATE_TEST_CASE_P(
    CacheTypes, LRUCacheTest,
    ::testing::Combine(::testing::Values(Cache::MemoryType::DRAM,
                                         Cache::MemoryType::NVM),
                       ::testing::Values(CacheComposition::MultiShard,
                                         CacheComposition::SingleShard)));
#else
INSTANTIATE_TEST_CASE_P(
    CacheTypes, LRUCacheTest,
    ::testing::Combine(::testing::Values(Cache::MemoryType::DRAM),
                       ::testing::Values(CacheComposition::MultiShard,
                                         CacheComposition::SingleShard)));
#endif // #if defined(HAVE_LIB_VMEM) ... #else ...

TEST_P(LRUCacheTest, EvictionPolicy) {
  static constexpr int kNumElems = 1000;
  const int size_per_elem = cache_size() / kNumElems;

  Insert(100, 101);
  Insert(200, 201);

  // Loop adding and looking up new entries, but repeatedly accessing key 101.
  // This frequently-used entry should not be evicted.
  for (int i = 0; i < kNumElems + 1000; i++) {
    Insert(1000+i, 2000+i, size_per_elem);
    ASSERT_EQ(2000+i, Lookup(1000+i));
    ASSERT_EQ(101, Lookup(100));
  }
  ASSERT_EQ(101, Lookup(100));
  // Since '200' wasn't accessed in the loop above, it should have
  // been evicted.
  ASSERT_EQ(-1, Lookup(200));
}

}  // namespace kudu
