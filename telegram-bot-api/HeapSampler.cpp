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
#include <new>

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

// Everything here can be called from any allocation function, so nothing here may allocate memory with malloc.
//
// Sampled blocks are allocated in a reserved address region, so free() recognizes them by their address:
//  - blocks of at most MAX_SMALL_SLOT_SIZE - SMALL_HEADER_SIZE bytes with an alignment of at most 16 bytes are slots
//    in slab pages; the SampleInfo of a slot is in the SMALL_HEADER_SIZE bytes before it; slots are never page-aligned;
//  - other blocks are page runs of 2^k pages; the SampleInfo of a run is in run_infos of its first page.
// Invariants of page runs:
//  - a run in free_runs contains only zeros, because its memory was returned to the OS; a run in dirty_runs doesn't;
//  - pages of a run after its resident_bytes weren't used since the run contained only zeros;
//  - if committed_bytes of a run isn't 0, then the block in the run isn't bigger than committed_bytes.

constexpr std::size_t HEAP_PAGE_SIZE = 4096;
constexpr std::uintptr_t REGION_SIZE = static_cast<std::uintptr_t>(1) << 38;  // address space for sampled blocks
constexpr td::uint32 REGION_PAGE_COUNT = static_cast<td::uint32>(REGION_SIZE / HEAP_PAGE_SIZE);
constexpr std::size_t MAX_SAMPLED_SIZE = REGION_SIZE / 4;  // bigger blocks are never sampled
constexpr td::int32 RUN_CLASS_COUNT = 25;                  // runs of up to 2^24 pages, i.e. MAX_SAMPLED_SIZE
static_assert(HEAP_PAGE_SIZE << (RUN_CLASS_COUNT - 1) == MAX_SAMPLED_SIZE, "");
constexpr std::size_t SMALL_HEADER_SIZE = 32;  // SampleInfo, padded to keep 16-byte alignment of slots
constexpr std::size_t MAX_SMALL_SLOT_SIZE = 2048;
constexpr std::size_t SMALL_CLASS_COUNT = MAX_SMALL_SLOT_SIZE / 16 + 1;
constexpr td::uint32 NO_PAGE = 0xFFFFFFFFu;
constexpr td::int64 DISABLED_RECHECK_BYTES = 1 << 20;
constexpr td::int64 DEFAULT_SAMPLING_INTERVAL = static_cast<td::int64>(64) << 10;
constexpr td::int64 MAX_SAMPLING_INTERVAL = static_cast<td::int64>(1) << 40;
// sampled blocks of at least this size are mapped with commit accounting; glibc uses mmap for blocks of this size too
constexpr std::size_t COMMIT_THRESHOLD = static_cast<std::size_t>(32) << 20;
// freed runs of up to 1 MB are kept for reuse without returning their memory to the OS, while their total resident size
// is at most RUN_CACHE_LIMIT; madvise is slow and interrupts other CPUs to flush their TLBs
constexpr td::uint32 MAX_CACHED_RUN_CLASS = 8;
constexpr td::int64 RUN_CACHE_LIMIT = static_cast<td::int64>(8) << 20;
constexpr td::int32 MAX_OWNERS = 1 << 16;
constexpr std::size_t CONTEXT_TABLE_SIZE = 1 << 16;  // at most 3/4 of the entries are used
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
RealAllocator real;

std::atomic<td::int64> sampling_interval{0};
std::atomic<std::uintptr_t> region_begin{~static_cast<std::uintptr_t>(0) - REGION_SIZE + 1};  // unreachable range
std::atomic<bool> is_region_reserved{false};
pthread_mutex_t sampler_mutex = PTHREAD_MUTEX_INITIALIZER;  // protects owners, contexts and sampled blocks

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
    return false;  // an allocation made by dlsym or getenv during the initialization
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
    // an allocator without nallocx is assumed to be glibc, which stores the size of a chunk before it;
    // allocators with nallocx use size classes without per-allocation headers
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

