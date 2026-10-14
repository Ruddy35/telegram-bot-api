//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/utils/common.h"

namespace telegram_bot_api {

// Accounting of heap memory by owners, for example, by bots.
//
// The memory allocation functions and the C++ operators new and delete are replaced. The replacements forward calls to
// the next allocator in the symbol lookup order (glibc or an allocator loaded with LD_PRELOAD) and
//  - count the total size of live allocations exactly, in sizes of chunks of the real allocator;
//  - sample allocations: an allocation of n bytes is sampled with the probability p = 1 - exp(-(n + 1) / interval),
//    independently of other allocations, and is accounted with the weight chunk / p for the owner of the actor context,
//    in which it was made. The sum of weights of live sampled allocations of an owner is an unbiased estimate of the
//    total size of its live allocations; the sum of weight * (weight - chunk) is an unbiased estimate of the variance.
// A chunk of a sampled allocation is the chunk, which the real allocator would use for it. Allocations with an
// alignment bigger than 4 KB and allocations bigger than 64 GB are never sampled.
// Sampled allocations are stored in a reserved address range, so free() recognizes them by an address comparison.
class HeapSampler {
 public:
  static constexpr td::int32 PART_COUNT = 3;  // memory of an owner is accounted separately for each part

  // returns false if allocation functions aren't replaced
  static bool is_supported();

  // returns true if memory of owners is also accounted exactly. The audit mode is enabled by the environment variable
  // TELEGRAM_BOT_API_MEMORY_AUDIT=1 and adds a 16-byte header to every allocation, which isn't sampled. It is used only
  // to verify the estimates. An allocation, which is freed at the same time as its owner is destroyed, can be
  // accounted for the wrong owner.
  static bool is_audit_enabled();

  // sets the mean number of allocated bytes between samples; 0 disables the sampling. The current thread uses the new
  // interval immediately, other threads use it after allocating the number of bytes left until their next sample.
  // The sampling is enabled from the start of the process with the interval of 64 KB, unless memory pages aren't 4 KB.
  static void set_sampling_interval(td::int64 interval);

  static td::int64 get_sampling_interval();

  // creates a new owner; returns 0 if there are too many owners
  static td::int32 create_owner();

  // memory of the destroyed owner is moved to the unattributed memory; contexts of the owner are ignored from now on
  static void destroy_owner(td::int32 owner);

  // allocations made in the actor context will be attributed to the owner; the last call for a context wins. If there
  // are too many contexts, then a new context is ignored and allocations made in it remain unattributed.
  static void set_context_owner(td::uint64 context, td::int32 owner, td::int32 part);

  // removes the context if it is still owned by the owner
  static void remove_context(td::uint64 context, td::int32 owner);

  // attributes allocations made by the current thread to another part of the owner of the current context
  class PartGuard {
   public:
    explicit PartGuard(td::int32 part);
    PartGuard(const PartGuard &) = delete;
    PartGuard &operator=(const PartGuard &) = delete;
    PartGuard(PartGuard &&) = delete;
    PartGuard &operator=(PartGuard &&) = delete;
    ~PartGuard();

   private:
    td::int32 old_part_;
  };

  struct OwnerStats {
    td::int64 bytes[PART_COUNT] = {};        // estimated size of live allocations
    td::int64 sample_count = 0;              // number of live sampled allocations
    double variance = 0.0;                   // unbiased estimate of the variance of the sum of bytes
    td::int64 exact_bytes[PART_COUNT] = {};  // exact size of live allocations; only in the audit mode, not for owner 0
  };
  // owner 0 is the unattributed memory
  static OwnerStats get_owner_stats(td::int32 owner);

  static constexpr std::size_t DURATION_BUCKET_COUNT = 6;  // including +Inf
  static constexpr td::uint64 DURATION_BUCKETS_NS[DURATION_BUCKET_COUNT - 1] = {1000, 10000, 100000, 1000000, 10000000};

  struct Stats {
    td::int64 live_bytes = 0;              // exact size of all live allocations
    td::int64 estimated_live_bytes = 0;    // estimated size of all live allocations, including unattributed
    double estimate_variance = 0.0;        // unbiased estimate of the variance of estimated_live_bytes
    td::int64 sample_count = 0;            // number of live sampled allocations
    td::int64 sampled_bytes = 0;           // exact size of live sampled allocations
    td::int64 sampler_overhead_bytes = 0;  // memory used by the sampler itself
    td::int64 audit_live_bytes = 0;        // exact size of all live allocations as accounted for owners; audit only

    // allocations and deallocations of sampled blocks; they take the lock of the sampler for a few operations
    td::uint64 operation_counts[DURATION_BUCKET_COUNT] = {};  // by duration, including waiting for the lock
    td::uint64 total_operation_ns = 0;
    td::uint64 max_operation_ns = 0;
    td::uint64 max_lock_wait_ns = 0;  // by all operations, which take the lock
  };
  static Stats get_stats();
};

}  // namespace telegram_bot_api
