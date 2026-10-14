//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Verification of HeapSampler: correctness of all allocation functions under concurrent use, unbiasedness and
// variance of estimates for different size distributions, attribution to owners, absence of drift, overhead.
//
// Usage: heap-sampler-test [stress] [exact] [attribution] [drift] [bias] [accuracy] [overhead]
//
#include "telegram-bot-api/HeapSampler.h"

#include "td/actor/actor.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using telegram_bot_api::HeapSampler;

namespace {

int failures = 0;

#define CHECK_TEST(condition, ...)            \
  do {                                        \
    if (!(condition)) {                       \
      std::printf("FAILED %s: ", #condition); \
      std::printf(__VA_ARGS__);               \
      std::printf(" at line %d\n", __LINE__); \
      failures++;                             \
    }                                         \
  } while (false)

// prevents the compiler from removing allocations, which aren't used
void *escape(void *ptr) {
  asm volatile("" : : "g"(ptr) : "memory");
  return ptr;
}

void set_context(std::uint64_t context) {
  td::Scheduler::context() = reinterpret_cast<td::ActorContext *>(context);
}

std::size_t (*real_nallocx)(std::size_t, int) = nullptr;

// size of memory used by the real allocator for an allocation, as defined by HeapSampler
double chunk_size(std::size_t size) {
  if (real_nallocx != nullptr) {
    return static_cast<double>(real_nallocx(size == 0 ? 1 : size, 0));
  }
  auto chunk = (size + 8 + 15) & ~static_cast<std::size_t>(15);
  if (chunk < 32) {
    chunk = 32;
  }
  if (size + 16 >= (static_cast<std::size_t>(128) << 10)) {
    return static_cast<double>((chunk + 8 + 4095) & ~static_cast<std::size_t>(4095));
  }
  return static_cast<double>(chunk);
}

double sample_variance(std::size_t size, double interval) {
  double p = -std::expm1(-static_cast<double>(size + 1) / interval);
  double c = chunk_size(size);
  return c * c * (1 - p) / p;
}

int (*real_mallctl)(const char *, void *, std::size_t *, void *, std::size_t) = nullptr;

// memory in use according to the real allocator; slow, but exact
long long glibc_in_use() {
  if (real_mallctl != nullptr) {
    // jemalloc
    std::uint64_t epoch = 1;
    std::size_t epoch_size = sizeof(epoch);
    real_mallctl("epoch", &epoch, &epoch_size, &epoch, epoch_size);
    std::size_t allocated = 0;
    std::size_t allocated_size = sizeof(allocated);
    real_mallctl("stats.allocated", &allocated, &allocated_size, nullptr, 0);
    return static_cast<long long>(allocated);
  }
  auto info = mallinfo2();
  return static_cast<long long>(info.uordblks + info.hblkhd);
}

// checks that the exact counter of HeapSampler changed in the same way as memory in use according to glibc
void check_exact_counter(const char *name, const HeapSampler::Stats &before, long long glibc_before,
                         long long tolerance) {
  auto after = HeapSampler::get_stats();
  auto glibc_after = glibc_in_use();
  long long counted = after.live_bytes - before.live_bytes;
  long long sampled = after.sampled_bytes - before.sampled_bytes;
  std::printf("%s: exact counter change %lld, glibc change %lld + sampled blocks %lld\n", name, counted,
              glibc_after - glibc_before, sampled);
  if (real_nallocx == nullptr || real_mallctl != nullptr) {
    CHECK_TEST(std::llabs(counted - (glibc_after - glibc_before + sampled)) <= tolerance,
               "%s: exact counter differs from the allocator by %lld", name,
               counted - (glibc_after - glibc_before + sampled));
  }
}

std::uint64_t pattern(const void *ptr, std::size_t i) {
  return (reinterpret_cast<std::uintptr_t>(ptr) * 0x9E3779B97F4A7C15ULL) ^ (i * 0xC2B2AE3D27D4EB4FULL);
}

void fill(void *ptr, std::size_t size) {
  auto *bytes = static_cast<unsigned char *>(ptr);
  for (std::size_t i = 0; i < size; i += 61) {
    bytes[i] = static_cast<unsigned char>(pattern(ptr, i));
  }
  if (size > 0) {
    bytes[size - 1] = static_cast<unsigned char>(pattern(ptr, size - 1));
  }
}

bool verify(const void *ptr, std::size_t size, const void *pattern_ptr) {
  auto *bytes = static_cast<const unsigned char *>(ptr);
  for (std::size_t i = 0; i < size; i += 61) {
    if (bytes[i] != static_cast<unsigned char>(pattern(pattern_ptr, i))) {
      return false;
    }
  }
  return true;
}

// 1. all allocation functions under concurrent use with very frequent sampling
void test_stress() {
  std::printf("== stress: all allocation functions, 8 threads, sampling interval 4 KB\n");
  HeapSampler::set_sampling_interval(4096);
  std::atomic<int> errors{0};
  static std::atomic<int> error_kinds[6];
  std::vector<std::thread> threads;
  threads.reserve(8);
  // let glibc create its arenas for the threads before measuring
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([] { free(escape(malloc(100))); });
  }
  for (auto &t : threads) {
    t.join();
  }
  threads.clear();
  auto before = HeapSampler::get_stats();
  auto glibc_before = glibc_in_use();
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([t, &errors] {
      std::mt19937_64 rng(t + 1);
      struct Block {
        void *ptr = nullptr;
        std::size_t size = 0;
        void *pattern_ptr = nullptr;
      };
      std::vector<Block> blocks(2000);
      auto random_size = [&] {
        auto r = rng() % 1000;
        if (r < 600) {
          return static_cast<std::size_t>(rng() % 256);
        }
        if (r < 900) {
          return static_cast<std::size_t>(rng() % 4096);
        }
        if (r < 995) {
          return static_cast<std::size_t>(rng() % 300000);
        }
        return static_cast<std::size_t>(rng() % 4000000);
      };
      for (int op = 0; op < 300000; op++) {
        set_context(0x1000 * (1 + rng() % 8));
        auto &b = blocks[rng() % blocks.size()];
        if (b.ptr != nullptr) {
          if (!verify(b.ptr, b.size, b.pattern_ptr)) {
            errors++;
            error_kinds[0]++;
          }
          if (malloc_usable_size(b.ptr) < b.size) {
            errors++;
            error_kinds[1]++;
          }
          if (rng() % 3 == 0) {
            // realloc must keep the prefix
            auto new_size = random_size();
            void *p = rng() % 2 ? realloc(b.ptr, new_size) : reallocarray(b.ptr, 1, new_size);
            if (new_size == 0) {
              b.ptr = nullptr;
              continue;
            }
            if (p == nullptr || !verify(p, std::min(b.size, new_size), b.pattern_ptr)) {
              errors++;
              error_kinds[2]++;
            }
            b.ptr = p;
            b.size = new_size;
            fill(b.ptr, b.size);
            b.pattern_ptr = b.ptr;
            continue;
          }
          free(b.ptr);
          b.ptr = nullptr;
          continue;
        }
        auto size = random_size();
        void *p = nullptr;
        // malloc must return memory aligned enough for any object of the size; jemalloc aligns 8-byte blocks to 8
        std::size_t alignment = size <= 8 ? 8 : 16;
        switch (rng() % 8) {
          case 0:
          case 1:
          case 2:
            p = malloc(size);
            break;
          case 3: {
            p = calloc(1 + size % 7, size / 7 + 1);
            size = (1 + size % 7) * (size / 7 + 1);
            alignment = size <= 8 ? 8 : 16;
            for (std::size_t i = 0; i < size; i++) {
              if (static_cast<unsigned char *>(p)[i] != 0) {
                errors++;
                error_kinds[3]++;
                break;
              }
            }
            break;
          }
          case 4:
            alignment = static_cast<std::size_t>(1) << (3 + rng() % 11);  // up to 8 KB
            p = memalign(alignment, size);
            break;
          case 5:
            alignment = static_cast<std::size_t>(1) << (3 + rng() % 11);
            if (posix_memalign(&p, alignment, size) != 0) {
              p = nullptr;
            }
            break;
          case 6:
            alignment = static_cast<std::size_t>(1) << (rng() % 14);
            p = aligned_alloc(alignment, size);
            break;
          case 7:
            alignment = 4096;
            p = rng() % 2 ? valloc(size) : pvalloc(size);
            break;
        }
        if (p == nullptr || reinterpret_cast<std::uintptr_t>(p) % alignment != 0) {
          errors++;
          if (error_kinds[p == nullptr ? 4 : 5]++ < 3) {
            std::printf("bad allocation %p of size %zu with alignment %zu\n", p, size, alignment);
          }
          continue;
        }
        b.ptr = p;
        b.size = size;
        fill(p, size);
        b.pattern_ptr = p;
      }
      for (auto &b : blocks) {
        if (b.ptr != nullptr) {
          if (!verify(b.ptr, b.size, b.pattern_ptr)) {
            errors++;
          }
          free(b.ptr);
        }
      }
      free(nullptr);
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  threads.clear();
  auto after = HeapSampler::get_stats();
  std::printf(
      "errors: %d (content %d, usable size %d, realloc %d, calloc %d, null %d, alignment %d), samples before %lld, "
      "after %lld\n",
      errors.load(), error_kinds[0].load(), error_kinds[1].load(), error_kinds[2].load(), error_kinds[3].load(),
      error_kinds[4].load(), error_kinds[5].load(), static_cast<long long>(before.sample_count),
      static_cast<long long>(after.sample_count));
  CHECK_TEST(errors.load() == 0, "%d errors", errors.load());
  // glibc can reallocate structures of cached thread stacks, which were allocated before the test
  CHECK_TEST(std::llabs(after.sample_count - before.sample_count) <= 16, "sample count drift");
  // memory of finished threads can be kept by glibc, but it must be counted in the same way
  check_exact_counter("stress", before, glibc_before, 0);
  HeapSampler::set_sampling_interval(0);
}

struct LiveAllocation {
  void *ptr;
  std::size_t size;
};

struct OwnerState {
  td::int32 owner = 0;
  std::uint64_t context = 0;
  double target = 0;
  std::vector<LiveAllocation> live;
  double exact = 0;
  double variance = 0;
  std::mutex mutex;
};

// 2. unbiasedness and variance of estimates for different size distributions
void run_accuracy(const char *name, double interval, const std::vector<double> &targets,
                  const std::function<std::size_t(std::mt19937_64 &)> &random_size, bool use_realloc) {
  HeapSampler::set_sampling_interval(static_cast<td::int64>(interval));
  std::vector<std::unique_ptr<OwnerState>> owners;
  for (std::size_t i = 0; i < targets.size(); i++) {
    auto state = std::make_unique<OwnerState>();
    state->owner = HeapSampler::create_owner();
    state->context = 0x100000 + 0x1000 * i;
    state->target = targets[i];
    HeapSampler::set_context_owner(state->context, state->owner, static_cast<td::int32>(i % 2));
    owners.push_back(std::move(state));
  }
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(1000 + t);
      while (!stop.load(std::memory_order_relaxed)) {
        auto &state = *owners[rng() % owners.size()];
        std::lock_guard<std::mutex> guard(state.mutex);
        // bookkeeping of the test allocates memory too, so only the measured operations are made in owner contexts
        set_context(0);
        for (int k = 0; k < 32; k++) {
          bool grow = state.exact < state.target;
          if (grow || (rng() % 2 == 0 && !state.live.empty())) {
            if (!grow) {
              continue;
            }
            if (use_realloc && !state.live.empty() && rng() % 2 == 0) {
              // grow an existing block like std::vector does
              auto &a = state.live[rng() % state.live.size()];
              auto new_size = a.size + a.size / 2 + 16;
              set_context(state.context);
              void *p = realloc(a.ptr, new_size);
              set_context(0);
              state.exact += chunk_size(new_size) - chunk_size(a.size);
              state.variance += sample_variance(new_size, interval) - sample_variance(a.size, interval);
              a.ptr = p;
              a.size = new_size;
              continue;
            }
            auto size = random_size(rng);
            set_context(state.context);
            void *p = malloc(size);
            set_context(0);
            static_cast<char *>(p)[0] = 1;
            state.live.push_back({p, size});
            state.exact += chunk_size(size);
            state.variance += sample_variance(size, interval);
          } else if (!state.live.empty()) {
            auto i = rng() % state.live.size();
            // free in the context of another owner and possibly on another thread than the allocation
            set_context(owners[rng() % owners.size()]->context);
            free(state.live[i].ptr);
            set_context(0);
            state.exact -= chunk_size(state.live[i].size);
            state.variance -= sample_variance(state.live[i].size, interval);
            state.live[i] = state.live.back();
            state.live.pop_back();
          }
        }
      }
    });
  }