// allocates zero-initialized static memory; the size of a block is stored 16 bytes before it
void *bootstrap_allocate(std::size_t size, std::size_t alignment) {
  if (alignment < 16) {
    alignment = 16;
  }
  if (size > BOOTSTRAP_SIZE || alignment > BOOTSTRAP_SIZE) {
    write_error("Bootstrap memory is exhausted\n");
    std::abort();
  }
  std::size_t total = (size + 16 + alignment + 15) & ~static_cast<std::size_t>(15);
  auto pos = bootstrap_used.fetch_add(total, std::memory_order_relaxed);
  if (pos + total > BOOTSTRAP_SIZE) {
    write_error("Bootstrap memory is exhausted\n");
    std::abort();
  }
  auto begin = reinterpret_cast<std::uintptr_t>(bootstrap_buffer + pos) + 16;
  auto *block = reinterpret_cast<char *>((begin + alignment - 1) & ~(alignment - 1));
  *reinterpret_cast<std::size_t *>(block - 16) = size;
  return block;
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

// size of memory, which would be used by the real allocator for an allocation of the given size; used for sampled
// blocks and in the audit mode
td::int64 get_chunk_size(std::size_t size) {
  if (real.nallocx_ != nullptr) {
    return static_cast<td::int64>(real.nallocx_(size == 0 ? 1 : size, 0));
  }
  auto chunk = get_glibc_chunk_size(size);
  if (chunk >= (static_cast<std::size_t>(128) << 10)) {
    // glibc allocates chunks of at least the initial mmap threshold of 128 KB with mmap, unless the threshold was
    // increased or the chunk fits into the top of the heap; then the real chunk is up to 4 KB smaller
    return static_cast<td::int64>(get_glibc_mmapped_chunk_size(size));
  }
  return static_cast<td::int64>(chunk);
}

__thread td::int64 countdown = 0;           // the number of bytes left before the next point of the sampling process
__thread td::int64 countdown_interval = 0;  // the interval used for the current countdown; 0 if sampling is disabled
__thread td::uint64 random_state = 0;
__thread td::int32 part_override = -1;  // set by PartGuard

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

// Points of a Poisson process with the mean distance of `interval` bytes are placed on the bytes allocated by the
// thread. The distance to the next point is exponentially distributed, i.e. P(distance > k) = exp(-k / interval)
// regardless of the past, so an allocation, which covers n + 1 bytes, contains a point with the probability
// 1 - exp(-(n + 1) / interval) independently of all other allocations.
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

td::int64 need_sample_slow(std::size_t size) {
  auto interval = countdown_interval;  // 0 if the countdown wasn't drawn from the distribution
  reset_countdown();
  return size <= MAX_SAMPLED_SIZE ? interval : 0;
}

// returns the sampling interval if the allocation must be sampled or 0 otherwise; an allocation of n bytes covers
// n + 1 bytes of the sampling process, so even empty allocations can be sampled
inline td::int64 need_sample(std::size_t size) {
  auto left = countdown;
  if (__builtin_expect(static_cast<td::uint64>(left) > static_cast<td::uint64>(size) + 1, 1)) {
    countdown = left - static_cast<td::int64>(size) - 1;
    return 0;
  }
  return need_sample_slow(size);
}

inline bool is_sampled_block(const void *ptr) {
  return reinterpret_cast<std::uintptr_t>(ptr) - region_begin.load(std::memory_order_relaxed) < REGION_SIZE;
}

struct SampleInfo {
  td::uint64 size;   // requested size
  td::int64 chunk;   // size of memory, which would be used by the real allocator
  td::int64 weight;  // chunk / probability of sampling: estimated size of all allocations represented by the sample
  td::uint32 generation;  // generation of the owner
  td::uint16 owner;       // 0 if the allocation wasn't made by an owner
  td::uint8 part;
  td::uint8 small_class;  // class of a small slot or 0 for page runs
};
static_assert(sizeof(SampleInfo) <= SMALL_HEADER_SIZE, "");
static_assert(MAX_OWNERS <= 65536, "");

struct RunInfo {
  SampleInfo sample;
  td::uint64 committed_bytes;  // size of the mapping with commit accounting at the beginning of the run, or 0
  td::uint64 resident_bytes;   // size of the beginning of the run, which could have been used since it was zero
  td::uint32 run_class;        // the run has 2^run_class pages
  td::uint32 next_free;        // the next run in the list of free runs
};
RunInfo *run_infos = nullptr;  // for each page of the region

// all fields are protected by the lock, except generation, which is read without the lock only with atomic builtins
struct OwnerInfo {
  td::int64 bytes[HeapSampler::PART_COUNT];  // sum of weights of live samples
  td::int64 sample_count;
  __int128 variance;      // sum of weight * (weight - chunk) of live samples
  td::uint32 generation;  // changed when the owner is created and destroyed
  bool is_used;
  td::int64 audit_baseline[HeapSampler::PART_COUNT];  // values of the exact tallies when the owner was created
};
OwnerInfo owners[MAX_OWNERS];  // owners[0] accumulates unattributed allocations
td::int32 free_owners[MAX_OWNERS];
td::int32 free_owner_count = 0;
td::int32 next_owner = 1;

// open addressing hash table of actor contexts with linear probing; protected by the lock
struct ContextEntry {
  td::uint64 context;  // 0 for empty entries
  td::int32 owner;
  td::int32 part;
  td::uint32 generation;  // generation of the owner; contexts of destroyed owners are ignored
};
ContextEntry context_table[CONTEXT_TABLE_SIZE];
std::size_t context_count = 0;
std::atomic<td::uint64> context_version{1};  // changed whenever a context or an owner is changed

// state of the sampled blocks allocator, protected by the lock
td::uint32 next_region_page = 0;
td::uint32 free_runs[RUN_CLASS_COUNT];   // runs, memory of which was returned to the OS
td::uint32 dirty_runs[RUN_CLASS_COUNT];  // runs, memory of which wasn't returned to the OS
void *free_small_slots[SMALL_CLASS_COUNT];
char *small_slab_pos[SMALL_CLASS_COUNT];
char *small_slab_end[SMALL_CLASS_COUNT];
td::int64 slab_page_count = 0;      // slab pages are never freed
td::int64 cached_run_bytes = 0;     // resident size of dirty_runs
td::int64 large_run_bytes = 0;      // resident size of live page runs
td::int64 total_sampled_bytes = 0;  // sum of chunk sizes of live sampled blocks

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

// measures the duration of an allocation or a deallocation of a sampled block
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

constexpr std::size_t CONTEXT_MASK = CONTEXT_TABLE_SIZE - 1;

std::size_t get_context_home(td::uint64 context) {
  return static_cast<std::size_t>(((context >> 4) * 0x9E3779B97F4A7C15ULL) >> 48) & CONTEXT_MASK;
}

// returns the entry of the context or the empty entry, where the context must be inserted; must be called under the
// lock; at least 1/4 of the entries are empty, so the search always stops
std::size_t find_context_position(td::uint64 context) {
  auto pos = get_context_home(context);
  while (context_table[pos].context != context && context_table[pos].context != 0) {
    pos = (pos + 1) & CONTEXT_MASK;
  }
  return pos;
}

// removes the entry and moves the following entries back to keep them reachable; must be called under the lock
void erase_context(std::size_t hole) {
  for (auto pos = (hole + 1) & CONTEXT_MASK; context_table[pos].context != 0; pos = (pos + 1) & CONTEXT_MASK) {
    // the entry can be moved to the hole if the hole is between the home position of the entry and its position
    auto home = get_context_home(context_table[pos].context);
    if (((pos - home) & CONTEXT_MASK) >= ((pos - hole) & CONTEXT_MASK)) {
      context_table[hole] = context_table[pos];
      hole = pos;
    }
  }
  context_table[hole].context = 0;
  context_count--;
}

// returns the owner of allocations made in the context; must be called under the lock
void get_context_owner(td::uint64 context, td::int32 &owner, td::int32 &part) {
  owner = 0;
  part = 0;
  if (context == 0) {
    return;
  }
  const auto &entry = context_table[find_context_position(context)];
  if (entry.context == context && owners[entry.owner].generation == entry.generation) {
    owner = entry.owner;
    part = part_override >= 0 ? part_override : entry.part;
  }
}

// exact tallies of memory of owners, counted separately by each thread; used only in the audit mode
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

// sparse mappings must not be collapsed into huge pages and dumped into core files; mmap with MAP_FIXED creates a new
// mapping, so the advice must be given again after it
void advise_sparse_mapping(void *begin, std::size_t size) {
  madvise(begin, size, MADV_NOHUGEPAGE);
  madvise(begin, size, MADV_DONTDUMP);
}

// maps pages with commit accounting, so a big block can't be allocated if there is not enough memory
bool commit_pages(char *begin, std::size_t size) {
  if (mmap(begin, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
    return false;
  }
  advise_sparse_mapping(begin, size);
  return true;
}

// returns memory to the OS; free() must not change errno
void decommit_pages(char *begin, std::size_t size, bool is_committed) {
  if (size == 0) {
    return;
  }
  auto saved_errno = errno;
  if (is_committed && mmap(begin, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
                           -1, 0) != MAP_FAILED) {
    advise_sparse_mapping(begin, size);
  } else {
    madvise(begin, size, MADV_DONTNEED);
  }
  errno = saved_errno;
}

std::size_t round_to_pages(std::size_t size) {
  return (size + HEAP_PAGE_SIZE - 1) & ~(HEAP_PAGE_SIZE - 1);
}

td::uint32 get_page(const void *ptr) {
  return static_cast<td::uint32>(
      (reinterpret_cast<std::uintptr_t>(ptr) - region_begin.load(std::memory_order_relaxed)) / HEAP_PAGE_SIZE);
}

char *get_page_address(td::uint32 page) {
  return reinterpret_cast<char *>(region_begin.load(std::memory_order_relaxed) + page * HEAP_PAGE_SIZE);
}

td::uint32 get_run_class(std::size_t size) {
  td::uint64 page_count = (size + HEAP_PAGE_SIZE - 1) / HEAP_PAGE_SIZE;
  td::uint32 run_class = 0;
  while ((static_cast<td::uint64>(1) << run_class) < page_count) {
    run_class++;
  }
  return run_class;
}

// returns a run and whether it contains only zeros; must be called under the lock
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
    } else {
      td::uint64 page_count = static_cast<td::uint64>(1) << run_class;
      if (next_region_page + page_count > REGION_PAGE_COUNT) {
        return nullptr;
      }
      page = next_region_page;
      next_region_page += static_cast<td::uint32>(page_count);
    }
    is_zero = true;
    run_infos[page].resident_bytes = 0;
  }
  run_infos[page].run_class = run_class;
  run_infos[page].committed_bytes = 0;
  return get_page_address(page);
}

