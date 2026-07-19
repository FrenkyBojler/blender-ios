/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_sound_reader.hh"
#include "BKE_sound_sample.hh"

#include "DNA_sound_types.h"

#include "sound_reader_cache.hh"

#include "testing/testing.h"

#if defined(WITH_AUDASPACE)

#  include <array>
#  include <atomic>
#  include <barrier>
#  include <cmath>
#  include <memory>
#  include <stdexcept>
#  include <thread>

#  include <IReader.h>
#  include <ISound.h>
#  include <util/Buffer.h>
#  include <util/StreamBuffer.h>

namespace blender::bke::tests {

class TestReaderError : public std::runtime_error {
 public:
  TestReaderError() : std::runtime_error("test reader failure") {}
};

struct ReaderControl {
  enum class CreateMode {
    Normal,
    ReturnNull,
    Throw,
  };

  int readers_created = 0;
  int live_readers = 0;
  int reads = 0;
  CreateMode create_mode = CreateMode::Normal;
  bool throw_on_read = false;
};

class ControlledReader : public aud::IReader {
 private:
  std::shared_ptr<aud::IReader> reader_;
  std::shared_ptr<ReaderControl> control_;

 public:
  ControlledReader(std::shared_ptr<aud::IReader> reader, std::shared_ptr<ReaderControl> control)
      : reader_(std::move(reader)), control_(std::move(control))
  {
    control_->live_readers++;
  }

  ~ControlledReader() override
  {
    control_->live_readers--;
  }

  bool isSeekable() const override
  {
    return reader_->isSeekable();
  }
  void seek(const int position) override
  {
    reader_->seek(position);
  }
  int getLength() const override
  {
    return reader_->getLength();
  }
  int getPosition() const override
  {
    return reader_->getPosition();
  }
  aud::Specs getSpecs() const override
  {
    return reader_->getSpecs();
  }
  void read(int &length, bool &eos, aud::sample_t *buffer) override
  {
    reader_->read(length, eos, buffer);
    control_->reads++;
    if (control_->throw_on_read) {
      throw TestReaderError();
    }
  }
};

class CountingSound : public aud::ISound {
 private:
  std::shared_ptr<aud::ISound> sound_;

 public:
  std::shared_ptr<ReaderControl> control = std::make_shared<ReaderControl>();

  explicit CountingSound(std::shared_ptr<aud::ISound> sound) : sound_(std::move(sound)) {}

  std::shared_ptr<aud::IReader> createReader() override
  {
    control->readers_created++;
    switch (control->create_mode) {
      case ReaderControl::CreateMode::ReturnNull:
        return nullptr;
      case ReaderControl::CreateMode::Throw:
        throw TestReaderError();
      case ReaderControl::CreateMode::Normal:
        break;
    }
    return std::make_shared<ControlledReader>(sound_->createReader(), control);
  }
};

static std::shared_ptr<CountingSound> create_test_sound()
{
  constexpr int sample_rate = 48000;
  constexpr int samples_num = sample_rate * 4;
  auto buffer = std::make_shared<aud::Buffer>(samples_num * sizeof(aud::sample_t));
  aud::sample_t *samples = buffer->getBuffer();
  for (const int i : IndexRange(samples_num)) {
    const float time = float(i) / sample_rate;
    samples[i] = 0.5f * std::sin(6.2831853071795864769f * 440.0f * time) +
                 0.25f * std::sin(6.2831853071795864769f * 1234.0f * time);
  }
  auto stream = std::make_shared<aud::StreamBuffer>(
      buffer, aud::Specs{aud::RATE_48000, aud::CHANNELS_MONO});
  return std::make_shared<CountingSound>(std::move(stream));
}

static std::shared_ptr<SoundReaderCache> create_reader_cache(
    const std::shared_ptr<CountingSound> &sound)
{
  return SoundReaderCacheTestAccess::create(sound);
}

static std::optional<SoundReaderLease> acquire_reader(
    const std::shared_ptr<SoundReaderCache> &readers)
{
  return SoundReaderCacheTestAccess::acquire(readers);
}

class TestSoundWithReaderCache {
 public:
  bSound sound{};

  explicit TestSoundWithReaderCache(std::shared_ptr<aud::ISound> reader_sound)
  {
    SoundReaderCacheTestAccess::initialize_sound(sound, std::move(reader_sound));
  }

