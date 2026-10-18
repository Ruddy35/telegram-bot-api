//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/CpuSampler.h"

#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/platform.h"
#include "td/utils/Slice.h"
#include "td/utils/Time.h"

#include <atomic>
#include <csignal>

#if TD_LINUX
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#if !defined(sigev_notify_thread_id) && defined(__GLIBC__)
#define sigev_notify_thread_id _sigev_un._tid
#endif
#endif

#if TD_LINUX && defined(SIGEV_THREAD_ID) && defined(sigev_notify_thread_id)
#define TELEGRAM_BOT_API_HAVE_CPU_SAMPLER 1
#else
#define TELEGRAM_BOT_API_HAVE_CPU_SAMPLER 0
#endif

namespace telegram_bot_api {

// the state is modified by the signal handler and by flush, which are both called only on the owning thread,
// so no synchronization except for signal fences is needed
struct CpuSampler::ThreadState {
  static constexpr int TABLE_BITS = 10;
  static constexpr std::size_t TABLE_SIZE = static_cast<std::size_t>(1) << TABLE_BITS;
  static constexpr std::size_t MAX_PROBE_COUNT = 64;

  // the key used for samples, which can't be attributed to an actor context
  static constexpr td::uint64 UNKNOWN_CONTEXT = 1;

  struct Table {
    td::uint64 keys[TABLE_SIZE];  // 0 for empty slots
    td::uint64 counts[TABLE_SIZE];
  };
  // the signal handler writes only to the active table, while flush reads and clears the other table
  Table tables_[2] = {};
  volatile std::sig_atomic_t active_table_ = 0;

#if TELEGRAM_BOT_API_HAVE_CPU_SAMPLER
  timer_t timer_{};
#endif
  bool has_timer_ = false;

  // must be async-signal-safe
  void add_samples(td::uint64 key, td::uint64 count) {
    if (key == 0) {
      key = UNKNOWN_CONTEXT;
    }
    auto &table = tables_[active_table_];
    auto hash = static_cast<std::size_t>(((key >> 4) * 0x9E3779B97F4A7C15ULL) >> (64 - TABLE_BITS));
    for (std::size_t i = 0; i < MAX_PROBE_COUNT; i++) {
      auto pos = (hash + i) & (TABLE_SIZE - 1);
      if (table.keys[pos] == key) {
        table.counts[pos] += count;
        return;
      }
      if (table.keys[pos] == 0) {
        table.keys[pos] = key;
        table.counts[pos] = count;
        return;
      }
    }
    if (key != UNKNOWN_CONTEXT) {
      add_samples(UNKNOWN_CONTEXT, count);
    }
  }
};

#if TELEGRAM_BOT_API_HAVE_CPU_SAMPLER
static void cpu_sampler_signal_handler(int signal, siginfo_t *info, void *) {
  if (info == nullptr || info->si_code != SI_TIMER) {
    // ignore signals sent by other processes
    return;
  }
  auto *state = static_cast<CpuSampler::ThreadState *>(info->si_value.sival_ptr);
  if (state == nullptr) {
    return;
  }
  td::uint64 count = 1;
  if (info->si_overrun > 0) {
    // the timer has expired more than once before the signal was delivered
    count += static_cast<td::uint64>(info->si_overrun);
  }
  // only the value of the pointer is used; the context must not be dereferenced, because it can be already destroyed
  state->add_samples(reinterpret_cast<td::uint64>(td::Scheduler::context()), count);
}
#endif

td::Status CpuSampler::init() {
#if TELEGRAM_BOT_API_HAVE_CPU_SAMPLER
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  action.sa_sigaction = cpu_sampler_signal_handler;
  if (sigaction(SIGPROF, &action, nullptr) != 0) {
    return OS_ERROR("Failed to set SIGPROF handler");
  }
  return td::Status::OK();
#else
  return td::Status::Error("CPU sampling isn't supported on the platform");
#endif
}

td::Result<CpuSampler::IoStat> CpuSampler::get_io_stat(td::int64 thread_id) {
#if TD_LINUX
  char path[64];
  if (thread_id == 0) {
    std::snprintf(path, sizeof(path), "/proc/self/io");
  } else {
    std::snprintf(path, sizeof(path), "/proc/self/task/%lld/io", static_cast<long long>(thread_id));
  }
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return OS_ERROR("Failed to open I/O statistics");
  }
  char buf[1024];
  std::size_t size = 0;
  while (size + 1 < sizeof(buf)) {
    auto read_size = ::read(fd, buf + size, sizeof(buf) - 1 - size);
    if (read_size < 0 && errno == EINTR) {
      continue;
    }
    if (read_size <= 0) {
      break;
    }
    size += static_cast<std::size_t>(read_size);
  }
  ::close(fd);

