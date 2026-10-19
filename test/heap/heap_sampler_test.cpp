//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Verification of HeapSampler: correctness of all allocation functions under concurrent use, unbiasedness and
// variance of estimates for different size distributions, attribution to owners, absence of drift, overhead.
//
// Usage: heap-sampler-test [stress] [exact] [attribution] [edge] [api] [limits] [drift] [bias] [accuracy] [overhead]
//
#include "telegram-bot-api/HeapSampler.h"

#include "td/actor/actor.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
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
#include <new>
#include <random>
#include <string>
#include <sys/wait.h>
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
  if (chunk >= (static_cast<std::size_t>(128) << 10)) {
    // glibc allocates chunks of at least 128 KiB with mmap
    return static_cast<double>((chunk + 8 + 4095) & ~static_cast<std::size_t>(4095));
  }
  return static_cast<double>(chunk);
}

double sample_variance(std::size_t size, double interval) {
  double p = -std::expm1(-static_cast<double>(size + 16) / interval);
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
  std::printf("== stress: all allocation functions, 8 threads, sampling interval 4 KiB\n");
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
            alignment = static_cast<std::size_t>(1) << (3 + rng() % 11);  // up to 8 KiB
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
  std::printf("%-34s interval %4.0f KiB:", name, interval / 1024);
  for (std::size_t i = 0; i < owners.size(); i++) {
    std::printf(" %s %.1f%%",
                owners[i]->target >= 1e9 ? (std::to_string(static_cast<int>(owners[i]->target / 1e9)) + "GB").c_str()
                                         : (std::to_string(static_cast<int>(owners[i]->target / 1e6)) + "MB").c_str(),
                max_relative_error[i] * 100);
  }
  std::printf(" | z mean %+.2f stddev %.2f max %.1f\n", mean, stddev, max_abs);
  // Each z has the variance 1 and owners are sampled independently, so the mean of z over all snapshots has a standard
  // deviation of at most 1 / sqrt(owners) however correlated snapshots of the same owner are. Only the mean is checked:
  // z of a heap of a few big blocks has heavy tails, because missing a block with a high probability of sampling
  // gives z below -10, so the distribution of z is checked by test_bias on independent heaps.
  double max_mean = 4 / std::sqrt(static_cast<double>(owners.size()));
  CHECK_TEST(std::fabs(mean) < max_mean, "%s: biased estimate, mean z %.2f", name, mean);

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
  run_accuracy("large sizes 32 KiB - 4 MiB", 65536, {2e9, 5e8, 1e8}, large, false);
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
  // coverage of the exact value by the intervals estimate +- k * sigma, where sigma is reported by HeapSampler itself
  int covered[4] = {};
  double sum_reported_variance = 0, sum_variance = 0;
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
    double reported_error = std::fabs(static_cast<double>(stats.bytes[0]) - exact);
    for (int k = 1; k <= 3; k++) {
      if (reported_error <= k * std::sqrt(std::max(stats.variance, 0.0))) {
        covered[k]++;
      }
    }
    sum_reported_variance += stats.variance;
    sum_variance += variance;
    if (HeapSampler::is_audit_enabled()) {
      // the exact size, which is used to verify the estimates on a real server, must be exact
      CHECK_TEST(static_cast<double>(stats.exact_bytes[0]) == exact, "%s: audit size %lld instead of %.0f", name,
                 static_cast<long long>(stats.exact_bytes[0]), exact);
    }
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
      "%-34s interval %4.0f KiB, %d heaps of %.0f MB: mean error %+.3f%%, z mean %+.3f (must be within +-%.2f), "
      "z stddev %.2f\n",
      name, interval / 1024, RUNS, heap_size / 1e6, sum_relative / RUNS * 100, mean, 3 / std::sqrt(RUNS * 1.0), stddev);
  CHECK_TEST(std::fabs(mean) < 3 / std::sqrt(RUNS * 1.0), "%s: biased estimate, z mean %.3f", name, mean);
  CHECK_TEST(stddev > 0.8 && stddev < 1.2, "%s: wrong variance, z stddev %.2f", name, stddev);
  double variance_ratio = sum_reported_variance / sum_variance;
  std::printf(
      "%-34s reported sigma: variance / theoretical %.3f, exact value within 1/2/3 sigma: %.1f%%/%.1f%%/%.1f%%\n", "",
      variance_ratio, covered[1] * 100.0 / RUNS, covered[2] * 100.0 / RUNS, covered[3] * 100.0 / RUNS);
  // the reported variance is an unbiased estimate, so its mean over the runs must be close to the theoretical variance
  CHECK_TEST(std::fabs(variance_ratio - 1) < 0.1, "%s: biased variance estimate, ratio %.3f", name, variance_ratio);
  // normal distribution gives 68.3%, 95.4% and 99.7%; the bounds allow for the binomial noise of 200 runs
  CHECK_TEST(covered[1] >= RUNS * 0.58, "%s: exact value is within 1 sigma in %d runs", name, covered[1]);
  CHECK_TEST(covered[2] >= RUNS * 0.90, "%s: exact value is within 2 sigma in %d runs", name, covered[2]);
  CHECK_TEST(covered[3] >= RUNS * 0.98, "%s: exact value is within 3 sigma in %d runs", name, covered[3]);
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
    return static_cast<std::size_t>(rng() % (200 << 10));
  };
  run_bias("tiny sizes 0-63", 65536, 2e7, tiny);
  run_bias("mixed sizes", 65536, 5e7, mixed);
  run_bias("mixed sizes", 262144, 5e7, mixed);
  run_bias("heavy-tailed sizes (Pareto 1.1)", 65536, 5e7, pareto);
  run_bias("sizes 0-200 KiB around interval", 65536, 2e8, around_interval);
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