  std::vector<double> z_scores;
  double max_relative_error[16] = {};
  for (int snapshot = 0; snapshot < 30; snapshot++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(snapshot == 0 ? 3000 : 300));
    for (std::size_t i = 0; i < owners.size(); i++) {
      auto &state = *owners[i];
      std::lock_guard<std::mutex> guard(state.mutex);
      auto stats = HeapSampler::get_owner_stats(state.owner);
      double estimate = static_cast<double>(stats.bytes[0] + stats.bytes[1]);
      double sigma = std::sqrt(std::max(state.variance, 1.0));
      z_scores.push_back((estimate - state.exact) / sigma);
      max_relative_error[i] = std::max(max_relative_error[i], std::fabs(estimate - state.exact) / state.exact);
    }
  }
  stop = true;
  for (auto &t : threads) {
    t.join();
  }

  double mean = 0;
  for (auto z : z_scores) {
    mean += z;
  }
  mean /= static_cast<double>(z_scores.size());
  double variance = 0;
  double max_abs = 0;
  for (auto z : z_scores) {
    variance += (z - mean) * (z - mean);
    max_abs = std::max(max_abs, std::fabs(z));
  }
  double stddev = std::sqrt(variance / static_cast<double>(z_scores.size() - 1));
  std::printf("%-34s interval %4.0f KB:", name, interval / 1024);
  for (std::size_t i = 0; i < owners.size(); i++) {
    std::printf(" %s %.1f%%",
                owners[i]->target >= 1e9 ? (std::to_string(static_cast<int>(owners[i]->target / 1e9)) + "GB").c_str()
                                         : (std::to_string(static_cast<int>(owners[i]->target / 1e6)) + "MB").c_str(),
                max_relative_error[i] * 100);
  }
  std::printf(" | z mean %+.2f stddev %.2f max %.1f\n", mean, stddev, max_abs);
  // snapshots of the same heap are correlated, so the bounds are loose; the bias is checked precisely by test_bias
  CHECK_TEST(std::fabs(mean) < 1.5, "%s: biased estimate, mean z %.2f", name, mean);
  CHECK_TEST(stddev > 0.3 && stddev < 2.0, "%s: wrong variance, z stddev %.2f", name, stddev);
  CHECK_TEST(max_abs < 6.0, "%s: too big error, max z %.1f", name, max_abs);

  for (auto &state : owners) {
    set_context(state->context);
    for (auto &a : state->live) {
      free(a.ptr);
    }
    set_context(0);
    auto stats = HeapSampler::get_owner_stats(state->owner);
    CHECK_TEST(stats.bytes[0] == 0 && stats.bytes[1] == 0 && stats.sample_count == 0,
               "%s: estimate isn't zero after all allocations are freed", name);
    HeapSampler::remove_context(state->context, state->owner);
    HeapSampler::destroy_owner(state->owner);
  }
  set_context(0);
  HeapSampler::set_sampling_interval(0);
}