  ~TestSoundWithReaderCache()
  {
    SoundReaderCacheTestAccess::clear_sound(sound);
  }
};

TEST(sound_reader, SequentialAcquisitionsReuseRetainedReader)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  aud::IReader *first_reader;
  {
    std::optional<SoundReaderLease> lease = acquire_reader(readers);
    ASSERT_TRUE(lease);
    first_reader = lease->operator->();
    lease->mark_reusable();
  }
  {
    std::optional<SoundReaderLease> lease = acquire_reader(readers);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->operator->(), first_reader);
    lease->mark_reusable();
  }
  EXPECT_EQ(sound->control->readers_created, 1);
  EXPECT_EQ(sound->control->live_readers, 1);
}

TEST(sound_reader, RetainedCapacityAndTemporaryOverflow)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  std::optional<SoundReaderLease> first = acquire_reader(readers);
  std::optional<SoundReaderLease> second = acquire_reader(readers);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  aud::IReader *first_reader = first->operator->();
  aud::IReader *second_reader = second->operator->();
  {
    std::optional<SoundReaderLease> overflow = acquire_reader(readers);
    ASSERT_TRUE(overflow);
    EXPECT_NE(overflow->operator->(), first_reader);
    EXPECT_NE(overflow->operator->(), second_reader);
    overflow->mark_reusable();
  }
  EXPECT_EQ(sound->control->readers_created, 3);
  EXPECT_EQ(sound->control->live_readers, 2);

  first->mark_reusable();
  second->mark_reusable();
  first.reset();
  second.reset();
  std::optional<SoundReaderLease> reused = acquire_reader(readers);
  ASSERT_TRUE(reused);
  EXPECT_TRUE(reused->operator->() == first_reader || reused->operator->() == second_reader);
  EXPECT_EQ(sound->control->readers_created, 3);
  reused->mark_reusable();
}

TEST(sound_reader, ConcurrentCreationPublishesEachRetainedSlotOnce)
{
  class ConcurrentSound : public aud::ISound {
   private:
    std::shared_ptr<aud::ISound> sound_;
    std::barrier<> create_barrier_{3};

   public:
    std::atomic<int> readers_created = 0;

    explicit ConcurrentSound(std::shared_ptr<aud::ISound> sound) : sound_(std::move(sound)) {}

    std::shared_ptr<aud::IReader> createReader() override
    {
      readers_created.fetch_add(1, std::memory_order_relaxed);
      create_barrier_.arrive_and_wait();
      return sound_->createReader();
    }
  };

  constexpr int samples_num = 48000;
  auto buffer = std::make_shared<aud::Buffer>(samples_num * sizeof(aud::sample_t));
  auto stream = std::make_shared<aud::StreamBuffer>(
      buffer, aud::Specs{aud::RATE_48000, aud::CHANNELS_MONO});
  auto sound = std::make_shared<ConcurrentSound>(std::move(stream));
  const std::shared_ptr<SoundReaderCache> readers = SoundReaderCacheTestAccess::create(sound);
  std::array<std::optional<SoundReaderLease>, 3> leases;
  std::array<std::thread, 3> threads;
  for (const int i : IndexRange(3)) {
    threads[i] = std::thread([&, i]() { leases[i] = acquire_reader(readers); });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(sound->readers_created.load(), 3);
  for (std::optional<SoundReaderLease> &lease : leases) {
    ASSERT_TRUE(lease);
    lease->mark_reusable();
  }
  leases = {};

  std::optional<SoundReaderLease> first = acquire_reader(readers);
  std::optional<SoundReaderLease> second = acquire_reader(readers);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(sound->readers_created.load(), 3);
  first->mark_reusable();
  second->mark_reusable();
}

TEST(sound_reader, UnmarkedLeaseIsDiscardedAndReplacedLazily)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  {
    std::optional<SoundReaderLease> failed = acquire_reader(readers);
    ASSERT_TRUE(failed);
  }
  EXPECT_EQ(sound->control->live_readers, 0);
  EXPECT_EQ(sound->control->readers_created, 1);

  std::optional<SoundReaderLease> replacement = acquire_reader(readers);
  ASSERT_TRUE(replacement);
  EXPECT_EQ(sound->control->readers_created, 2);
  replacement->mark_reusable();
}