// adds a run, which contains only zeros, to the free runs; must be called under the lock
void release_run(td::uint32 page) {
  auto run_class = run_infos[page].run_class;
  run_infos[page].next_free = free_runs[run_class];
  free_runs[run_class] = page;
}

// keeps a freed run with its memory if possible; must be called under the lock
bool cache_dirty_run(td::uint32 page) {
  auto &run_info = run_infos[page];
  if (run_info.run_class > MAX_CACHED_RUN_CLASS || run_info.committed_bytes != 0 ||
      cached_run_bytes + static_cast<td::int64>(run_info.resident_bytes) > RUN_CACHE_LIMIT) {
    return false;
  }
  run_info.next_free = dirty_runs[run_info.run_class];
  dirty_runs[run_info.run_class] = page;
  cached_run_bytes += static_cast<td::int64>(run_info.resident_bytes);
  return true;
}

// returns a free small slot of the given class; must be called under the lock
char *allocate_small_slot(std::size_t small_class) {
  auto slot_size = small_class * 16;
  char *slot = static_cast<char *>(free_small_slots[small_class]);
  if (slot != nullptr) {
    free_small_slots[small_class] = *reinterpret_cast<void **>(slot);
    return slot;
  }
  if (small_slab_pos[small_class] == nullptr || small_slab_pos[small_class] + slot_size > small_slab_end[small_class]) {
    bool is_zero;
    char *page = allocate_run(0, is_zero);
    if (page == nullptr) {
      return nullptr;
    }
    slab_page_count++;
    small_slab_pos[small_class] = page;
    small_slab_end[small_class] = page + HEAP_PAGE_SIZE;
  }
  slot = small_slab_pos[small_class];
  small_slab_pos[small_class] += slot_size;
  return slot;
}

