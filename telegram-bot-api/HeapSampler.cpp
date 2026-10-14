//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/HeapSampler.h"

#include "td/actor/actor.h"

#include "td/utils/port/platform.h"

#if defined(TELEGRAM_BOT_API_HEAP_SAMPLING) && TD_LINUX && defined(__GLIBC__)
#define TELEGRAM_BOT_API_HEAP_SAMPLER_ENABLED 1
#else
#define TELEGRAM_BOT_API_HEAP_SAMPLER_ENABLED 0
#endif

#if TELEGRAM_BOT_API_HEAP_SAMPLER_ENABLED
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dlfcn.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace telegram_bot_api {

#if TELEGRAM_BOT_API_HEAP_SAMPLER_ENABLED
namespace {

// Everything here can be called from any allocation function, so nothing may allocate memory with malloc

constexpr std::size_t HEAP_PAGE_SIZE = 4096;
constexpr std::uintptr_t REGION_SIZE = static_cast<std::uintptr_t>(1) << 38;  // address space for sampled blocks
constexpr td::uint32 REGION_PAGE_COUNT = static_cast<td::uint32>(REGION_SIZE / HEAP_PAGE_SIZE);
constexpr std::size_t MAX_SAMPLED_SIZE = REGION_SIZE / 4;
constexpr std::size_t SMALL_HEADER_SIZE = 32;  // SmallHeader, padded to keep 16-byte alignment
constexpr std::size_t MAX_SMALL_SLOT_SIZE = 2048;
constexpr std::size_t SMALL_CLASS_COUNT = MAX_SMALL_SLOT_SIZE / 16 + 1;
constexpr td::int32 RUN_CLASS_COUNT = 40;
constexpr td::uint32 NO_PAGE = 0xFFFFFFFFu;
constexpr td::int64 DISABLED_RECHECK_BYTES = 1 << 20;
constexpr td::int64 DEFAULT_SAMPLING_INTERVAL = static_cast<td::int64>(64) << 10;
// sampled blocks of at least this size are mapped with commit accounting; glibc uses mmap for blocks of this size too
constexpr std::size_t COMMIT_THRESHOLD = static_cast<std::size_t>(32) << 20;
// freed page runs of up to 1 MB are kept for reuse without returning their memory to the OS, while their total size is
// at most RUN_CACHE_LIMIT; madvise is slow and interrupts other CPUs to flush their TLBs
constexpr td::uint32 MAX_CACHED_RUN_CLASS = 8;
constexpr td::int64 RUN_CACHE_LIMIT = static_cast<td::int64>(8) << 20;
constexpr td::int32 MAX_OWNERS = 1 << 16;
constexpr std::size_t CONTEXT_TABLE_SIZE = 1 << 16;
constexpr td::uint64 DELETED_CONTEXT = 1;
constexpr std::size_t MAX_THREAD_COUNTERS = 4096;
constexpr std::size_t BOOTSTRAP_SIZE = 1 << 20;
constexpr std::size_t AUDIT_HEADER_SIZE = 16;
constexpr std::size_t MAX_AUDIT_THREADS = 1024;
constexpr std::size_t AUDIT_TALLY_SIZE = static_cast<std::size_t>(MAX_OWNERS) * HeapSampler::PART_COUNT;
constexpr std::size_t CONTEXT_CACHE_SIZE = 64;

struct RealAllocator {
  void *(*malloc_)(std::size_t) = nullptr;
  void (*free_)(void *) = nullptr;
  void *(*calloc_)(std::size_t, std::size_t) = nullptr;
  void *(*realloc_)(void *, std::size_t) = nullptr;
  void *(*memalign_)(std::size_t, std::size_t) = nullptr;
  std::size_t (*malloc_usable_size_)(void *) = nullptr;
  std::size_t (*nallocx_)(std::size_t, int) = nullptr;  // jemalloc and tcmalloc only
  td::int64 chunk_overhead_ = 0;                        // per-allocation overhead of the allocator
  bool use_glibc_chunk_header_ = false;                 // read chunk size directly from the glibc chunk header
};
// sampling
std::atomic<td::int64> sampling_interval{0};
std::atomic<std::uintptr_t> region_begin{~static_cast<std::uintptr_t>(0) - REGION_SIZE + 1};  // unreachable range
std::atomic<bool> is_region_reserved{false};
pthread_mutex_t sampler_mutex = PTHREAD_MUTEX_INITIALIZER;  // protects owners, contexts and sampled blocks

RealAllocator real;
bool audit_mode = false;                  // set during initialization before the first allocation
td::int64 *shared_audit_tally = nullptr;  // exact tally for threads without own tally; changed atomically
std::atomic<int> init_state{0};           // 0 - not initialized, 1 - initializing, 2 - initialized
__thread bool is_initializing = false;

// memory for allocations made by dlsym during initialization; it is never freed
alignas(16) char bootstrap_buffer[BOOTSTRAP_SIZE];
std::atomic<std::size_t> bootstrap_used{0};

void write_error(const char *message) {
  auto result = ::write(2, message, std::strlen(message));
  static_cast<void>(result);
}

template <class F>
void resolve(F &function, const char *name) {
  function = reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
}

std::size_t get_glibc_chunk_size(std::size_t size) {
  std::size_t chunk = (size + 8 + 15) & ~static_cast<std::size_t>(15);
  return chunk < 32 ? 32 : chunk;
}

std::size_t get_glibc_mmapped_chunk_size(std::size_t size) {
  return (get_glibc_chunk_size(size) + 8 + HEAP_PAGE_SIZE - 1) & ~(HEAP_PAGE_SIZE - 1);
}

std::size_t read_glibc_chunk_header(const void *ptr) {
  return static_cast<const std::size_t *>(ptr)[-1];
}

// checks that the chunk header has the expected format, so the chunk size can be read without a function call;
// freeing of mmapped chunks smaller than 32 MB would increase the dynamic mmap threshold of glibc, so they aren't used
bool check_glibc_chunk_header(const RealAllocator &r) {
  static const std::size_t sizes[] = {0, 1, 24, 25, 100, 1000, 5000, 100000, static_cast<std::size_t>(40) << 20};
  bool is_ok = true;
  for (auto size : sizes) {
    void *ptr = r.malloc_(size);
    if (ptr == nullptr) {
      return false;
    }
    auto header = read_glibc_chunk_header(ptr);
    auto chunk = header & ~static_cast<std::size_t>(7);
    bool is_mmapped = (header & 2) != 0;
    auto usable = r.malloc_usable_size_(ptr);
    if (is_mmapped) {
      is_ok &= chunk == get_glibc_mmapped_chunk_size(size) && usable == chunk - 16;
    } else {
      is_ok &= chunk == get_glibc_chunk_size(size) && usable == chunk - 8;
    }
    r.free_(ptr);
  }
  return is_ok;
}

bool reserve_region();

bool ensure_initialized() {
  if (init_state.load(std::memory_order_acquire) == 2) {
    return true;
  }
  if (is_initializing) {
    return false;
  }
  is_initializing = true;
  int expected = 0;
  if (init_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
    RealAllocator r;
    resolve(r.malloc_, "malloc");
    resolve(r.free_, "free");
    resolve(r.calloc_, "calloc");
    resolve(r.realloc_, "realloc");
    resolve(r.memalign_, "memalign");
    resolve(r.malloc_usable_size_, "malloc_usable_size");
    resolve(r.nallocx_, "nallocx");
    if (r.malloc_ == nullptr || r.free_ == nullptr || r.calloc_ == nullptr || r.realloc_ == nullptr ||
        r.memalign_ == nullptr || r.malloc_usable_size_ == nullptr) {
      write_error("Failed to find memory allocation functions\n");
      std::abort();
    }
    // glibc stores size of a chunk before it; other allocators use size classes without per-allocation headers
    r.chunk_overhead_ = r.nallocx_ == nullptr ? 8 : 0;
    r.use_glibc_chunk_header_ = r.nallocx_ == nullptr && check_glibc_chunk_header(r);
    real = r;
    const char *audit = getenv("TELEGRAM_BOT_API_MEMORY_AUDIT");
    if (audit != nullptr && audit[0] == '1') {
      void *memory = mmap(nullptr, sizeof(td::int64) * AUDIT_TALLY_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
      if (memory != MAP_FAILED) {
        shared_audit_tally = static_cast<td::int64 *>(memory);
        audit_mode = true;
      } else {
        write_error("Failed to enable memory audit\n");
      }
    }
    // allocations are sampled from the start of the process, so the estimates include memory allocated before main
    if (reserve_region()) {
      sampling_interval.store(DEFAULT_SAMPLING_INTERVAL, std::memory_order_relaxed);
    }
    init_state.store(2, std::memory_order_release);
    // a child process must not inherit the mutex locked by another thread
    pthread_atfork([] { pthread_mutex_lock(&sampler_mutex); }, [] { pthread_mutex_unlock(&sampler_mutex); },
                   [] { pthread_mutex_unlock(&sampler_mutex); });
  } else {
    while (init_state.load(std::memory_order_acquire) != 2) {
      sched_yield();
    }
  }
  is_initializing = false;
  return true;
}

void *bootstrap_allocate(std::size_t size) {
  std::size_t total = (size + 16 + 15) & ~static_cast<std::size_t>(15);
  auto pos = bootstrap_used.fetch_add(total, std::memory_order_relaxed);
  if (size > BOOTSTRAP_SIZE || pos + total > BOOTSTRAP_SIZE) {
    write_error("Bootstrap memory is exhausted\n");
    std::abort();
  }
  auto *block = bootstrap_buffer + pos;
  *reinterpret_cast<std::size_t *>(block) = size;
  return block + 16;  // static memory is zero-initialized
}

inline bool is_bootstrap(const void *ptr) {
  return reinterpret_cast<std::uintptr_t>(ptr) - reinterpret_cast<std::uintptr_t>(bootstrap_buffer) < BOOTSTRAP_SIZE;
}

std::size_t get_bootstrap_size(const void *ptr) {
  return *reinterpret_cast<const std::size_t *>(static_cast<const char *>(ptr) - 16);
}

// exact size of live allocations, counted separately by each thread
struct alignas(64) ThreadCounter {
  std::atomic<td::int64> value{0};
};
ThreadCounter thread_counters[MAX_THREAD_COUNTERS];
std::atomic<std::size_t> thread_counter_count{0};
std::atomic<td::int64> shared_counter{0};  // for threads without own counter
__thread std::atomic<td::int64> *thread_counter = nullptr;

inline void add_live_bytes(td::int64 delta) {
  auto *counter = thread_counter;
  if (__builtin_expect(counter == nullptr, 0)) {
    auto index = thread_counter_count.fetch_add(1, std::memory_order_relaxed);
    counter = index < MAX_THREAD_COUNTERS ? &thread_counters[index].value : &shared_counter;
    thread_counter = counter;
  }
  if (__builtin_expect(counter == &shared_counter, 0)) {
    shared_counter.fetch_add(delta, std::memory_order_relaxed);
  } else {
    // the counter is changed only by the current thread
    counter->store(counter->load(std::memory_order_relaxed) + delta, std::memory_order_relaxed);
  }
}

// size of memory used by the real allocator for an allocation; for glibc it is the chunk size
inline td::int64 get_real_size(void *ptr) {
  if (__builtin_expect(real.use_glibc_chunk_header_, 1)) {
    return static_cast<td::int64>(read_glibc_chunk_header(ptr) & ~static_cast<std::size_t>(7));
  }
  return static_cast<td::int64>(real.malloc_usable_size_(ptr)) + real.chunk_overhead_;
}

// size of memory, which would be used by the real allocator for an allocation
td::int64 get_chunk_size(std::size_t size) {
  if (real.nallocx_ != nullptr) {
    return static_cast<td::int64>(real.nallocx_(size == 0 ? 1 : size, 0));
  }
  if (size + 16 >= (static_cast<std::size_t>(128) << 10)) {
    // glibc allocates big chunks with mmap by default
    return static_cast<td::int64>(get_glibc_mmapped_chunk_size(size));
  }
  return static_cast<td::int64>(get_glibc_chunk_size(size));
}

__thread td::int64 countdown = 0;
__thread td::int64 countdown_interval = 0;  // the interval used for the current countdown; 0 if not random
__thread td::int64 sample_interval = 0;     // the interval for the current sampled allocation
__thread td::uint64 random_state = 0;
__thread td::int32 part_override = -1;

double next_random() {
  if (random_state == 0) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    random_state = (reinterpret_cast<std::uintptr_t>(&random_state) ^ static_cast<td::uint64>(ts.tv_nsec) ^
                    (static_cast<td::uint64>(ts.tv_sec) << 32)) |
                   1;
  }
  // xorshift64*
  random_state ^= random_state >> 12;
  random_state ^= random_state << 25;
  random_state ^= random_state >> 27;
  auto value = random_state * 0x2545F4914F6CDD1DULL;
  return static_cast<double>((value >> 11) + 1) * (1.0 / 9007199254740992.0);  // (0, 1]
}

// the distance to the next point of the Poisson process is exponentially distributed regardless of the past
void reset_countdown() {
  auto interval = sampling_interval.load(std::memory_order_relaxed);
  if (interval > 0) {
    countdown = static_cast<td::int64>(-std::log(next_random()) * static_cast<double>(interval)) + 1;
    countdown_interval = interval;
  } else {
    countdown = DISABLED_RECHECK_BYTES;
    countdown_interval = 0;
  }
}

bool need_sample_slow(std::size_t size) {
  // the allocation contains a point of the Poisson process only if the countdown was drawn from the distribution
  bool is_sampled = countdown_interval != 0;
  sample_interval = countdown_interval;
  reset_countdown();
  return is_sampled && size <= MAX_SAMPLED_SIZE;
}

// an allocation of n bytes covers n + 1 bytes of the sampling process, so even empty allocations can be sampled
inline bool need_sample(std::size_t size) {
  auto left = countdown;
  if (__builtin_expect(static_cast<td::uint64>(left) > static_cast<td::uint64>(size) + 1, 1)) {
    countdown = left - static_cast<td::int64>(size) - 1;
    return false;
  }
  return need_sample_slow(size);
}

inline bool is_sampled_block(const void *ptr) {
  return reinterpret_cast<std::uintptr_t>(ptr) - region_begin.load(std::memory_order_relaxed) < REGION_SIZE;
}

struct SampleInfo {
  td::uint64 size;        // requested size
  td::int64 chunk;        // size of memory, which would be used by the real allocator
  td::int64 weight;       // estimated size of all allocations represented by the sample
  td::uint32 generation;  // generation of the owner
  td::uint16 owner;       // 0 if the allocation wasn't made by an owner
  td::uint8 part;
  td::uint8 small_class;  // class of a small slot or 0 for page runs
};
static_assert(sizeof(SampleInfo) <= SMALL_HEADER_SIZE, "");
static_assert(MAX_OWNERS <= 65536, "");

struct RunInfo {
  SampleInfo sample;
  td::uint64 committed_bytes;  // size of the mapping with commit accounting at the beginning of the run
  td::uint64 resident_bytes;   // size of the beginning of the run, which could have been used since it was zeroed
  td::uint32 run_class;
  td::uint32 next_free;
};
RunInfo *run_infos = nullptr;  // for each page of the region

struct OwnerInfo {
  std::atomic<td::int64> bytes[HeapSampler::PART_COUNT];
  std::atomic<td::int64> sample_count;
  __int128 variance;      // sum of weight * (weight - chunk) of live samples; protected by the lock
  td::uint32 generation;  // read without the lock only with atomic builtins
  bool is_used;
  td::int64 audit_baseline[HeapSampler::PART_COUNT];  // values of the exact tallies when the owner was created
};
OwnerInfo owners[MAX_OWNERS];  // owners[0] accumulates unattributed allocations
td::int32 free_owners[MAX_OWNERS];
td::int32 free_owner_count = 0;
td::int32 next_owner = 1;

struct ContextEntry {
  td::uint64 context;  // 0 for empty entries
  td::int32 owner;
  td::int32 part;
};
ContextEntry context_table[CONTEXT_TABLE_SIZE];
ContextEntry context_table_copy[CONTEXT_TABLE_SIZE];
std::size_t context_count = 0;
std::size_t deleted_context_count = 0;

// state of the sampled blocks allocator, protected by sampler_mutex
td::uint32 next_region_page = 0;
td::uint32 free_runs[RUN_CLASS_COUNT];   // runs, which are returned to the OS, except runs of one page
td::uint32 dirty_runs[RUN_CLASS_COUNT];  // runs of more than one page, which weren't returned to the OS
void *free_small_slots[SMALL_CLASS_COUNT];
char *small_slab_pos[SMALL_CLASS_COUNT];
char *small_slab_end[SMALL_CLASS_COUNT];
td::int64 slab_page_count = 0;
td::int64 cached_run_pages = 0;  // pages of free runs of one page, which weren't returned to the OS
td::int64 cached_run_bytes = 0;  // resident size of dirty_runs
td::int64 large_run_bytes = 0;   // resident size of live page runs
td::int64 total_sample_count = 0;
td::int64 total_sampled_bytes = 0;
__int128 total_variance = 0;
td::int64 audit_unattributed_adjustment[HeapSampler::PART_COUNT];  // exact memory of destroyed owners
std::atomic<td::uint64> context_version{1};                        // changed whenever a context or an owner is changed

td::uint64 now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<td::uint64>(ts.tv_sec) * 1000000000 + static_cast<td::uint64>(ts.tv_nsec);
}

void update_max(std::atomic<td::uint64> &max_value, td::uint64 value) {
  auto current = max_value.load(std::memory_order_relaxed);
  while (value > current && !max_value.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

std::atomic<td::uint64> operation_counts[HeapSampler::DURATION_BUCKET_COUNT];
std::atomic<td::uint64> total_operation_ns{0};
std::atomic<td::uint64> max_operation_ns{0};
std::atomic<td::uint64> max_lock_wait_ns{0};

// measures duration of an operation with sampled allocations
class OperationTimer {
 public:
  OperationTimer() : start_(now_ns()) {
  }
  OperationTimer(const OperationTimer &) = delete;
  OperationTimer &operator=(const OperationTimer &) = delete;
  ~OperationTimer() {
    auto duration = now_ns() - start_;
    std::size_t bucket = 0;
    while (bucket + 1 < HeapSampler::DURATION_BUCKET_COUNT && duration > HeapSampler::DURATION_BUCKETS_NS[bucket]) {
      bucket++;
    }
    operation_counts[bucket].fetch_add(1, std::memory_order_relaxed);
    total_operation_ns.fetch_add(duration, std::memory_order_relaxed);
    update_max(max_operation_ns, duration);
  }

 private:
  td::uint64 start_;
};

class SamplerLock {
 public:
  SamplerLock() {
    if (pthread_mutex_trylock(&sampler_mutex) != 0) {
      auto start = now_ns();
      pthread_mutex_lock(&sampler_mutex);
      update_max(max_lock_wait_ns, now_ns() - start);
    }
  }
  SamplerLock(const SamplerLock &) = delete;
  SamplerLock &operator=(const SamplerLock &) = delete;
  ~SamplerLock() {
    pthread_mutex_unlock(&sampler_mutex);
  }
};

std::size_t get_context_hash(td::uint64 context) {
  return static_cast<std::size_t>(((context >> 4) * 0x9E3779B97F4A7C15ULL) >> 48);
}

// must be called under the lock
const ContextEntry *find_context(td::uint64 context) {
  if (context <= DELETED_CONTEXT) {
    return nullptr;
  }
  for (std::size_t i = 0, pos = get_context_hash(context); i < CONTEXT_TABLE_SIZE;
       i++, pos = (pos + 1) & (CONTEXT_TABLE_SIZE - 1)) {
    auto &entry = context_table[pos];
    if (entry.context == context) {
      return &entry;
    }
    if (entry.context == 0) {
      return nullptr;
    }
  }
  return nullptr;
}

// exact tallies of memory of owners, counted separately by each thread; used only in audit mode
std::atomic<td::int64 *> audit_tallies[MAX_AUDIT_THREADS];
std::atomic<std::size_t> audit_tally_count{0};
__thread td::int64 *thread_audit_tally = nullptr;

void add_audit_bytes(td::int32 owner, td::int32 part, td::int64 delta) {
  auto *tally = thread_audit_tally;
  if (tally == nullptr) {
    auto index = audit_tally_count.fetch_add(1, std::memory_order_relaxed);
    void *memory = MAP_FAILED;
    if (index < MAX_AUDIT_THREADS) {
      memory = mmap(nullptr, sizeof(td::int64) * AUDIT_TALLY_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    }
    if (memory != MAP_FAILED) {
      tally = static_cast<td::int64 *>(memory);
      audit_tallies[index].store(tally, std::memory_order_release);
    } else {
      tally = shared_audit_tally;
    }
    thread_audit_tally = tally;
  }
  auto &value = tally[static_cast<std::size_t>(owner) * HeapSampler::PART_COUNT + static_cast<std::size_t>(part)];
  if (tally == shared_audit_tally) {
    __atomic_fetch_add(&value, delta, __ATOMIC_RELAXED);
  } else {
    __atomic_store_n(&value, __atomic_load_n(&value, __ATOMIC_RELAXED) + delta, __ATOMIC_RELAXED);
  }
}

td::int64 sum_audit_tallies(td::int32 owner, td::int32 part) {
  auto offset = static_cast<std::size_t>(owner) * HeapSampler::PART_COUNT + static_cast<std::size_t>(part);
  td::int64 result = __atomic_load_n(&shared_audit_tally[offset], __ATOMIC_RELAXED);
  auto count = audit_tally_count.load(std::memory_order_relaxed);
  for (std::size_t i = 0; i < count && i < MAX_AUDIT_THREADS; i++) {
    auto *tally = audit_tallies[i].load(std::memory_order_acquire);
    if (tally != nullptr) {
      result += __atomic_load_n(&tally[offset], __ATOMIC_RELAXED);
    }
  }
  return result;
}

// must be called under the lock
void insert_context(const ContextEntry &new_entry) {
  ContextEntry *deleted = nullptr;
  for (std::size_t i = 0, pos = get_context_hash(new_entry.context); i < CONTEXT_TABLE_SIZE;
       i++, pos = (pos + 1) & (CONTEXT_TABLE_SIZE - 1)) {
    auto &entry = context_table[pos];
    if (entry.context == new_entry.context) {
      entry = new_entry;
      return;
    }
    if (entry.context == DELETED_CONTEXT && deleted == nullptr) {
      deleted = &entry;
    }
    if (entry.context == 0) {
      if (deleted == nullptr) {
        deleted = &entry;
      } else {
        deleted_context_count--;
      }
      *deleted = new_entry;
      context_count++;
      return;
    }
  }
  if (deleted != nullptr) {
    deleted_context_count--;
    *deleted = new_entry;
    context_count++;
  }
}

// must be called under the lock
void rehash_contexts() {
  std::memcpy(context_table_copy, context_table, sizeof(context_table));
  std::memset(context_table, 0, sizeof(context_table));
  context_count = 0;
  deleted_context_count = 0;
  for (auto &entry : context_table_copy) {
    if (entry.context > DELETED_CONTEXT) {
      insert_context(entry);
    }
  }
}

// must be called under the lock
char *allocate_run(td::uint32 run_class, bool &is_zero) {
  td::uint32 page = dirty_runs[run_class];
  if (page != NO_PAGE) {
    dirty_runs[run_class] = run_infos[page].next_free;
    cached_run_bytes -= static_cast<td::int64>(run_infos[page].resident_bytes);
    is_zero = false;
  } else {
    page = free_runs[run_class];
    if (page != NO_PAGE) {
      free_runs[run_class] = run_infos[page].next_free;
      // pages of bigger runs are returned to the OS when the runs are freed
      is_zero = run_class > 0;
      if (run_class == 0) {
        cached_run_pages--;
      }
    } else {
      is_zero = true;
      td::uint64 page_count = static_cast<td::uint64>(1) << run_class;
      if (next_region_page + page_count > REGION_PAGE_COUNT) {
        return nullptr;
      }
      page = next_region_page;
      next_region_page += static_cast<td::uint32>(page_count);
    }
    run_infos[page].resident_bytes = 0;
  }
  run_infos[page].run_class = run_class;
  run_infos[page].committed_bytes = 0;
  return reinterpret_cast<char *>(region_begin.load(std::memory_order_relaxed) + page * HEAP_PAGE_SIZE);
}

// must be called under the lock
void release_run(td::uint32 page) {
  auto run_class = run_infos[page].run_class;
  run_infos[page].next_free = free_runs[run_class];
  free_runs[run_class] = page;
  if (run_class == 0) {
    cached_run_pages++;
  }
}

// keeps a freed run with its memory if possible; must be called under the lock
bool cache_dirty_run(td::uint32 page) {
  auto &run_info = run_infos[page];
  if (run_info.run_class == 0 || run_info.run_class > MAX_CACHED_RUN_CLASS || run_info.committed_bytes != 0 ||
      cached_run_bytes + static_cast<td::int64>(run_info.resident_bytes) > RUN_CACHE_LIMIT) {
    return false;
  }
  run_info.next_free = dirty_runs[run_info.run_class];
  dirty_runs[run_info.run_class] = page;
  cached_run_bytes += static_cast<td::int64>(run_info.resident_bytes);
  return true;
}

// big blocks are mapped with commit accounting, so they can't be allocated if there is not enough memory
bool commit_pages(char *begin, std::size_t size) {
  return mmap(begin, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED;
}

// returns memory to the OS
void decommit_pages(char *begin, std::size_t size, bool is_committed) {
  if (!is_committed || mmap(begin, size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == MAP_FAILED) {
    madvise(begin, size, MADV_DONTNEED);
  }
}

std::size_t round_to_pages(std::size_t size) {
  return (size + HEAP_PAGE_SIZE - 1) & ~(HEAP_PAGE_SIZE - 1);
}

// must be called under the lock
void add_owner_sample(SampleInfo &info, td::uint64 context, td::int32 sign) {
  if (sign > 0) {
    td::int32 owner = 0;
    td::int32 part = 0;
    auto *context_entry = find_context(context);
    if (context_entry != nullptr) {
      owner = context_entry->owner;
      part = part_override >= 0 ? part_override : context_entry->part;
    }
    info.owner = static_cast<td::uint16>(owner);
    info.generation = owners[owner].generation;
    info.part = static_cast<td::uint8>(part);
  }
  td::int32 owner = info.owner;
  if (owner != 0 && owners[owner].generation != info.generation) {
    owner = 0;  // the owner was destroyed and its memory was moved to the unattributed memory
  }
  auto &owner_info = owners[owner];
  owner_info.bytes[info.part].store(owner_info.bytes[info.part].load(std::memory_order_relaxed) + sign * info.weight,
                                    std::memory_order_relaxed);
  owner_info.sample_count.store(owner_info.sample_count.load(std::memory_order_relaxed) + sign,
                                std::memory_order_relaxed);
  // Horvitz-Thompson estimator of the variance: chunk^2 * (1 - p) / p^2 == weight * (weight - chunk)
  auto variance = static_cast<__int128>(info.weight) * (info.weight - info.chunk);
  owner_info.variance += sign * variance;
  total_variance += sign * variance;
  total_sample_count += sign;
  total_sampled_bytes += sign * info.chunk;
  if (audit_mode) {
    add_audit_bytes(owner, info.part, sign * info.chunk);
  }
}

// owner of allocations made in the current actor context; cached by each thread
struct ContextCacheEntry {
  td::uint64 context;
  td::uint64 version;
  td::int32 owner;
  td::int32 part;
  td::uint32 generation;
};
__thread ContextCacheEntry context_cache[CONTEXT_CACHE_SIZE];

void get_current_owner(td::int32 &owner, td::int32 &part, td::uint32 &generation) {
  auto context = reinterpret_cast<td::uint64>(td::Scheduler::context());
  auto version = context_version.load(std::memory_order_acquire);
  auto &entry = context_cache[(context >> 4) & (CONTEXT_CACHE_SIZE - 1)];
  if (entry.context != context || entry.version != version) {
    SamplerLock lock;
    entry.context = context;
    entry.version = context_version.load(std::memory_order_relaxed);
    auto *context_entry = find_context(context);
    entry.owner = context_entry != nullptr ? context_entry->owner : 0;
    entry.part = context_entry != nullptr ? context_entry->part : 0;
    entry.generation = owners[entry.owner].generation;
  }
  owner = entry.owner;
  part = entry.owner != 0 && part_override >= 0 ? part_override : entry.part;
  generation = entry.generation;
}

// in audit mode every block, which isn't sampled, has a header with its owner before it
struct AuditHeader {
  td::int64 chunk;  // size accounted for the owner
  td::uint32 generation;
  td::uint16 owner;
  td::uint8 part;
  td::uint8 shift;  // log2 of the distance between the block of the real allocator and the returned pointer
};
static_assert(sizeof(AuditHeader) == AUDIT_HEADER_SIZE, "");

AuditHeader *get_audit_header(void *ptr) {
  return reinterpret_cast<AuditHeader *>(static_cast<char *>(ptr) - AUDIT_HEADER_SIZE);
}

void *get_audit_base(void *ptr) {
  return static_cast<char *>(ptr) - (static_cast<std::size_t>(1) << get_audit_header(ptr)->shift);
}

void *finish_audit_allocation(void *base, td::uint8 shift, std::size_t size) {
  auto *result = static_cast<char *>(base) + (static_cast<std::size_t>(1) << shift);
  auto *header = get_audit_header(result);
  td::int32 owner;
  td::int32 part;
  td::uint32 generation;
  get_current_owner(owner, part, generation);
  header->chunk = get_chunk_size(size);
  header->generation = generation;
  header->owner = static_cast<td::uint16>(owner);
  header->part = static_cast<td::uint8>(part);
  header->shift = shift;
  add_audit_bytes(owner, part, header->chunk);
  return result;
}

void release_audit_allocation(const AuditHeader &header) {
  td::int32 owner = header.owner;
  if (owner != 0 && __atomic_load_n(&owners[owner].generation, __ATOMIC_RELAXED) != header.generation) {
    owner = 0;  // the owner was destroyed
  }
  add_audit_bytes(owner, header.part, -header.chunk);
}

void *audit_allocate(std::size_t size, std::size_t alignment, bool zero) {
  std::size_t prefix = AUDIT_HEADER_SIZE;
  td::uint8 shift = 4;
  while (prefix < alignment) {
    prefix <<= 1;
    shift++;
  }
  if (size > ~static_cast<std::size_t>(0) - prefix) {
    errno = ENOMEM;
    return nullptr;
  }
  void *base;
  if (prefix > AUDIT_HEADER_SIZE) {
    base = real.memalign_(prefix, size + prefix);
  } else if (zero) {
    base = real.calloc_(1, size + prefix);
  } else {
    base = real.malloc_(size + prefix);
  }
  if (base == nullptr) {
    return nullptr;
  }
  if (zero && prefix > AUDIT_HEADER_SIZE) {
    std::memset(static_cast<char *>(base) + prefix, 0, size);
  }
  add_live_bytes(get_real_size(base));
  return finish_audit_allocation(base, shift, size);
}

void audit_free(void *ptr) {
  release_audit_allocation(*get_audit_header(ptr));
  void *base = get_audit_base(ptr);
  add_live_bytes(-get_real_size(base));
  real.free_(base);
}

std::size_t get_audit_usable_size(void *ptr) {
  return real.malloc_usable_size_(get_audit_base(ptr)) - (static_cast<std::size_t>(1) << get_audit_header(ptr)->shift);
}

void *audit_reallocate(void *ptr, std::size_t size) {
  AuditHeader old_header = *get_audit_header(ptr);
  if (old_header.shift != 4 || size > ~static_cast<std::size_t>(0) - AUDIT_HEADER_SIZE) {
    // aligned blocks are moved
    auto old_size = get_audit_usable_size(ptr);
    void *result = audit_allocate(size, 16, false);
    if (result == nullptr) {
      return nullptr;
    }
    std::memcpy(result, ptr, old_size < size ? old_size : size);
    audit_free(ptr);
    return result;
  }
  void *base = get_audit_base(ptr);
  auto old_real_size = get_real_size(base);
  void *new_base = real.realloc_(base, size + AUDIT_HEADER_SIZE);
  if (new_base == nullptr) {
    return nullptr;
  }
  release_audit_allocation(old_header);
  add_live_bytes(get_real_size(new_base) - old_real_size);
  return finish_audit_allocation(new_base, 4, size);
}

td::uint32 get_page(const void *ptr) {
  return static_cast<td::uint32>(
      (reinterpret_cast<std::uintptr_t>(ptr) - region_begin.load(std::memory_order_relaxed)) / HEAP_PAGE_SIZE);
}

td::uint32 get_run_class(std::size_t size) {
  td::uint64 page_count = (size + HEAP_PAGE_SIZE - 1) / HEAP_PAGE_SIZE;
  td::uint32 run_class = 0;
  while ((static_cast<td::uint64>(1) << run_class) < page_count) {
    run_class++;
  }
  return run_class;
}

SampleInfo *get_sample_info(void *ptr) {
  if ((reinterpret_cast<std::uintptr_t>(ptr) & (HEAP_PAGE_SIZE - 1)) == 0) {
    return &run_infos[get_page(ptr)].sample;
  }
  return reinterpret_cast<SampleInfo *>(static_cast<char *>(ptr) - SMALL_HEADER_SIZE);
}

td::int64 get_sample_weight(std::size_t size, td::int64 chunk, td::int64 interval) {
  double probability = -std::expm1(-static_cast<double>(size + 1) / static_cast<double>(interval));
  return static_cast<td::int64>(std::llround(static_cast<double>(chunk) / probability));
}

// returns nullptr if the block can't be allocated
void *sample_allocate(std::size_t size, std::size_t alignment, bool zero) {
  if (alignment > HEAP_PAGE_SIZE || sample_interval <= 0) {
    return nullptr;
  }
  OperationTimer timer;
  auto chunk = get_chunk_size(size);
  auto weight = get_sample_weight(size, chunk, sample_interval);
  auto context = reinterpret_cast<td::uint64>(td::Scheduler::context());
  bool is_small = alignment <= 16 && size + SMALL_HEADER_SIZE <= MAX_SMALL_SLOT_SIZE;

  char *result;
  bool is_zero = false;
  {
    SamplerLock lock;
    SampleInfo *info;
    if (is_small) {
      // slots have at least 16 bytes of data, so the last slot of a page doesn't end at the page boundary and
      // pointers to small blocks are never page-aligned
      auto slot_size = (size + SMALL_HEADER_SIZE + 15) & ~static_cast<std::size_t>(15);
      if (slot_size < SMALL_HEADER_SIZE + 16) {
        slot_size = SMALL_HEADER_SIZE + 16;
      }
      auto small_class = slot_size / 16;
      char *slot = static_cast<char *>(free_small_slots[small_class]);
      if (slot != nullptr) {
        free_small_slots[small_class] = *reinterpret_cast<void **>(slot);
      } else {
        if (small_slab_pos[small_class] == nullptr ||
            small_slab_pos[small_class] + slot_size > small_slab_end[small_class]) {
          bool is_page_zero;
          char *page = allocate_run(0, is_page_zero);
          if (page == nullptr) {
            return nullptr;
          }
          slab_page_count++;
          small_slab_pos[small_class] = page;
          small_slab_end[small_class] = page + HEAP_PAGE_SIZE;
        }
        slot = small_slab_pos[small_class];
        small_slab_pos[small_class] += slot_size;
      }
      info = reinterpret_cast<SampleInfo *>(slot);
      info->small_class = static_cast<td::uint8>(small_class);
      result = slot + SMALL_HEADER_SIZE;
    } else {
      char *run = allocate_run(get_run_class(size), is_zero);
      if (run == nullptr) {
        return nullptr;
      }
      auto page = get_page(run);
      auto mapped_size = round_to_pages(size);
      if (mapped_size >= COMMIT_THRESHOLD) {
        if (!commit_pages(run, mapped_size)) {
          release_run(page);
          return nullptr;
        }
        run_infos[page].committed_bytes = mapped_size;
        is_zero = true;
      }
      info = &run_infos[page].sample;
      info->small_class = 0;
      auto &resident_bytes = run_infos[page].resident_bytes;
      if (resident_bytes < mapped_size) {
        resident_bytes = mapped_size;
      }
      large_run_bytes += static_cast<td::int64>(resident_bytes);
      result = run;
    }
    info->size = size;
    info->chunk = chunk;
    info->weight = weight;
    add_owner_sample(*info, context, 1);
  }

  if (zero && !is_zero) {
    std::memset(result, 0, size);
  }
  return result;
}

// changes size of a sampled page run without moving it; returns false if it isn't possible
bool sample_reallocate_in_place(void *ptr, std::size_t size) {
  if ((reinterpret_cast<std::uintptr_t>(ptr) & (HEAP_PAGE_SIZE - 1)) != 0) {
    return false;  // a small slot
  }
  // a block of the new size would be sampled with the probability of at least 1 - exp(-16), so keeping the block
  // sampled doesn't introduce a noticeable bias
  auto interval = sampling_interval.load(std::memory_order_relaxed);
  if (interval <= 0 || size / 16 < static_cast<std::size_t>(interval)) {
    return false;
  }
  OperationTimer timer;
  auto &run_info = run_infos[get_page(ptr)];
  auto mapped_size = round_to_pages(size);
  if (mapped_size > (HEAP_PAGE_SIZE << run_info.run_class)) {
    return false;
  }
  if (run_info.committed_bytes == 0 ? mapped_size >= COMMIT_THRESHOLD : mapped_size > run_info.committed_bytes) {
    if (run_info.committed_bytes == 0 ||
        !commit_pages(static_cast<char *>(ptr) + run_info.committed_bytes, mapped_size - run_info.committed_bytes)) {
      return false;
    }
    run_info.committed_bytes = mapped_size;
  }

  auto chunk = get_chunk_size(size);
  auto weight = get_sample_weight(size, chunk, interval);
  auto context = reinterpret_cast<td::uint64>(td::Scheduler::context());
  SamplerLock lock;
  auto &info = run_info.sample;
  add_owner_sample(info, 0, -1);
  if (run_info.resident_bytes < mapped_size) {
    large_run_bytes += static_cast<td::int64>(mapped_size - run_info.resident_bytes);
    run_info.resident_bytes = mapped_size;
  }
  info.size = size;
  info.chunk = chunk;
  info.weight = weight;
  add_owner_sample(info, context, 1);
  return true;
}

void sample_free(void *ptr) {
  OperationTimer timer;
  auto *info = get_sample_info(ptr);
  if (info->small_class != 0) {
    SamplerLock lock;
    add_owner_sample(*info, 0, -1);
    auto small_class = info->small_class;
    char *slot = reinterpret_cast<char *>(info);
    *reinterpret_cast<void **>(slot) = free_small_slots[small_class];
    free_small_slots[small_class] = slot;
    return;
  }

  auto page = get_page(ptr);
  auto &run_info = run_infos[page];
  {
    SamplerLock lock;
    add_owner_sample(*info, 0, -1);
    large_run_bytes -= static_cast<td::int64>(run_info.resident_bytes);
    if (run_info.run_class == 0) {
      release_run(page);
      return;
    }
    if (cache_dirty_run(page)) {
      return;
    }
  }

  // return memory to the OS without the lock and before the run can be reused by another thread
  if (run_info.committed_bytes > 0) {
    // the block is never bigger than the committed mapping, so pages after it weren't used
    decommit_pages(static_cast<char *>(ptr), run_info.committed_bytes, true);
    run_info.committed_bytes = 0;
  } else {
    decommit_pages(static_cast<char *>(ptr), HEAP_PAGE_SIZE << run_info.run_class, false);
  }
  SamplerLock lock;
  release_run(page);
}

// reserves address space for sampled blocks; returns false if it isn't possible
bool reserve_region() {
  if (is_region_reserved.load()) {
    return true;
  }
  SamplerLock lock;
  if (is_region_reserved.load()) {
    return true;
  }
  auto infos_size = sizeof(RunInfo) * static_cast<std::size_t>(REGION_PAGE_COUNT);
  void *region = mmap(nullptr, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  void *infos = mmap(nullptr, infos_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (region == MAP_FAILED || infos == MAP_FAILED) {
    if (region != MAP_FAILED) {
      munmap(region, REGION_SIZE);
    }
    if (infos != MAP_FAILED) {
      munmap(infos, infos_size);
    }
    write_error("Failed to reserve address space for memory accounting\n");
    return false;
  }
  // sparse ranges must not be collapsed into huge pages and dumped into core files
  madvise(region, REGION_SIZE, MADV_NOHUGEPAGE);
  madvise(region, REGION_SIZE, MADV_DONTDUMP);
  madvise(infos, infos_size, MADV_NOHUGEPAGE);
  madvise(infos, infos_size, MADV_DONTDUMP);
  run_infos = static_cast<RunInfo *>(infos);
  for (auto &free_run : free_runs) {
    free_run = NO_PAGE;
  }
  for (auto &dirty_run : dirty_runs) {
    dirty_run = NO_PAGE;
  }
  region_begin.store(reinterpret_cast<std::uintptr_t>(region), std::memory_order_relaxed);
  is_region_reserved.store(true);
  return true;
}

std::size_t get_sampled_size(void *ptr) {
  return static_cast<std::size_t>(get_sample_info(ptr)->size);
}

void *do_malloc(std::size_t size) {
  if (__builtin_expect(init_state.load(std::memory_order_acquire) != 2, 0) && !ensure_initialized()) {
    return bootstrap_allocate(size);
  }
  if (__builtin_expect(need_sample(size), 0)) {
    void *result = sample_allocate(size, 16, false);
    if (result != nullptr) {
      return result;
    }
  }
  if (__builtin_expect(audit_mode, 0)) {
    return audit_allocate(size, 16, false);
  }
  void *result = real.malloc_(size);
  if (result != nullptr) {
    add_live_bytes(get_real_size(result));
  }
  return result;
}

void do_free(void *ptr) {
  if (__builtin_expect(is_sampled_block(ptr), 0)) {
    return sample_free(ptr);
  }
  if (ptr == nullptr || is_bootstrap(ptr)) {
    return;
  }
  if (__builtin_expect(audit_mode, 0)) {
    return audit_free(ptr);
  }
  add_live_bytes(-get_real_size(ptr));
  real.free_(ptr);
}

void *do_memalign(std::size_t alignment, std::size_t size) {
  if (size == 0) {
    size = 1;  // jemalloc doesn't align empty allocations
  }
  if (__builtin_expect(init_state.load(std::memory_order_acquire) != 2, 0) && !ensure_initialized()) {
    // bootstrap blocks are aligned to 16 bytes
    return alignment <= 16 ? bootstrap_allocate(size) : nullptr;
  }
  if (__builtin_expect(need_sample(size), 0)) {
    void *result = sample_allocate(size, alignment, false);
    if (result != nullptr) {
      return result;
    }
  }
  if (__builtin_expect(audit_mode, 0)) {
    return audit_allocate(size, alignment, false);
  }
  void *result = real.memalign_(alignment, size);
  if (result != nullptr) {
    add_live_bytes(get_real_size(result));
  }
  return result;
}

std::size_t do_usable_size(void *ptr) {
  if (ptr == nullptr) {
    return 0;
  }
  if (is_sampled_block(ptr)) {
    return get_sampled_size(ptr);
  }
  if (is_bootstrap(ptr)) {
    return get_bootstrap_size(ptr);
  }
  if (audit_mode) {
    return get_audit_usable_size(ptr);
  }
  return real.malloc_usable_size_(ptr);
}

}  // namespace

bool HeapSampler::is_supported() {
  return true;
}

bool HeapSampler::is_audit_enabled() {
  return audit_mode;
}

void HeapSampler::set_sampling_interval(td::int64 interval) {
  if (interval > 0 && !reserve_region()) {
    interval = 0;
  }
  sampling_interval.store(interval < 0 ? 0 : interval, std::memory_order_relaxed);
  // other threads will use the new interval after their current countdown expires
  reset_countdown();
}

td::int64 HeapSampler::get_sampling_interval() {
  return sampling_interval.load(std::memory_order_relaxed);
}

HeapSampler::PartGuard::PartGuard(td::int32 part) : old_part_(part_override) {
  part_override = part >= 0 && part < PART_COUNT ? part : -1;
}

HeapSampler::PartGuard::~PartGuard() {
  part_override = old_part_;
}

td::int32 HeapSampler::create_owner() {
  SamplerLock lock;
  td::int32 owner;
  if (free_owner_count > 0) {
    owner = free_owners[--free_owner_count];
  } else if (next_owner < MAX_OWNERS) {
    owner = next_owner++;
  } else {
    return 0;
  }
  auto &info = owners[owner];
  for (auto &bytes : info.bytes) {
    bytes.store(0, std::memory_order_relaxed);
  }
  info.sample_count.store(0, std::memory_order_relaxed);
  info.variance = 0;
  if (audit_mode) {
    // tallies of the slot can contain memory of destroyed owners, which is subtracted from the unattributed memory
    for (td::int32 part = 0; part < PART_COUNT; part++) {
      info.audit_baseline[part] = sum_audit_tallies(owner, part);
    }
  }
  __atomic_store_n(&info.generation, info.generation + 1, __ATOMIC_RELAXED);
  info.is_used = true;
  context_version.fetch_add(1, std::memory_order_release);
  return owner;
}

void HeapSampler::destroy_owner(td::int32 owner) {
  if (owner <= 0 || owner >= MAX_OWNERS) {
    return;
  }
  SamplerLock lock;
  auto &info = owners[owner];
  if (!info.is_used) {
    return;
  }
  auto &unattributed = owners[0];
  for (td::int32 part = 0; part < PART_COUNT; part++) {
    unattributed.bytes[part].store(
        unattributed.bytes[part].load(std::memory_order_relaxed) + info.bytes[part].load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    info.bytes[part].store(0, std::memory_order_relaxed);
  }
  unattributed.sample_count.store(
      unattributed.sample_count.load(std::memory_order_relaxed) + info.sample_count.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  info.sample_count.store(0, std::memory_order_relaxed);
  unattributed.variance += info.variance;
  info.variance = 0;
  if (audit_mode) {
    for (td::int32 part = 0; part < PART_COUNT; part++) {
      audit_unattributed_adjustment[part] += sum_audit_tallies(owner, part) - info.audit_baseline[part];
    }
  }
  // samples with the previous generation will be removed from the unattributed memory
  __atomic_store_n(&info.generation, info.generation + 1, __ATOMIC_RELAXED);
  info.is_used = false;
  free_owners[free_owner_count++] = owner;
  context_version.fetch_add(1, std::memory_order_release);
}

void HeapSampler::set_context_owner(td::uint64 context, td::int32 owner, td::int32 part) {
  if (context <= DELETED_CONTEXT || owner <= 0 || owner >= MAX_OWNERS || part < 0 || part >= PART_COUNT) {
    return;
  }
  SamplerLock lock;
  if ((context_count + deleted_context_count + 1) * 4 > CONTEXT_TABLE_SIZE * 3) {
    rehash_contexts();
    if ((context_count + 1) * 4 > CONTEXT_TABLE_SIZE * 3) {
      return;  // too many contexts; allocations in the context will be unattributed
    }
  }
  insert_context(ContextEntry{context, owner, part});
  context_version.fetch_add(1, std::memory_order_release);
}

void HeapSampler::remove_context(td::uint64 context, td::int32 owner) {
  SamplerLock lock;
  auto *entry = const_cast<ContextEntry *>(find_context(context));
  if (entry != nullptr && entry->owner == owner) {
    entry->context = DELETED_CONTEXT;
    context_count--;
    deleted_context_count++;
    context_version.fetch_add(1, std::memory_order_release);
  }
}

HeapSampler::OwnerStats HeapSampler::get_owner_stats(td::int32 owner) {
  OwnerStats result;
  if (owner < 0 || owner >= MAX_OWNERS) {
    return result;
  }
  SamplerLock lock;
  auto &info = owners[owner];
  for (td::int32 part = 0; part < PART_COUNT; part++) {
    result.bytes[part] = info.bytes[part].load(std::memory_order_relaxed);
    if (audit_mode) {
      result.exact_bytes[part] = owner == 0 ? sum_audit_tallies(0, part) + audit_unattributed_adjustment[part]
                                            : sum_audit_tallies(owner, part) - info.audit_baseline[part];
    }
  }
  result.sample_count = info.sample_count.load(std::memory_order_relaxed);
  result.variance = static_cast<double>(info.variance);
  return result;
}

HeapSampler::Stats HeapSampler::get_stats() {
  Stats result;
  auto counter_count = thread_counter_count.load(std::memory_order_relaxed);
  if (counter_count > MAX_THREAD_COUNTERS) {
    counter_count = MAX_THREAD_COUNTERS;
  }
  for (std::size_t i = 0; i < counter_count; i++) {
    result.live_bytes += thread_counters[i].value.load(std::memory_order_relaxed);
  }
  result.live_bytes += shared_counter.load(std::memory_order_relaxed);

  SamplerLock lock;
  result.live_bytes += total_sampled_bytes;
  result.sample_count = total_sample_count;
  result.sampled_bytes = total_sampled_bytes;
  for (td::int32 owner = 0; owner < next_owner; owner++) {
    for (td::int32 part = 0; part < PART_COUNT; part++) {
      auto bytes = owners[owner].bytes[part].load(std::memory_order_relaxed);
      result.estimated_live_bytes += bytes;
      if (owner == 0) {
        result.unattributed_bytes += bytes;
      }
    }
  }
  result.sampler_overhead_bytes = (slab_page_count + cached_run_pages) * static_cast<td::int64>(HEAP_PAGE_SIZE) +
                                  cached_run_bytes + large_run_bytes - total_sampled_bytes;
  if (result.sampler_overhead_bytes < 0) {
    result.sampler_overhead_bytes = 0;
  }
  result.estimate_variance = static_cast<double>(total_variance);
  if (audit_mode) {
    // every live allocation is accounted in exactly one slot, including allocations of destroyed owners
    for (td::int32 owner = 0; owner < next_owner; owner++) {
      for (td::int32 part = 0; part < PART_COUNT; part++) {
        result.audit_live_bytes += sum_audit_tallies(owner, part);
      }
    }
  }
  for (std::size_t i = 0; i < DURATION_BUCKET_COUNT; i++) {
    result.operation_counts[i] = operation_counts[i].load(std::memory_order_relaxed);
  }
  result.total_operation_ns = total_operation_ns.load(std::memory_order_relaxed);
  result.max_operation_ns = max_operation_ns.load(std::memory_order_relaxed);
  result.max_lock_wait_ns = max_lock_wait_ns.load(std::memory_order_relaxed);
  return result;
}

#else

bool HeapSampler::is_supported() {
  return false;
}

bool HeapSampler::is_audit_enabled() {
  return false;
}

void HeapSampler::set_sampling_interval(td::int64 interval) {
}

td::int64 HeapSampler::get_sampling_interval() {
  return 0;
}

td::int32 HeapSampler::create_owner() {
  return 0;
}

void HeapSampler::destroy_owner(td::int32 owner) {
}

void HeapSampler::set_context_owner(td::uint64 context, td::int32 owner, td::int32 part) {
}

void HeapSampler::remove_context(td::uint64 context, td::int32 owner) {
}

HeapSampler::PartGuard::PartGuard(td::int32 part) : old_part_(-1) {
}

HeapSampler::PartGuard::~PartGuard() {
  static_cast<void>(old_part_);
}

HeapSampler::OwnerStats HeapSampler::get_owner_stats(td::int32 owner) {
  return OwnerStats();
}

HeapSampler::Stats HeapSampler::get_stats() {
  return Stats();
}

#endif

}  // namespace telegram_bot_api

#if TELEGRAM_BOT_API_HEAP_SAMPLER_ENABLED
// replacements of the memory allocation functions; see "Replacing malloc" in the GNU C Library manual
extern "C" {

void *malloc(std::size_t size) noexcept {
  return telegram_bot_api::do_malloc(size);
}

void free(void *ptr) noexcept {
  telegram_bot_api::do_free(ptr);
}

void *calloc(std::size_t count, std::size_t size) noexcept {
  using namespace telegram_bot_api;
  std::size_t total;
  if (__builtin_mul_overflow(count, size, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  if (__builtin_expect(init_state.load(std::memory_order_acquire) != 2, 0) && !ensure_initialized()) {
    return bootstrap_allocate(total);
  }
  if (__builtin_expect(need_sample(total), 0)) {
    void *result = sample_allocate(total, 16, true);
    if (result != nullptr) {
      return result;
    }
  }
  if (__builtin_expect(audit_mode, 0)) {
    return audit_allocate(total, 16, true);
  }
  void *result = real.calloc_(count, size);
  if (result != nullptr) {
    add_live_bytes(get_real_size(result));
  }
  return result;
}

void *realloc(void *ptr, std::size_t size) noexcept {
  using namespace telegram_bot_api;
  if (ptr == nullptr) {
    return do_malloc(size);
  }
  if (size == 0) {
    do_free(ptr);
    return nullptr;
  }
  if (is_sampled_block(ptr) && sample_reallocate_in_place(ptr, size)) {
    return ptr;
  }
  if (is_sampled_block(ptr) || is_bootstrap(ptr)) {
    auto old_size = do_usable_size(ptr);
    void *result = do_malloc(size);
    if (result == nullptr) {
      return nullptr;
    }
    std::memcpy(result, ptr, old_size < size ? old_size : size);
    do_free(ptr);
    return result;
  }
  if (__builtin_expect(need_sample(size), 0)) {
    void *result = sample_allocate(size, 16, false);
    if (result != nullptr) {
      auto old_size = do_usable_size(ptr);
      std::memcpy(result, ptr, old_size < size ? old_size : size);
      do_free(ptr);
      return result;
    }
  }
  if (__builtin_expect(audit_mode, 0)) {
    return audit_reallocate(ptr, size);
  }
  auto old_real_size = get_real_size(ptr);
  void *result = real.realloc_(ptr, size);
  if (result != nullptr) {
    add_live_bytes(get_real_size(result) - old_real_size);
  }
  return result;
}

void *reallocarray(void *ptr, std::size_t count, std::size_t size) noexcept {
  std::size_t total;
  if (__builtin_mul_overflow(count, size, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  return realloc(ptr, total);
}

void *memalign(std::size_t alignment, std::size_t size) noexcept {
  return telegram_bot_api::do_memalign(alignment, size);
}

int posix_memalign(void **result, std::size_t alignment, std::size_t size) noexcept {
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  void *ptr = telegram_bot_api::do_memalign(alignment, size);
  if (ptr == nullptr) {
    return ENOMEM;
  }
  *result = ptr;
  return 0;
}

void *aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
    errno = EINVAL;
    return nullptr;
  }
  return telegram_bot_api::do_memalign(alignment, size);
}

void *valloc(std::size_t size) noexcept {
  return telegram_bot_api::do_memalign(telegram_bot_api::HEAP_PAGE_SIZE, size);
}

void *pvalloc(std::size_t size) noexcept {
  auto page_size = telegram_bot_api::HEAP_PAGE_SIZE;
  if (size > ~static_cast<std::size_t>(0) - page_size) {
    errno = ENOMEM;
    return nullptr;
  }
  size = size == 0 ? page_size : (size + page_size - 1) & ~(page_size - 1);
  return telegram_bot_api::do_memalign(page_size, size);
}

std::size_t malloc_usable_size(void *ptr) noexcept {
  using namespace telegram_bot_api;
  if (__builtin_expect(init_state.load(std::memory_order_acquire) != 2, 0) && !ensure_initialized()) {
    return ptr != nullptr && is_bootstrap(ptr) ? get_bootstrap_size(ptr) : 0;
  }
  return do_usable_size(ptr);
}

}  // extern "C"
#endif