TEST(sound_reader, FailedReaderIOIsDiscarded)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  sound->control->throw_on_read = true;
  {
    std::optional<SoundReaderLease> lease = acquire_reader(readers);
    ASSERT_TRUE(lease);
    int length = 16;
    bool eos = false;
    float buffer[16];
    EXPECT_THROW((*lease)->read(length, eos, buffer), TestReaderError);
  }
  EXPECT_EQ(sound->control->live_readers, 0);
  sound->control->throw_on_read = false;
  std::optional<SoundReaderLease> replacement = acquire_reader(readers);
  ASSERT_TRUE(replacement);
  EXPECT_EQ(sound->control->readers_created, 2);
  replacement->mark_reusable();
}

TEST(sound_reader, NullAndThrowingCreationLeaveCacheUsable)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  sound->control->create_mode = ReaderControl::CreateMode::ReturnNull;
  EXPECT_FALSE(acquire_reader(readers));
  sound->control->create_mode = ReaderControl::CreateMode::Throw;
  EXPECT_THROW(acquire_reader(readers), TestReaderError);
  sound->control->create_mode = ReaderControl::CreateMode::Normal;
  std::optional<SoundReaderLease> first = acquire_reader(readers);
  std::optional<SoundReaderLease> second = acquire_reader(readers);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  first->mark_reusable();
  second->mark_reusable();
}

TEST(sound_reader, MoveConstructionAndAssignmentReleaseExactlyOnce)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  std::optional<SoundReaderLease> first = acquire_reader(readers);
  ASSERT_TRUE(first);
  aud::IReader *first_reader = first->operator->();
  first->mark_reusable();
  SoundReaderLease moved(std::move(*first));
  first.reset();

  std::optional<SoundReaderLease> second = acquire_reader(readers);
  ASSERT_TRUE(second);
  second->mark_reusable();
  moved = std::move(*second);
  second.reset();

  std::optional<SoundReaderLease> reused = acquire_reader(readers);
  ASSERT_TRUE(reused);
  EXPECT_EQ(reused->operator->(), first_reader);
  EXPECT_EQ(sound->control->readers_created, 2);
  reused->mark_reusable();
  moved.mark_reusable();
}

TEST(sound_reader, LeaseKeepsOwnerAlive)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  const std::weak_ptr<SoundReaderCache> weak_readers = readers;
  std::optional<SoundReaderLease> lease = acquire_reader(readers);
  ASSERT_TRUE(lease);
  lease->mark_reusable();
  readers.reset();
  EXPECT_FALSE(weak_readers.expired());
  lease.reset();
  EXPECT_TRUE(weak_readers.expired());
}

TEST(sound_reader, SoundAcquisitionsShareRuntimeCache)
{
  const std::shared_ptr<CountingSound> reader_sound = create_test_sound();
  TestSoundWithReaderCache sound(reader_sound);

  aud::IReader *first_reader;
  {
    std::optional<SoundReaderLease> lease = sound_reader_acquire(sound.sound);
    ASSERT_TRUE(lease);
    first_reader = lease->operator->();
    lease->mark_reusable();
  }
  {
    std::optional<SoundReaderLease> lease = sound_reader_acquire(sound.sound);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->operator->(), first_reader);
    lease->mark_reusable();
  }
  EXPECT_EQ(reader_sound->control->readers_created, 1);
}

#  if defined(WITH_FFTW3)

static float sample_at_time(const bSoundFrequencySampler &sampler, const float time)
{
  return sampler.sample(time,
                        300.0f,
                        6000.0f,
                        bSoundFrequencySampler::InterpolationMethod::CatmullRom,
                        bSoundFrequencySampler::InterpolationMethod::CatmullRom);
}

TEST(sound_sample, FrequencySamplingUsesReusableSoundReader)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const bSoundFrequencySampler::Key key = {
      bSoundFrequencySampler::WindowFunction::Hann, 4096, std::nullopt};
  bSoundFrequencySampler sampler(sound, key);
  const float first = sample_at_time(sampler, 0.5f);
  const float second = sample_at_time(sampler, 1.5f);
  EXPECT_GT(first, 0.0f);
  EXPECT_GT(second, 0.0f);
  EXPECT_EQ(sound->control->readers_created, 2);
  EXPECT_GT(sound->control->reads, 2);
}

#  endif

}  // namespace blender::bke::tests

#endif