  IoStat result;
  int found_count = 0;
  for (auto line : td::full_split(td::Slice(buf, size), '\n')) {
    if (td::begins_with(line, "read_bytes: ")) {
      result.read_bytes = td::to_integer<td::int64>(line.substr(12));
      found_count++;
    } else if (td::begins_with(line, "write_bytes: ")) {
      result.written_bytes = td::to_integer<td::int64>(line.substr(13));
      found_count++;
    }
  }
  if (found_count != 2) {
    return td::Status::Error("I/O statistics are unavailable");
  }
  return result;
#else
  return td::Status::Error("I/O statistics are unsupported on the platform");
#endif
}

CpuSampler::CpuSampler(double period, Callback callback) : period_(period), callback_(std::move(callback)) {
}

CpuSampler::~CpuSampler() = default;

void CpuSampler::start_up() {
#if TELEGRAM_BOT_API_HAVE_CPU_SAMPLER
  state_ = td::make_unique<ThreadState>();

  struct sigevent event;
  std::memset(&event, 0, sizeof(event));
  event.sigev_notify = SIGEV_THREAD_ID;
  event.sigev_signo = SIGPROF;
  event.sigev_value.sival_ptr = state_.get();
  event.sigev_notify_thread_id = static_cast<pid_t>(syscall(SYS_gettid));
  if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &state_->timer_) != 0) {
    LOG(ERROR) << OS_ERROR("Failed to create CPU timer");
    return stop();
  }
  state_->has_timer_ = true;

  auto period_ns = td::max(static_cast<td::int64>(period_ * 1e9), static_cast<td::int64>(1000000));
  struct itimerspec spec;
  std::memset(&spec, 0, sizeof(spec));
  spec.it_interval.tv_sec = static_cast<time_t>(period_ns / 1000000000);
  spec.it_interval.tv_nsec = static_cast<long>(period_ns % 1000000000);
  spec.it_value = spec.it_interval;
  if (timer_settime(state_->timer_, 0, &spec, nullptr) != 0) {
    LOG(ERROR) << OS_ERROR("Failed to start CPU timer");
    return stop();
  }

  next_probe_time_ = td::Time::now() + LAG_PROBE_PERIOD;
  set_timeout_in(LAG_PROBE_PERIOD);
#else
  stop();
#endif
}

void CpuSampler::tear_down() {
#if TELEGRAM_BOT_API_HAVE_CPU_SAMPLER
  if (state_ != nullptr && state_->has_timer_) {
    // a signal, which was generated before the timer was deleted, is delivered to the thread before
    // the system call returns, therefore state_ can be safely destroyed afterwards
    timer_delete(state_->timer_);
    state_->has_timer_ = false;
  }
#endif
}

void CpuSampler::hangup() {
  stop();
}

void CpuSampler::timeout_expired() {
  auto now = td::Time::now();
  auto lag = td::max(now - next_probe_time_, 0.0);
  std::size_t bucket = 0;
  while (bucket + 1 < LAG_BUCKET_COUNT && lag > LAG_BUCKETS[bucket]) {
    bucket++;
  }
  lag_.lag_counts[bucket]++;
  lag_.lag_sum += lag;
  lag_.max_lag = td::max(lag_.max_lag, lag);

  if (++probe_count_ >= FLUSH_PROBE_COUNT) {
    probe_count_ = 0;
    flush();
  }
  next_probe_time_ = td::Time::now() + LAG_PROBE_PERIOD;
  set_timeout_in(LAG_PROBE_PERIOD);
}

void CpuSampler::flush() {
#if TELEGRAM_BOT_API_HAVE_CPU_SAMPLER
  CHECK(state_ != nullptr);
  Samples samples = lag_;
  lag_ = Samples();
  samples.scheduler_id = td::Scheduler::instance()->sched_id();
  samples.thread_id = static_cast<td::int64>(syscall(SYS_gettid));

  struct timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
    samples.thread_cpu_time = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
  }

  auto old_table_index = state_->active_table_;
  state_->active_table_ = 1 - old_table_index;
  std::atomic_signal_fence(std::memory_order_seq_cst);

  auto &table = state_->tables_[old_table_index];
  for (std::size_t pos = 0; pos < ThreadState::TABLE_SIZE; pos++) {
    if (table.keys[pos] != 0) {
      samples.contexts_.emplace_back(table.keys[pos], table.counts[pos]);
      table.keys[pos] = 0;
      table.counts[pos] = 0;
    }
  }
  std::atomic_signal_fence(std::memory_order_seq_cst);

  callback_(std::move(samples));
#endif
}

}  // namespace telegram_bot_api