void test_accuracy() {
  std::printf("== accuracy: max relative error per owner and z = (estimate - exact) / theoretical sigma\n");
  auto mixed = [](std::mt19937_64 &rng) {
    static const std::size_t sizes[] = {24, 48, 80, 136, 264, 1000, 4000};
    static const double weights[] = {30, 20, 15, 10, 10, 8, 5};
    std::discrete_distribution<int> dist(std::begin(weights), std::end(weights));
    double r = std::uniform_real_distribution<double>(0, 1)(rng);
    if (r < 0.001) {
      return static_cast<std::size_t>(65536 + rng() % 65536);
    }
    if (r < 0.0011) {
      return static_cast<std::size_t>((1 << 20) + rng() % (1 << 20));
    }
    auto base = sizes[dist(rng)];
    return base + static_cast<std::size_t>(rng() % (base / 4 + 1));
  };
  auto tiny = [](std::mt19937_64 &rng) {
    return static_cast<std::size_t>(rng() % 64);
  };
  auto large = [](std::mt19937_64 &rng) {
    return static_cast<std::size_t>(32768 + rng() % (4 << 20));
  };
  auto pareto = [](std::mt19937_64 &rng) {
    double u = std::uniform_real_distribution<double>(1e-9, 1)(rng);
    return static_cast<std::size_t>(std::min(32.0 / std::pow(u, 1 / 1.1), 64e6));
  };
  std::vector<double> targets = {2e9, 5e8, 1e8, 2e7};
  run_accuracy("mixed sizes", 65536, targets, mixed, false);
  run_accuracy("mixed sizes", 262144, targets, mixed, false);
  run_accuracy("tiny sizes 0-63", 65536, {5e8, 1e8, 2e7}, tiny, false);
  run_accuracy("large sizes 32 KB - 4 MB", 65536, {2e9, 5e8, 1e8}, large, false);
  run_accuracy("heavy-tailed sizes (Pareto 1.1)", 65536, targets, pareto, false);
  run_accuracy("mixed sizes with realloc growth", 65536, targets, mixed, true);
}