// adds or removes a sample from the memory of its owner; must be called under the lock
void add_owner_sample(SampleInfo &info, td::uint64 context, td::int32 sign) {
  if (sign > 0) {
    td::int32 owner;
    td::int32 part;
    get_context_owner(context, owner, part);
    info.owner = static_cast<td::uint16>(owner);
    info.generation = owners[owner].generation;
    info.part = static_cast<td::uint8>(part);
  }
  td::int32 owner = info.owner;
  if (owner != 0 && owners[owner].generation != info.generation) {
    owner = 0;  // the owner was destroyed and its memory was moved to the unattributed memory
  }
  auto &owner_info = owners[owner];
  owner_info.bytes[info.part] += sign * info.weight;
  owner_info.sample_count += sign;
  // unbiased estimate of the variance of the weight: chunk^2 * (1 - p) / p^2 == weight * (weight - chunk)
  owner_info.variance += sign * static_cast<__int128>(info.weight) * (info.weight - info.chunk);
  total_sampled_bytes += sign * info.chunk;
  if (audit_mode) {
    add_audit_bytes(owner, info.part, sign * info.chunk);
  }
}

td::int64 get_sample_weight(std::size_t size, td::int64 chunk, td::int64 interval) {
  double probability = -std::expm1(-static_cast<double>(size + 1) / static_cast<double>(interval));
  return static_cast<td::int64>(std::llround(static_cast<double>(chunk) / probability));
}

// registers a new sample; must be called under the lock
void add_sample(SampleInfo &info, std::size_t size, td::int64 interval) {
  info.size = size;
  info.chunk = get_chunk_size(size);
  info.weight = get_sample_weight(size, info.chunk, interval);
  add_owner_sample(info, reinterpret_cast<td::uint64>(td::Scheduler::context()), 1);
}

