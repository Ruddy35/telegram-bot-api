//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/actor/actor.h"

#include "td/utils/common.h"
#include "td/utils/Status.h"

#include <functional>
#include <utility>

namespace telegram_bot_api {

// Statistical profiler, which estimates CPU time consumed by actors of each actor context.
// Every scheduler thread has a timer, which interrupts the thread after each `period` seconds of its CPU time.
// The signal handler only increments the number of samples of the current actor context of the thread.
// Samples are periodically collected by the thread itself and passed to the callback on the same thread.
class CpuSampler final : public td::Actor {
 public:
  // lag of the scheduler is the delay of a timer event, during which the thread was busy with something else
  static constexpr std::size_t LAG_BUCKET_COUNT = 7;
  static constexpr double LAG_BUCKETS[LAG_BUCKET_COUNT - 1] = {0.001, 0.005, 0.01, 0.05, 0.1, 0.5};

  struct Samples {
    td::uint64 lag_counts[LAG_BUCKET_COUNT] = {};  // since the previous samples
    double lag_sum = 0.0;
    double max_lag = 0.0;
    td::int32 scheduler_id = 0;
    td::int64 thread_id = 0;                                  // identifier of the thread in the OS
    double thread_cpu_time = 0.0;                             // total CPU time consumed by the thread
    td::vector<std::pair<td::uint64, td::uint64>> contexts_;  // context pointer -> number of samples
  };
  using Callback = std::function<void(Samples)>;

  // must be called once before creation of the first CpuSampler
  static td::Status init();

  struct IoStat {
    td::int64 read_bytes = 0;     // bytes read from the storage
    td::int64 written_bytes = 0;  // bytes written to the storage or to the page cache
  };
  // returns storage I/O statistics of a thread of the process, or of the whole process if thread_id == 0
  static td::Result<IoStat> get_io_stat(td::int64 thread_id);

  CpuSampler(double period, Callback callback);
  CpuSampler(const CpuSampler &) = delete;
  CpuSampler &operator=(const CpuSampler &) = delete;
  CpuSampler(CpuSampler &&) = delete;
  CpuSampler &operator=(CpuSampler &&) = delete;
  ~CpuSampler() final;

  struct ThreadState;

 private:
  static constexpr double LAG_PROBE_PERIOD = 0.05;
  static constexpr int FLUSH_PROBE_COUNT = 20;  // samples are flushed every second

  double period_;
  Callback callback_;
  td::unique_ptr<ThreadState> state_;
  double next_probe_time_ = 0.0;
  int probe_count_ = 0;
  Samples lag_;

  void start_up() final;

  void tear_down() final;

  void timeout_expired() final;

  void hangup() final;

  void flush();
};

}  // namespace telegram_bot_api