// 2b. bias over independent runs: z-scores of independent heaps must have mean 0 and standard deviation 1
void run_bias(const char *name, double interval, double heap_size,
              const std::function<std::size_t(std::mt19937_64 &)> &random_size) {
  HeapSampler::set_sampling_interval(static_cast<td::int64>(interval));
  const int RUNS = 200;
  auto owner = HeapSampler::create_owner();
  HeapSampler::set_context_owner(0xF000, owner, 0);
  std::mt19937_64 rng(4242);
  std::vector<void *> blocks;
  blocks.reserve(static_cast<std::size_t>(heap_size / 8));
  double sum_z = 0, sum_z2 = 0, sum_relative = 0;
  for (int run = 0; run < RUNS; run++) {
    double exact = 0, variance = 0;
    while (exact < heap_size) {
      auto size = random_size(rng);
      set_context(0xF000);
      void *p = malloc(size);
      set_context(0);
      blocks.push_back(p);
      exact += chunk_size(size);
      variance += sample_variance(size, interval);
    }
    auto stats = HeapSampler::get_owner_stats(owner);
    double z = (static_cast<double>(stats.bytes[0]) - exact) / std::sqrt(variance);
    sum_z += z;
    sum_z2 += z * z;
    sum_relative += (static_cast<double>(stats.bytes[0]) - exact) / exact;
    set_context(0xF000);
    for (auto p : blocks) {
      free(p);
    }
    set_context(0);
    blocks.clear();
  }
  double mean = sum_z / RUNS;
  double stddev = std::sqrt(sum_z2 / RUNS - mean * mean);
  std::printf(
      "%-34s interval %4.0f KB, %d heaps of %.0f MB: mean error %+.3f%%, z mean %+.3f (must be within +-%.2f), "
      "z stddev %.2f\n",
      name, interval / 1024, RUNS, heap_size / 1e6, sum_relative / RUNS * 100, mean, 3 / std::sqrt(RUNS * 1.0), stddev);
  CHECK_TEST(std::fabs(mean) < 3 / std::sqrt(RUNS * 1.0), "%s: biased estimate, z mean %.3f", name, mean);
  CHECK_TEST(stddev > 0.8 && stddev < 1.2, "%s: wrong variance, z stddev %.2f", name, stddev);
  HeapSampler::remove_context(0xF000, owner);
  HeapSampler::destroy_owner(owner);
  HeapSampler::set_sampling_interval(0);
}