// 4b. edge cases found by review
void test_edge() {
  std::printf("== edge cases\n");
  HeapSampler::set_sampling_interval(1);  // sample every allocation

  // sampled empty blocks fill whole slab pages; pointers to them must never be taken for page runs
  std::vector<void *> empty_blocks;
  std::vector<std::pair<void *, std::size_t>> big_blocks;
  empty_blocks.reserve(1000);
  big_blocks.reserve(100);
  auto before = HeapSampler::get_stats();
  for (int i = 0; i < 1000; i++) {
    empty_blocks.push_back(escape(i % 2 ? malloc(0) : calloc(0, 10)));
    if (i % 10 == 0) {
      std::size_t size = 5000 + i;
      void *p = malloc(size);
      fill(p, size);
      big_blocks.emplace_back(p, size);
    }
  }
  int page_aligned = 0;
  for (auto p : empty_blocks) {
    // usable size of sampled empty blocks is 0; the real allocator can return page-aligned pointers, and in the audit
    // mode usable size of a block of the real allocator can be 0 too
    page_aligned += (reinterpret_cast<std::uintptr_t>(p) & 4095) == 0 && malloc_usable_size(p) == 0 &&
                    !HeapSampler::is_audit_enabled();
    // 0 for sampled blocks, at least 24 for glibc, which can return a slightly bigger free chunk; a wrong lookup
    // would return the size of another block
    CHECK_TEST(malloc_usable_size(p) <= 64, "usable size of an empty block");
  }
  CHECK_TEST(page_aligned == 0, "%d empty blocks are page-aligned", page_aligned);
  for (auto p : empty_blocks) {
    free(p);
  }
  int corrupted = 0;
  for (auto &b : big_blocks) {
    corrupted += !verify(b.first, b.second, b.first);
    free(b.first);
  }
  CHECK_TEST(corrupted == 0, "%d blocks are corrupted", corrupted);
  CHECK_TEST(HeapSampler::get_stats().sample_count == before.sample_count, "samples are freed");

  // huge allocations fail like with glibc; jemalloc relies on overcommit and succeeds
  void *huge = escape(malloc(static_cast<std::size_t>(1) << 46));
  CHECK_TEST(huge == nullptr || real_mallctl != nullptr, "64 TB allocation succeeded");
  free(huge);
  errno = 0;
  CHECK_TEST(pvalloc(~static_cast<std::size_t>(0) - 100) == nullptr && errno == ENOMEM, "pvalloc overflow");

  // big sampled blocks are committed lazily, zeroed and resized in place
  std::size_t big_size = static_cast<std::size_t>(40) << 20;
  auto *big = static_cast<unsigned char *>(escape(calloc(1, big_size)));
  CHECK_TEST(big != nullptr && big[0] == 0 && big[big_size - 1] == 0 && big[big_size / 2] == 0, "big calloc");
  big[0] = 1;
  big[big_size - 1] = 2;
  auto *bigger = static_cast<unsigned char *>(escape(realloc(big, static_cast<std::size_t>(60) << 20)));
  CHECK_TEST(bigger == big, "realloc in place");
  CHECK_TEST(bigger[0] == 1 && bigger[big_size - 1] == 2, "realloc keeps data");
  auto *smaller = static_cast<unsigned char *>(escape(realloc(bigger, static_cast<std::size_t>(30) << 20)));
  CHECK_TEST(smaller == big && smaller[0] == 1, "shrink in place");
  free(smaller);
  CHECK_TEST(HeapSampler::get_stats().sample_count == before.sample_count, "big samples are freed");

  // disabled sampling doesn't sample new allocations
  HeapSampler::set_sampling_interval(0);
  auto disabled_before = HeapSampler::get_stats().sample_count;
  std::vector<void *> blocks;
  blocks.reserve(10000);
  for (int i = 0; i < 10000; i++) {
    blocks.push_back(escape(malloc(1000)));
  }
  CHECK_TEST(HeapSampler::get_stats().sample_count == disabled_before, "sampling is disabled");
  for (auto p : blocks) {
    free(p);
  }

  // generation of a reused owner slot doesn't wrap, so old samples aren't removed from a new owner
  HeapSampler::set_sampling_interval(1);
  auto owner = HeapSampler::create_owner();
  HeapSampler::set_context_owner(0x9000, owner, 0);
  set_context(0x9000);
  void *old_block = escape(malloc(100));
  set_context(0);
  HeapSampler::remove_context(0x9000, owner);
  HeapSampler::destroy_owner(owner);
  td::int32 new_owner = 0;
  for (int i = 0; i < 100000; i++) {
    new_owner = HeapSampler::create_owner();
    if (i + 1 < 100000) {
      HeapSampler::destroy_owner(new_owner);
    }
  }
  CHECK_TEST(new_owner == owner, "owner slot is reused");
  free(old_block);
  auto stats = HeapSampler::get_owner_stats(new_owner);
  CHECK_TEST(stats.bytes[0] == 0 && stats.sample_count == 0, "old sample affected a new owner after %d reuses", 100000);
  HeapSampler::destroy_owner(new_owner);

  // fork while another thread samples allocations
  std::atomic<bool> stop{false};
  std::thread allocator([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      free(escape(malloc(100)));
    }
  });
  int forks_ok = 0;
  for (int i = 0; i < 50; i++) {
    auto pid = fork();
    if (pid == 0) {
      for (int k = 0; k < 1000; k++) {
        free(escape(malloc(static_cast<std::size_t>(k))));
      }
      _exit(0);
    }
    int status = 0;
    if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      forks_ok++;
    }
  }
  stop = true;
  allocator.join();
  CHECK_TEST(forks_ok == 50, "%d of 50 forked children finished", forks_ok);
  HeapSampler::set_sampling_interval(0);
}