// allocates a sampled block with an alignment of at most HEAP_PAGE_SIZE; returns nullptr if it isn't possible
void *sample_allocate(std::size_t size, std::size_t alignment, bool zero, td::int64 interval) {
  OperationTimer timer;
  char *result;
  bool is_zero = false;
  if (alignment <= 16 && size + SMALL_HEADER_SIZE <= MAX_SMALL_SLOT_SIZE) {
    // slots have at least 16 bytes of data, so the last slot of a page doesn't end at the page boundary and
    // pointers to small blocks are never page-aligned
    auto small_class = td::max((size + SMALL_HEADER_SIZE + 15) / 16, SMALL_HEADER_SIZE / 16 + 1);
    SamplerLock lock;
    char *slot = allocate_small_slot(small_class);
    if (slot == nullptr) {
      return nullptr;
    }
    auto *info = reinterpret_cast<SampleInfo *>(slot);
    info->small_class = static_cast<td::uint8>(small_class);
    add_sample(*info, size, interval);
    result = slot + SMALL_HEADER_SIZE;
  } else {
    td::uint32 page;
    {
      SamplerLock lock;
      result = allocate_run(get_run_class(size), is_zero);
      if (result == nullptr) {
        return nullptr;
      }
      page = get_page(result);
    }
    // nobody else uses the run now, so mmap, which is slow, is done without the lock
    auto &run_info = run_infos[page];
    auto mapped_size = round_to_pages(size);
    if (mapped_size >= COMMIT_THRESHOLD) {
      if (!commit_pages(result, mapped_size)) {
        SamplerLock lock;
        release_run(page);  // the run is still zero
        return nullptr;
      }
      run_info.committed_bytes = mapped_size;
    }
    SamplerLock lock;
    run_info.resident_bytes = td::max(run_info.resident_bytes, static_cast<td::uint64>(mapped_size));
    large_run_bytes += static_cast<td::int64>(run_info.resident_bytes);
    run_info.sample.small_class = 0;
    add_sample(run_info.sample, size, interval);
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
  auto &run_info = run_infos[get_page(ptr)];
  auto mapped_size = round_to_pages(size);
  // pages of a block, which isn't committed, can't be committed without losing their contents
  if (mapped_size > (HEAP_PAGE_SIZE << run_info.run_class) ||
      (run_info.committed_bytes == 0 && mapped_size >= COMMIT_THRESHOLD)) {
    return false;
  }
  OperationTimer timer;
  // nobody else uses the run now, so mmap and madvise are done without the lock; unused pages are returned to the OS
  auto *begin = static_cast<char *>(ptr);
  if (run_info.committed_bytes != 0) {
    if (mapped_size > run_info.committed_bytes) {
      if (!commit_pages(begin + run_info.committed_bytes, mapped_size - run_info.committed_bytes)) {
        return false;
      }
    } else {
      decommit_pages(begin + mapped_size, run_info.committed_bytes - mapped_size, true);
    }
    run_info.committed_bytes = mapped_size;
  } else if (mapped_size < run_info.resident_bytes) {
    decommit_pages(begin + mapped_size, run_info.resident_bytes - mapped_size, false);
  }

  SamplerLock lock;
  large_run_bytes += static_cast<td::int64>(mapped_size) - static_cast<td::int64>(run_info.resident_bytes);
  run_info.resident_bytes = mapped_size;
  add_owner_sample(run_info.sample, 0, -1);
  add_sample(run_info.sample, size, interval);
  return true;
}

void sample_free(void *ptr) {
  OperationTimer timer;
  if ((reinterpret_cast<std::uintptr_t>(ptr) & (HEAP_PAGE_SIZE - 1)) != 0) {
    char *slot = static_cast<char *>(ptr) - SMALL_HEADER_SIZE;
    auto &info = *reinterpret_cast<SampleInfo *>(slot);
    SamplerLock lock;
    add_owner_sample(info, 0, -1);
    *reinterpret_cast<void **>(slot) = free_small_slots[info.small_class];
    free_small_slots[info.small_class] = slot;
    return;
  }

  auto page = get_page(ptr);
  auto &run_info = run_infos[page];
  {
    SamplerLock lock;
    add_owner_sample(run_info.sample, 0, -1);
    large_run_bytes -= static_cast<td::int64>(run_info.resident_bytes);
    if (cache_dirty_run(page)) {
      return;
    }
  }
  // return memory to the OS without the lock and before the run can be reused by another thread
  if (run_info.committed_bytes != 0) {
    decommit_pages(static_cast<char *>(ptr), run_info.committed_bytes, true);
  } else {
    decommit_pages(static_cast<char *>(ptr), run_info.resident_bytes, false);
  }
  SamplerLock lock;
  release_run(page);
}

std::size_t get_sampled_size(const void *ptr) {
  if ((reinterpret_cast<std::uintptr_t>(ptr) & (HEAP_PAGE_SIZE - 1)) != 0) {
    return static_cast<std::size_t>(
        reinterpret_cast<const SampleInfo *>(static_cast<const char *>(ptr) - SMALL_HEADER_SIZE)->size);
  }
  return static_cast<std::size_t>(run_infos[get_page(ptr)].sample.size);
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
  if (sysconf(_SC_PAGESIZE) != static_cast<long>(HEAP_PAGE_SIZE)) {
    write_error("Memory accounting by bots requires memory pages of 4 KB\n");
    return false;
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
  advise_sparse_mapping(region, REGION_SIZE);
  advise_sparse_mapping(infos, infos_size);
  run_infos = static_cast<RunInfo *>(infos);
  for (td::int32 i = 0; i < RUN_CLASS_COUNT; i++) {
    free_runs[i] = NO_PAGE;
    dirty_runs[i] = NO_PAGE;
  }
  region_begin.store(reinterpret_cast<std::uintptr_t>(region), std::memory_order_relaxed);
  is_region_reserved.store(true);
  return true;
}

// owner of allocations made in the current actor context; cached by each thread; used only in the audit mode
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
    auto saved_part_override = part_override;
    part_override = -1;
    get_context_owner(context, entry.owner, entry.part);
    part_override = saved_part_override;
    entry.generation = owners[entry.owner].generation;
  }
  owner = entry.owner;
  part = entry.owner != 0 && part_override >= 0 ? part_override : entry.part;
  generation = entry.generation;
}