void test_bias() {
  std::printf("== bias over independent heaps\n");
  auto tiny = [](std::mt19937_64 &rng) {
    return static_cast<std::size_t>(rng() % 64);
  };
  auto mixed = [](std::mt19937_64 &rng) {
    static const std::size_t sizes[] = {24, 48, 80, 136, 264, 1000, 4000};
    auto base = sizes[rng() % 7];
    return base + static_cast<std::size_t>(rng() % (base / 4 + 1));
  };
  auto pareto = [](std::mt19937_64 &rng) {
    double u = std::uniform_real_distribution<double>(1e-9, 1)(rng);
    return static_cast<std::size_t>(std::min(32.0 / std::pow(u, 1 / 1.1), 64e6));
  };
  auto around_interval = [](std::mt19937_64 &rng) {
    return static_cast<std::size_t>(rng() % 200000);
  };
  run_bias("tiny sizes 0-63", 65536, 2e7, tiny);
  run_bias("mixed sizes", 65536, 5e7, mixed);
  run_bias("mixed sizes", 262144, 5e7, mixed);
  run_bias("heavy-tailed sizes (Pareto 1.1)", 65536, 5e7, pareto);
  run_bias("sizes 0-200 KB around the interval", 65536, 2e8, around_interval);
}

// 3. attribution: owners, contexts, cross-thread frees, destroyed owners
void test_attribution() {
  std::printf("== attribution\n");
  HeapSampler::set_sampling_interval(1);  // sample every allocation to make the test deterministic
  auto a = HeapSampler::create_owner();
  auto b = HeapSampler::create_owner();
  CHECK_TEST(a > 0 && b > 0 && a != b, "owners %d %d", a, b);
  const std::uint64_t context_a = 0xA000, context_b = 0xB000, context_unknown = 0xC000;
  HeapSampler::set_context_owner(context_a, a, 0);
  HeapSampler::set_context_owner(context_b, b, 1);
  auto unattributed_before = HeapSampler::get_owner_stats(0);

  set_context(context_a);
  void *pa = escape(malloc(1000));
  set_context(context_b);
  void *pb = escape(malloc(2000));
  set_context(context_unknown);
  void *pu = escape(malloc(3000));
  auto sa = HeapSampler::get_owner_stats(a);
  auto sb = HeapSampler::get_owner_stats(b);
  auto su = HeapSampler::get_owner_stats(0);
  std::printf("owner a: %lld/%lld, owner b: %lld/%lld, unattributed +%lld\n", static_cast<long long>(sa.bytes[0]),
              static_cast<long long>(sa.bytes[1]), static_cast<long long>(sb.bytes[0]),
              static_cast<long long>(sb.bytes[1]), static_cast<long long>(su.bytes[0] - unattributed_before.bytes[0]));
  CHECK_TEST(sa.bytes[0] > 0 && sa.bytes[1] == 0 && sa.sample_count == 1, "owner a");
  CHECK_TEST(sb.bytes[0] == 0 && sb.bytes[1] > 0 && sb.sample_count == 1, "owner b, part 1");
  CHECK_TEST(su.sample_count == unattributed_before.sample_count + 1, "unknown context is unattributed");

  // PartGuard attributes allocations to another part of the owner of the current context
  set_context(context_a);
  void *pg = nullptr;
  {
    HeapSampler::PartGuard guard(2);
    pg = escape(malloc(700));
  }
  void *pa2 = escape(malloc(800));
  sa = HeapSampler::get_owner_stats(a);
  CHECK_TEST(sa.bytes[2] > 0 && sa.sample_count == 3, "part guard");
  free(pg);
  free(pa2);
  sa = HeapSampler::get_owner_stats(a);
  CHECK_TEST(sa.bytes[2] == 0 && sa.sample_count == 1, "part guard memory is freed");

  // a block is freed on another thread in another context: it is removed from the owner, which allocated it
  std::thread([&] {
    set_context(context_b);
    free(pa);
  }).join();
  sa = HeapSampler::get_owner_stats(a);
  CHECK_TEST(sa.bytes[0] == 0 && sa.sample_count == 0, "cross-thread free");

  // realloc moves a block to the owner of the current context
  set_context(context_a);
  pb = escape(realloc(pb, 2100));
  sa = HeapSampler::get_owner_stats(a);
  sb = HeapSampler::get_owner_stats(b);
  CHECK_TEST(sa.sample_count == 1 && sb.sample_count == 0, "realloc in another context");

  // the last owner of a context wins; removal by a previous owner doesn't remove the context
  HeapSampler::set_context_owner(context_a, b, 0);
  HeapSampler::remove_context(context_a, a);
  void *pc = escape(malloc(500));
  sb = HeapSampler::get_owner_stats(b);
  CHECK_TEST(sb.sample_count == 1, "context reuse");

  // memory of a destroyed owner is moved to the unattributed memory and removed from it when freed
  auto unattributed_mid = HeapSampler::get_owner_stats(0);
  auto sa_before_destroy = HeapSampler::get_owner_stats(a);
  HeapSampler::destroy_owner(a);
  auto unattributed_after_destroy = HeapSampler::get_owner_stats(0);
  CHECK_TEST(unattributed_after_destroy.bytes[0] == unattributed_mid.bytes[0] + sa_before_destroy.bytes[0],
             "destroyed owner memory moved");
  auto reused = HeapSampler::create_owner();
  CHECK_TEST(reused == a, "owner slot is reused");
  free(pb);  // was allocated by the destroyed owner
  auto sr = HeapSampler::get_owner_stats(reused);
  CHECK_TEST(sr.bytes[0] == 0 && sr.sample_count == 0, "new owner in the reused slot isn't affected");
  CHECK_TEST(HeapSampler::get_owner_stats(0).bytes[0] == unattributed_mid.bytes[0], "unattributed memory restored");

  free(pc);
  free(pu);
  HeapSampler::remove_context(context_a, b);
  HeapSampler::remove_context(context_b, b);
  HeapSampler::destroy_owner(reused);
  HeapSampler::destroy_owner(b);
  auto unattributed_end = HeapSampler::get_owner_stats(0);
  CHECK_TEST(unattributed_end.bytes[0] == unattributed_before.bytes[0] &&
                 unattributed_end.bytes[1] == unattributed_before.bytes[1] &&
                 unattributed_end.sample_count == unattributed_before.sample_count,
             "everything is returned");
  set_context(0);
  HeapSampler::set_sampling_interval(0);
}

