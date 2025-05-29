#include "tracing_internal.hh"
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

static std::chrono::steady_clock::time_point profile_start = std::chrono::steady_clock::now();
static std::map<size_t, int> thread_table;
static std::atomic<int> cur_thread = 1;
static std::mutex thread_table_lock;

size_t lazytrace::internal::CurrentThreadID()
{
  size_t thread_hash = std::hash<std::thread::id>()(std::this_thread::get_id());
  if (!thread_table.count(thread_hash)) {
    thread_table_lock.lock();
    if (!thread_table.count(thread_hash)) {
      thread_table[thread_hash] = cur_thread++;
    }
    thread_table_lock.unlock();
  }
  return thread_table[thread_hash];
}

size_t lazytrace::internal::CurrentProcessID()
{
  return 1;
}

size_t lazytrace::internal::CurrentClock()
{
  std::chrono::steady_clock::time_point tp = std::chrono::steady_clock::now();
  const std::chrono::microseconds stamp = std::chrono::duration_cast<std::chrono::microseconds>(
      tp - profile_start);
  return static_cast<size_t>(stamp.count());
}