// In the audit mode every block, which isn't sampled, has a header with its owner before it. The block of the real
// allocator starts 2^shift bytes before the returned pointer, so the returned pointer has the requested alignment.
struct AuditHeader {
  td::int64 chunk;  // size accounted for the owner
  td::uint32 generation;
  td::uint16 owner;
  td::uint8 part;
  td::uint8 shift;
};
static_assert(sizeof(AuditHeader) == AUDIT_HEADER_SIZE, "");

AuditHeader *get_audit_header(void *ptr) {
  return reinterpret_cast<AuditHeader *>(static_cast<char *>(ptr) - AUDIT_HEADER_SIZE);
}

void *get_audit_base(void *ptr) {
  return static_cast<char *>(ptr) - (static_cast<std::size_t>(1) << get_audit_header(ptr)->shift);
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
  void *base = alignment != 0 ? real.memalign_(prefix, size + prefix)
               : zero         ? real.calloc_(1, size + prefix)
                              : real.malloc_(size + prefix);
  if (base == nullptr) {
    return nullptr;
  }
  add_live_bytes(get_real_size(base));
  auto *result = static_cast<char *>(base) + prefix;
  if (zero && alignment != 0) {
    std::memset(result, 0, size);
  }
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

void audit_free(void *ptr) {
  const auto &header = *get_audit_header(ptr);
  td::int32 owner = header.owner;
  if (owner != 0 && __atomic_load_n(&owners[owner].generation, __ATOMIC_RELAXED) != header.generation) {
    owner = 0;  // the owner was destroyed
  }
  add_audit_bytes(owner, header.part, -header.chunk);
  void *base = get_audit_base(ptr);
  add_live_bytes(-get_real_size(base));
  real.free_(base);
}

std::size_t get_audit_usable_size(void *ptr) {
  return real.malloc_usable_size_(get_audit_base(ptr)) - (static_cast<std::size_t>(1) << get_audit_header(ptr)->shift);
}

// allocates a block; alignment 0 means the alignment of malloc, otherwise it must be a power of 2
void *do_allocate(std::size_t size, std::size_t alignment, bool zero) {
  if (__builtin_expect(init_state.load(std::memory_order_acquire) != 2, 0) && !ensure_initialized()) {
    return bootstrap_allocate(size, alignment);  // static memory is zero-initialized
  }
  if (__builtin_expect(alignment <= HEAP_PAGE_SIZE, 1)) {  // blocks with a bigger alignment are never sampled
    auto interval = need_sample(size);
    if (__builtin_expect(interval != 0, 0)) {
      void *result = sample_allocate(size, alignment, zero, interval);
      if (result != nullptr) {
        return result;
      }
    }
  }
  if (__builtin_expect(audit_mode, 0)) {
    return audit_allocate(size, alignment, zero);
  }
  void *result = alignment != 0 ? real.memalign_(alignment, size) : zero ? real.calloc_(1, size) : real.malloc_(size);
  if (result != nullptr) {
    if (zero && alignment != 0) {
      std::memset(result, 0, size);
    }
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

void *do_memalign(std::size_t alignment, std::size_t size) {
  if (size == 0) {
    size = 1;  // jemalloc doesn't align empty allocations
  }
  return do_allocate(size, alignment, false);
}

// copies a block to the newly allocated block and frees it; the old block stays valid if the allocation failed
void *move_block(void *ptr, std::size_t size, void *result) {
  if (result != nullptr) {
    auto old_size = do_usable_size(ptr);
    std::memcpy(result, ptr, old_size < size ? old_size : size);
    do_free(ptr);
  }
  return result;
}

void *do_reallocate(void *ptr, std::size_t size) {
  if (ptr == nullptr) {
    return do_allocate(size, 0, false);
  }
  if (size == 0) {
    do_free(ptr);
    return nullptr;
  }
  if (is_sampled_block(ptr) || is_bootstrap(ptr)) {
    if (is_sampled_block(ptr) && sample_reallocate_in_place(ptr, size)) {
      return ptr;
    }
    return move_block(ptr, size, do_allocate(size, 0, false));
  }
  auto interval = need_sample(size);
  if (__builtin_expect(interval != 0, 0)) {
    void *result = sample_allocate(size, 0, false, interval);
    if (result != nullptr) {
      return move_block(ptr, size, result);
    }
  }
  if (__builtin_expect(audit_mode, 0)) {
    return move_block(ptr, size, audit_allocate(size, 0, false));
  }
  auto old_real_size = get_real_size(ptr);
  void *result = real.realloc_(ptr, size);
  if (result != nullptr) {
    add_live_bytes(get_real_size(result) - old_real_size);
  }
  return result;
}

// a replacement of operator new must call the new handler until the allocation succeeds; without the handler it must
// throw std::bad_alloc, which terminates the server, because it is built without exceptions
void *allocate_for_new(std::size_t size, std::size_t alignment, bool is_nothrow) {
  while (true) {
    void *result = alignment == 0 ? do_allocate(size, 0, false) : do_memalign(alignment, size);
    if (result != nullptr) {
      return result;
    }
    auto handler = std::get_new_handler();
    if (handler == nullptr) {
      if (is_nothrow) {
        return nullptr;
      }
      write_error("Failed to allocate memory\n");
      std::abort();
    }
    handler();
  }
}

}  // namespace

bool HeapSampler::is_supported() {
  return true;
}

bool HeapSampler::is_audit_enabled() {
  return audit_mode;
}

void HeapSampler::set_sampling_interval(td::int64 interval) {
  if (interval > MAX_SAMPLING_INTERVAL) {
    interval = MAX_SAMPLING_INTERVAL;
  }
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
    bytes = 0;
  }
  info.sample_count = 0;
  info.variance = 0;
  if (audit_mode) {
    // tallies of the slot can contain memory of destroyed owners, which isn't memory of the new owner
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
    unattributed.bytes[part] += info.bytes[part];
    info.bytes[part] = 0;
  }
  unattributed.sample_count += info.sample_count;
  info.sample_count = 0;
  unattributed.variance += info.variance;
  info.variance = 0;
  // samples and contexts with the previous generation now belong to the unattributed memory
  __atomic_store_n(&info.generation, info.generation + 1, __ATOMIC_RELAXED);
  info.is_used = false;
  free_owners[free_owner_count++] = owner;
  context_version.fetch_add(1, std::memory_order_release);
}

void HeapSampler::set_context_owner(td::uint64 context, td::int32 owner, td::int32 part) {
  if (context == 0 || owner <= 0 || owner >= MAX_OWNERS || part < 0 || part >= PART_COUNT) {
    return;
  }
  SamplerLock lock;
  auto pos = find_context_position(context);
  auto &entry = context_table[pos];
  if (entry.context == 0) {
    if ((context_count + 1) * 4 > CONTEXT_TABLE_SIZE * 3) {
      return;  // too many contexts; allocations in the context will be unattributed
    }
    context_count++;
  }
  entry = ContextEntry{context, owner, part, owners[owner].generation};
  context_version.fetch_add(1, std::memory_order_release);
}

void HeapSampler::remove_context(td::uint64 context, td::int32 owner) {
  if (context == 0) {
    return;
  }
  SamplerLock lock;
  auto pos = find_context_position(context);
  if (context_table[pos].context == context && context_table[pos].owner == owner) {
    erase_context(pos);
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
    result.bytes[part] = info.bytes[part];
    if (audit_mode && owner != 0) {
      result.exact_bytes[part] = sum_audit_tallies(owner, part) - info.audit_baseline[part];
    }
  }
  result.sample_count = info.sample_count;
  result.variance = static_cast<double>(info.variance);
  return result;
}

HeapSampler::Stats HeapSampler::get_stats() {
  Stats result;
  auto counter_count = td::min(thread_counter_count.load(std::memory_order_relaxed), MAX_THREAD_COUNTERS);
  for (std::size_t i = 0; i < counter_count; i++) {
    result.live_bytes += thread_counters[i].value.load(std::memory_order_relaxed);
  }
  result.live_bytes += shared_counter.load(std::memory_order_relaxed);

  SamplerLock lock;
  result.live_bytes += total_sampled_bytes;
  result.sampled_bytes = total_sampled_bytes;
  __int128 variance = 0;
  for (td::int32 owner = 0; owner < next_owner; owner++) {
    const auto &info = owners[owner];
    for (td::int32 part = 0; part < PART_COUNT; part++) {
      result.estimated_live_bytes += info.bytes[part];
      if (audit_mode) {
        // every live allocation is accounted in exactly one slot, including allocations of destroyed owners
        result.audit_live_bytes += sum_audit_tallies(owner, part);
      }
    }
    result.sample_count += info.sample_count;
    variance += info.variance;
  }
  result.estimate_variance = static_cast<double>(variance);
  result.sampler_overhead_bytes = td::max(slab_page_count * static_cast<td::int64>(HEAP_PAGE_SIZE) + cached_run_bytes +
                                              large_run_bytes - total_sampled_bytes,
                                          static_cast<td::int64>(0));
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
// Replacements of the memory allocation functions; see "Replacing malloc" in the GNU C Library manual. The C++
// operators are replaced too, because allocators like jemalloc define their own operators, which don't call malloc.
extern "C" {

void *malloc(std::size_t size) noexcept {
  return telegram_bot_api::do_allocate(size, 0, false);
}

void free(void *ptr) noexcept {
  telegram_bot_api::do_free(ptr);
}

void *calloc(std::size_t count, std::size_t size) noexcept {
  std::size_t total;
  if (__builtin_mul_overflow(count, size, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  return telegram_bot_api::do_allocate(total, 0, true);
}

void *realloc(void *ptr, std::size_t size) noexcept {
  return telegram_bot_api::do_reallocate(ptr, size);
}

void *reallocarray(void *ptr, std::size_t count, std::size_t size) noexcept {
  std::size_t total;
  if (__builtin_mul_overflow(count, size, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  return telegram_bot_api::do_reallocate(ptr, total);
}

void *memalign(std::size_t alignment, std::size_t size) noexcept {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
    // glibc rounds the alignment up to a power of 2
    std::size_t power = 1;
    while (power < alignment && power != 0) {
      power <<= 1;
    }
    if (power == 0) {
      errno = EINVAL;
      return nullptr;
    }
    alignment = power;
  }
  return telegram_bot_api::do_memalign(alignment, size);
}

int posix_memalign(void **result, std::size_t alignment, std::size_t size) noexcept {
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  auto saved_errno = errno;
  void *ptr = telegram_bot_api::do_memalign(alignment, size);
  errno = saved_errno;
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

void *operator new(std::size_t size) {
  return telegram_bot_api::allocate_for_new(size, 0, false);
}

void *operator new[](std::size_t size) {
  return telegram_bot_api::allocate_for_new(size, 0, false);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  return telegram_bot_api::allocate_for_new(size, 0, true);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return telegram_bot_api::allocate_for_new(size, 0, true);
}

void *operator new(std::size_t size, std::align_val_t alignment) {
  return telegram_bot_api::allocate_for_new(size, static_cast<std::size_t>(alignment), false);
}

void *operator new[](std::size_t size, std::align_val_t alignment) {
  return telegram_bot_api::allocate_for_new(size, static_cast<std::size_t>(alignment), false);
}

void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept {
  return telegram_bot_api::allocate_for_new(size, static_cast<std::size_t>(alignment), true);
}

void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept {
  return telegram_bot_api::allocate_for_new(size, static_cast<std::size_t>(alignment), true);
}

void operator delete(void *ptr) noexcept {
  telegram_bot_api::do_free(ptr);
}

void operator delete[](void *ptr) noexcept {
  telegram_bot_api::do_free(ptr);
}

void operator delete(void *ptr, std::size_t) noexcept {
  telegram_bot_api::do_free(ptr);
}

void operator delete[](void *ptr, std::size_t) noexcept {
  telegram_bot_api::do_free(ptr);
}

void operator delete(void *ptr, const std::nothrow_t &) noexcept {
  telegram_bot_api::do_free(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t &) noexcept {
  telegram_bot_api::do_free(ptr);
}

void operator delete(void *ptr, std::align_val_t) noexcept {
  telegram_bot_api::do_free(ptr);
}

void operator delete[](void *ptr, std::align_val_t) noexcept {
  telegram_bot_api::do_free(ptr);
}

void operator delete(void *ptr, std::size_t, std::align_val_t) noexcept {
  telegram_bot_api::do_free(ptr);
}

void operator delete[](void *ptr, std::size_t, std::align_val_t) noexcept {
  telegram_bot_api::do_free(ptr);
}

void operator delete(void *ptr, std::align_val_t, const std::nothrow_t &) noexcept {
  telegram_bot_api::do_free(ptr);
}

void operator delete[](void *ptr, std::align_val_t, const std::nothrow_t &) noexcept {
  telegram_bot_api::do_free(ptr);
}
#endif