// 4c. results and errors of all allocation functions, C++ operators, realloc transitions and zeroing
void test_api() {
  std::printf("== API: errors, C++ operators, realloc transitions, zeroing\n");
  // volatile arguments prevent the compiler from evaluating the calls itself
  volatile std::size_t max_size = ~static_cast<std::size_t>(0);
  volatile std::size_t invalid_alignment = 3;
  const std::size_t MAX_SIZE = max_size;

  // errors
  errno = 0;
  CHECK_TEST(escape(calloc(MAX_SIZE / 2, 4)) == nullptr && errno == ENOMEM, "calloc overflow");
  void *block = escape(malloc(100));
  errno = 0;
  CHECK_TEST(escape(reallocarray(block, MAX_SIZE / 2, 4)) == nullptr && errno == ENOMEM, "reallocarray overflow");
  free(block);  // the block stays valid
  void *result = nullptr;
  errno = 0;
  CHECK_TEST(posix_memalign(&result, invalid_alignment, 10) == EINVAL && posix_memalign(&result, 4, 10) == EINVAL &&
                 errno == 0,
             "posix_memalign with an invalid alignment");
  CHECK_TEST(posix_memalign(&result, 64, 10) == 0 && reinterpret_cast<std::uintptr_t>(result) % 64 == 0 && errno == 0,
             "posix_memalign");
  free(result);
  errno = 0;
  CHECK_TEST(escape(aligned_alloc(invalid_alignment, 10)) == nullptr && errno == EINVAL,
             "aligned_alloc with an invalid alignment");
  CHECK_TEST(malloc_usable_size(nullptr) == 0, "malloc_usable_size(nullptr)");
  block = escape(realloc(nullptr, 100));
  CHECK_TEST(block != nullptr && malloc_usable_size(block) >= 100, "realloc(nullptr)");
  free(block);

  for (td::int64 interval : {static_cast<td::int64>(0), static_cast<td::int64>(1)}) {
    // with the interval 1, allocations of at least 64 bytes are sampled with the probability 1 - exp(-65)
    HeapSampler::set_sampling_interval(interval);
    auto sample_count = HeapSampler::get_stats().sample_count;

    // glibc rounds a non-power-of-2 alignment up
    volatile std::size_t alignment_24 = 24;
    block = escape(memalign(alignment_24, 100));
    CHECK_TEST(block != nullptr && reinterpret_cast<std::uintptr_t>(block) % 32 == 0, "memalign(24)");
    free(block);

    // C++ operators are accounted like malloc and free; jemalloc has own operators, which don't call malloc
    auto before = HeapSampler::get_stats().live_bytes;
    auto *array = static_cast<char *>(escape(new char[1 << 20]));
    array[0] = 1;
    CHECK_TEST(HeapSampler::get_stats().live_bytes - before >= (1 << 20), "operator new[] isn't accounted");
    delete[] array;
    struct alignas(256) Aligned {
      char data[300];
    };
    auto *aligned = static_cast<Aligned *>(escape(new Aligned()));
    CHECK_TEST(reinterpret_cast<std::uintptr_t>(aligned) % 256 == 0, "aligned operator new");
    delete aligned;
    void *page_aligned = escape(::operator new(100, std::align_val_t(8192)));
    CHECK_TEST(reinterpret_cast<std::uintptr_t>(page_aligned) % 8192 == 0, "operator new with big alignment");
    ::operator delete(page_aligned, std::align_val_t(8192));
    void *nothrow = escape(::operator new(16, std::nothrow));
    CHECK_TEST(nothrow != nullptr, "nothrow operator new");
    ::operator delete(nothrow);
    CHECK_TEST(HeapSampler::get_stats().live_bytes == before, "operators leak %lld bytes",
               static_cast<long long>(HeapSampler::get_stats().live_bytes - before));

    // free doesn't change errno, including when sampled blocks are returned to the OS
    for (std::size_t size :
         {static_cast<std::size_t>(100), static_cast<std::size_t>(2) << 20, static_cast<std::size_t>(40) << 20}) {
      block = escape(malloc(size));
      static_cast<char *>(block)[size - 1] = 1;
      errno = 12345;
      free(block);
      CHECK_TEST(errno == 12345, "free changed errno for a block of %zu bytes", size);
    }

    // calloc returns zeros for memory reused in every way: free slots, cached and released runs, committed blocks
    for (std::size_t size :
         {static_cast<std::size_t>(100), static_cast<std::size_t>(3000), static_cast<std::size_t>(64) << 10,
          static_cast<std::size_t>(1) << 20, static_cast<std::size_t>(2) << 20, static_cast<std::size_t>(40) << 20}) {
      for (int i = 0; i < 3; i++) {
        auto *data = static_cast<unsigned char *>(escape(malloc(size)));
        std::memset(data, 0xFF, size);
        free(data);
        data = static_cast<unsigned char *>(escape(calloc(1, size)));
        std::size_t non_zero = 0;
        for (std::size_t k = 0; k < size; k++) {
          non_zero += data[k] != 0;
        }
        CHECK_TEST(non_zero == 0, "calloc of %zu bytes returned %zu non-zero bytes", size, non_zero);
        free(data);
      }
    }
    CHECK_TEST(HeapSampler::get_stats().sample_count == sample_count, "samples leak");
  }

  // realloc moves a block between small slots, page runs and the real allocator, and resizes big runs in place;
  // a resized block belongs to the owner of the context, in which it was resized
  HeapSampler::set_sampling_interval(1);
  auto a = HeapSampler::create_owner();
  auto b = HeapSampler::create_owner();
  HeapSampler::set_context_owner(0xA100, a, 0);
  HeapSampler::set_context_owner(0xB100, b, 0);
  auto check_block = [](const unsigned char *data, std::size_t size, const char *step) {
    bool is_ok = true;
    for (std::size_t k = 0; k < size; k += 997) {
      is_ok &= data[k] == static_cast<unsigned char>(k % 251);
    }
    CHECK_TEST(is_ok, "contents changed by %s", step);
  };
  auto fill_block = [](unsigned char *data, std::size_t size) {
    for (std::size_t k = 0; k < size; k += 997) {
      data[k] = static_cast<unsigned char>(k % 251);
    }
  };
  auto samples = [](td::int32 owner) {
    return HeapSampler::get_owner_stats(owner).sample_count;
  };
  set_context(0xA100);
  auto *data = static_cast<unsigned char *>(escape(malloc(100)));
  fill_block(data, 100);
  CHECK_TEST(samples(a) == 1 && malloc_usable_size(data) == 100, "small sampled block");
  data = static_cast<unsigned char *>(escape(realloc(data, 3000)));
  check_block(data, 100, "small slot -> page run");
  fill_block(data, 3000);
  const std::size_t MiB = 1 << 20;
  data = static_cast<unsigned char *>(escape(realloc(data, MiB + MiB / 2)));
  check_block(data, 3000, "page run -> bigger page run");
  fill_block(data, MiB + MiB / 2);
  auto *same = static_cast<unsigned char *>(escape(realloc(data, 2 * MiB - 100)));
  CHECK_TEST(same == data, "growth in place");
  check_block(data, MiB + MiB / 2, "growth in place");
  CHECK_TEST(samples(a) == 1, "one sample after growth");
  set_context(0xB100);
  same = static_cast<unsigned char *>(escape(realloc(data, MiB + MiB / 4)));
  CHECK_TEST(same == data, "shrink in place");
  check_block(data, MiB + MiB / 4, "shrink in place");
  CHECK_TEST(samples(a) == 0 && samples(b) == 1, "a resized block belongs to the new context");
  data = static_cast<unsigned char *>(escape(realloc(data, 20 * MiB)));
  check_block(data, MiB + MiB / 4, "page run -> 20 MiB");
  fill_block(data, 20 * MiB);
  data = static_cast<unsigned char *>(escape(realloc(data, 33 * MiB)));
  check_block(data, 20 * MiB, "20 MiB -> committed 33 MiB");
  HeapSampler::set_sampling_interval(0);
  data = static_cast<unsigned char *>(escape(realloc(data, 100)));
  check_block(data, 100, "sampled -> not sampled");
  CHECK_TEST(samples(b) == 0, "a block, which isn't sampled anymore");
  HeapSampler::set_sampling_interval(1);
  data = static_cast<unsigned char *>(escape(realloc(data, 200)));
  check_block(data, 100, "not sampled -> sampled");
  CHECK_TEST(samples(b) == 1, "a block, which is sampled again");
  free(data);
  CHECK_TEST(samples(a) == 0 && samples(b) == 0, "all samples are freed");
  set_context(0);

  // freed runs keep at most 8 MiB of memory
  auto overhead_before = HeapSampler::get_stats().sampler_overhead_bytes;
  std::vector<void *> blocks;
  for (int i = 0; i < 20; i++) {
    blocks.push_back(escape(malloc(MiB)));
    std::memset(blocks.back(), 1, MiB);
  }
  for (auto p : blocks) {
    free(p);
  }
  auto overhead = HeapSampler::get_stats().sampler_overhead_bytes - overhead_before;
  CHECK_TEST(overhead <= static_cast<td::int64>(8 * MiB), "sampler keeps %lld more bytes of freed memory",
             static_cast<long long>(overhead));
  HeapSampler::remove_context(0xA100, a);
  HeapSampler::remove_context(0xB100, b);
  HeapSampler::destroy_owner(a);
  HeapSampler::destroy_owner(b);
  HeapSampler::set_sampling_interval(0);
}