// 4. long churn: the estimate returns exactly to zero and the exact counter has no drift
void test_drift() {
  std::printf("== drift: 20 million operations\n");
  HeapSampler::set_sampling_interval(65536);
  auto owner = HeapSampler::create_owner();
  HeapSampler::set_context_owner(0xD000, owner, 0);
  std::vector<std::thread> threads;
  threads.reserve(4);
  auto before = HeapSampler::get_stats();
  auto glibc_before = glibc_in_use();
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([t] {
      set_context(0xD000);
      std::mt19937_64 rng(77 + t);
      std::vector<void *> live(10000, nullptr);
      for (int op = 0; op < 5000000; op++) {
        auto &p = live[rng() % live.size()];
        if (p != nullptr) {
          free(p);
          p = nullptr;
        } else {
          p = malloc(rng() % 2 ? rng() % 512 : rng() % 70000);
        }
      }
      for (auto p : live) {
        free(p);
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  threads.clear();
  auto stats = HeapSampler::get_owner_stats(owner);
  auto after = HeapSampler::get_stats();
  std::printf("owner estimate %lld, samples %lld, exact counter change %lld\n", static_cast<long long>(stats.bytes[0]),
              static_cast<long long>(stats.sample_count), static_cast<long long>(after.live_bytes - before.live_bytes));
  CHECK_TEST(stats.bytes[0] == 0 && stats.sample_count == 0, "estimate returns to zero");
  check_exact_counter("drift", before, glibc_before, 0);
  HeapSampler::remove_context(0xD000, owner);
  HeapSampler::destroy_owner(owner);
  set_context(0);
  HeapSampler::set_sampling_interval(0);
}

// 5. the exact counter matches glibc accounting while many blocks are alive
void test_exact() {
  std::printf("== exact counter with 1 million live blocks\n");
  HeapSampler::set_sampling_interval(65536);
  std::vector<void *> blocks;
  blocks.reserve(1000000);
  auto before = HeapSampler::get_stats();
  auto glibc_before = glibc_in_use();
  std::mt19937_64 rng(5);
  for (int i = 0; i < 1000000; i++) {
    std::size_t size = rng() % 1000 == 0 ? 100000 + rng() % 1000000 : rng() % 600;
    blocks.push_back(rng() % 4 == 0 ? calloc(1, size) : malloc(size));
  }
  for (int i = 0; i < 200000; i++) {
    auto &p = blocks[rng() % blocks.size()];
    p = realloc(p, rng() % 2000);
  }
  check_exact_counter("1 million live blocks", before, glibc_before, 0);
  for (auto p : blocks) {
    free(p);
  }
  check_exact_counter("all blocks are freed", before, glibc_before, 0);
  HeapSampler::set_sampling_interval(0);
}

// 6. cost of an allocation and a deallocation
void test_overhead() {
  std::printf("== overhead: malloc + free pair, live set of 4096 blocks per thread\n");
  for (td::int64 interval :
       {static_cast<td::int64>(0), static_cast<td::int64>(262144), static_cast<td::int64>(65536)}) {
    HeapSampler::set_sampling_interval(interval);
    for (int thread_count : {1, 4}) {
      const long OPS = 10000000;
      auto start = std::chrono::steady_clock::now();
      std::vector<std::thread> threads;
      for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([t] {
          set_context(0xE000);
          static const std::size_t sizes[] = {24, 48, 80, 136, 264, 40, 32, 1000};
          std::vector<void *> live(4096, nullptr);
          unsigned x = 12345 + t;
          for (long i = 0; i < OPS; i++) {
            x = x * 1103515245 + 12345;
            auto &p = live[(x >> 8) & 4095];
            free(p);
            p = malloc(sizes[(x >> 20) & 7]);
            *static_cast<char *>(p) = 1;
          }
          for (auto p : live) {
            free(p);
          }
        });
      }
      for (auto &t : threads) {
        t.join();
      }
      double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      std::printf("interval %6lld, %d thread(s): %.1f ns per pair\n", static_cast<long long>(interval), thread_count,
                  seconds * 1e9 / OPS);
    }
  }
  HeapSampler::set_sampling_interval(0);
}

}  // namespace

int main(int argc, char *argv[]) {
  real_nallocx = reinterpret_cast<std::size_t (*)(std::size_t, int)>(dlsym(RTLD_DEFAULT, "nallocx"));
  real_mallctl = reinterpret_cast<decltype(real_mallctl)>(dlsym(RTLD_DEFAULT, "mallctl"));
  if (real_mallctl != nullptr && std::getenv("HEAP_SAMPLER_TEST_REEXEC") == nullptr) {
    // jemalloc counts blocks in its thread caches as allocated, so the caches are disabled to compare exactly
    setenv("MALLOC_CONF", "tcache:false", 1);
    setenv("HEAP_SAMPLER_TEST_REEXEC", "1", 1);
    execv("/proc/self/exe", argv);
  }
  if (real_nallocx == nullptr && std::getenv("HEAP_SAMPLER_TEST_REEXEC") == nullptr) {
    // glibc counts blocks in its thread caches and its arena headers as used, so they are disabled to compare exactly
    setenv("GLIBC_TUNABLES", "glibc.malloc.tcache_count=0:glibc.malloc.arena_max=1", 1);
    setenv("HEAP_SAMPLER_TEST_REEXEC", "1", 1);
    execv("/proc/self/exe", argv);
  }
  std::printf("allocator: %s, heap sampler: %s\n", real_nallocx != nullptr ? "with nallocx (jemalloc)" : "glibc",
              HeapSampler::is_supported() ? "enabled" : "disabled");
  if (!HeapSampler::is_supported()) {
    return 1;
  }
  std::vector<std::string> tests;
  for (int i = 1; i < argc; i++) {
    tests.push_back(argv[i]);
  }
  if (tests.empty()) {
    tests = {"stress", "exact", "attribution", "drift", "bias", "accuracy", "overhead"};
  }
  for (auto &test : tests) {
    if (test == "stress") {
      test_stress();
    } else if (test == "accuracy") {
      test_accuracy();
    } else if (test == "bias") {
      test_bias();
    } else if (test == "attribution") {
      test_attribution();
    } else if (test == "drift") {
      test_drift();
    } else if (test == "exact") {
      test_exact();
    } else if (test == "overhead") {
      test_overhead();
    }
  }
  std::printf(failures == 0 ? "ALL CHECKS PASSED\n" : "%d CHECKS FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
