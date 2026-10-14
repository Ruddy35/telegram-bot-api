//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/utils/common.h"

namespace telegram_bot_api {

// Accounting of heap memory by owner. All memory allocation functions are replaced with wrappers, which forward calls
// to the next allocator in the symbol lookup order (glibc or a preloaded allocator) and
//  - exactly count total size of live allocations;
//  - sample allocations: an allocation of n bytes is sampled with probability 1 - exp(-(n + 1) / interval) and accounted with
//    weight chunk_size / probability for the owner of the actor context in which it was made. Sum of weights of live
//    sampled allocations is an unbiased estimate of memory used by an owner.
// Sampled allocations are stored in a reserved address range, so free() recognizes them by an address comparison.
class HeapSampler {
 public:
  static constexpr td::int32 PART_COUNT = 3;  // memory of an owner is accounted separately for each part

  // returns false if allocation functions aren't replaced
  static bool is_supported();

  // returns true if memory of owners is also accounted exactly; enabled by the environment variable
  // TELEGRAM_BOT_API_MEMORY_AUDIT=1, which adds 16 bytes to every allocation; used to verify the estimates
  static bool is_audit_enabled();

  // sets mean number of allocated bytes between samples; 0 disables the sampling. The current thread uses the new
  // interval immediately, other threads use it after allocating the number of bytes left until their next sample.
  // The sampling is enabled from the start of the process with the interval of 64 KB
  static void set_sampling_interval(td::int64 interval);

  static td::int64 get_sampling_interval();

  // creates a new owner; returns 0 if there are too many owners
  static td::int32 create_owner();

  // memory of the destroyed owner is moved to the unattributed memory
  static void destroy_owner(td::int32 owner);

  // allocations made in the actor context will be attributed to the owner; the last call for a context wins
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
    td::int64 exact_bytes[PART_COUNT] = {};  // exact size of live allocations; only in audit mode
  };
  static OwnerStats get_owner_stats(td::int32 owner);

  static constexpr std::size_t DURATION_BUCKET_COUNT = 6;
  static constexpr td::uint64 DURATION_BUCKETS_NS[DURATION_BUCKET_COUNT - 1] = {1000, 10000, 100000, 1000000, 10000000};

  struct Stats {
    td::int64 live_bytes = 0;              // exact size of all live allocations
    td::int64 estimated_live_bytes = 0;    // estimated size of all live allocations, including unattributed
    double estimate_variance = 0.0;        // unbiased estimate of the variance of estimated_live_bytes
    td::int64 unattributed_bytes = 0;      // estimated size of allocations not made by any owner
    td::int64 sample_count = 0;            // total number of live sampled allocations
    td::int64 sampled_bytes = 0;           // exact size of live sampled allocations
    td::int64 sampler_overhead_bytes = 0;  // memory used by the sampler itself
    td::int64 audit_live_bytes = 0;        // exact size of all live allocations as accounted for owners; audit only

    // operations with sampled allocations, which are the only operations taking a lock
    td::uint64 operation_counts[DURATION_BUCKET_COUNT] = {};  // by duration, including waiting for the lock
    td::uint64 total_operation_ns = 0;
    td::uint64 max_operation_ns = 0;
    td::uint64 max_lock_wait_ns = 0;
  };
  static Stats get_stats();
};

}  // namespace telegram_bot_api