// 4d. limits: owners, contexts and threads
void test_limits() {
  std::printf("== limits: owners, contexts, threads\n");
  HeapSampler::set_sampling_interval(1);
  // returns true if an allocation in the context is attributed to the owner
  auto is_owner = [](std::uint64_t context, td::int32 owner) {
    auto before = HeapSampler::get_owner_stats(owner).sample_count;
    set_context(context);
    void *block = escape(malloc(100));
    set_context(0);
    bool result = HeapSampler::get_owner_stats(owner).sample_count == before + 1;
    free(block);
    return result;
  };

  // there are at most 65535 owners, and destroyed owners are reused
  std::vector<td::int32> owners;
  while (true) {
    auto owner = HeapSampler::create_owner();
    if (owner == 0) {
      break;
    }
    owners.push_back(owner);
  }
  std::printf("created %zu owners\n", owners.size());
  CHECK_TEST(owners.size() > 65000 && owners.size() <= 65535, "%zu owners", owners.size());
  HeapSampler::destroy_owner(owners.back());
  CHECK_TEST(HeapSampler::create_owner() == owners.back(), "a destroyed owner is reused");
  for (auto owner : owners) {
    HeapSampler::destroy_owner(owner);
  }

  // a full table of contexts ignores new contexts, but updates existing ones
  auto a = HeapSampler::create_owner();
  auto b = HeapSampler::create_owner();
  std::vector<std::uint64_t> contexts;
  for (std::uint64_t i = 1; i <= 50000; i++) {
    contexts.push_back(i * 0x40);
    HeapSampler::set_context_owner(i * 0x40, a, 0);
  }
  CHECK_TEST(is_owner(contexts[0], a), "registered context");
  CHECK_TEST(is_owner(contexts.back(), 0), "a context in a full table is ignored");
  HeapSampler::set_context_owner(contexts[0], b, 0);
  CHECK_TEST(is_owner(contexts[0], b), "a context is updated in a full table");
  HeapSampler::destroy_owner(b);
  CHECK_TEST(is_owner(contexts[0], 0), "contexts of a destroyed owner are ignored");
  for (auto context : contexts) {
    HeapSampler::remove_context(context, a);
  }
  HeapSampler::remove_context(contexts[0], b);

  // contexts are found after many insertions and removals
  std::mt19937_64 rng(7);
  std::vector<std::uint64_t> kept;
  for (int i = 0; i < 1000; i++) {
    kept.push_back((rng() | 1) << 4);
    HeapSampler::set_context_owner(kept.back(), a, 0);
  }
  for (int i = 0; i < 200000; i++) {
    auto context = (rng() | 1) << 4;
    HeapSampler::set_context_owner(context, a, 1);
    HeapSampler::remove_context(context, a);
  }
  int lost = 0;
  for (std::size_t i = 0; i < kept.size(); i += 10) {
    lost += !is_owner(kept[i], a);
  }
  CHECK_TEST(lost == 0, "%d contexts are lost after churn", lost);
  for (auto context : kept) {
    HeapSampler::remove_context(context, a);
  }
  HeapSampler::destroy_owner(a);

  // threads after the first 4096 share an exact counter; blocks are freed by another thread
  HeapSampler::set_sampling_interval(0);
  auto before = HeapSampler::get_stats();
  auto glibc_before = glibc_in_use();
  std::vector<void *> blocks(4200);
  for (std::size_t i = 0; i < blocks.size(); i++) {
    std::thread([&blocks, i] { blocks[i] = escape(malloc(100 + i)); }).join();
  }
  check_exact_counter("4200 threads", before, glibc_before, 0);
  for (auto p : blocks) {
    free(p);
  }
  check_exact_counter("blocks of 4200 threads are freed", before, glibc_before, 0);
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
  const std::vector<std::pair<std::string, void (*)()>> all_tests = {
      {"stress", test_stress},    {"exact", test_exact}, {"attribution", test_attribution},
      {"edge", test_edge},        {"api", test_api},     {"limits", test_limits},
      {"drift", test_drift},      {"bias", test_bias},   {"accuracy", test_accuracy},
      {"overhead", test_overhead}};
  std::vector<std::string> names(argv + 1, argv + argc);
  if (names.empty()) {
    for (auto &test : all_tests) {
      names.push_back(test.first);
    }
  }
  for (auto &name : names) {
    auto it = std::find_if(all_tests.begin(), all_tests.end(), [&](const auto &test) { return test.first == name; });
    if (it == all_tests.end()) {
      std::printf("unknown test %s\n", name.c_str());
      return 2;
    }
    it->second();
  }
  std::printf(failures == 0 ? "ALL CHECKS PASSED\n" : "%d CHECKS FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
