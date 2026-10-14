# Verification

This document lists the claims about the changes of the fork, and for each claim how it is proven or tested, the
command to repeat the check and the result. All tests run without network access.

```
test/run_all.sh <build dir>   # all tests below except the long experiments; prints ALL TESTS PASSED
```

| Claim | Evidence | Command | Result |
|---|---|---|---|
| A graceful restart doesn't fail requests, lose or duplicate updates, or change the webhook | Integration scenarios with a real server | `test/restart/restart_test.py` | All outcomes below pass |
| Every answered request of a bot is counted exactly once, and the metrics page is valid | Scenarios compare the counted requests with the requests answered to the test; every page read by the tests is validated | `test/restart/restart_test.py` | Pass |
| The total heap size is counted exactly | Comparison with statistics of glibc and jemalloc | `heap-sampler-test exact stress drift limits` | Equal to the byte |
| The memory estimate of a bot is unbiased and its reported standard deviation σ is right | A proof below; 1000 independent model heaps; 200 independent real servers | `heap-sampler-test bias`, `verify_memory_accuracy.py offline` | Mean z within ±0.15 and coverage of 2σ 94.5-98% on model heaps; 93.8-95.2% on real servers |
| Memory accounting doesn't break allocations | Concurrent stress of all allocation functions with pattern checks; all results and errors of each function | `heap-sampler-test stress api edge limits` | 0 errors with glibc and jemalloc, in the normal and audit modes |
| Memory accounting doesn't add latency or stalls | Concurrent A/B test of the server with and without memory accounting | `latency_ab.py` | No failures of 1080120 requests; an increase of p50, p99 and p99.9 latency is rejected; +0.9% CPU time per request |
| Documentation describes the code | All declared metrics are documented; all documented options exist | `test/test_docs.py` | Pass |

## Graceful restarts

`test/restart/restart_test.py <build dir> [scenario]...` runs a real server for every scenario. Test bots are marked as
authorized in their databases by `restart-test-fake-auth`, and updates are added to the update queue by
`restart-test-fake-updates`. Every scenario checks that the server exits with the code 0 and logs no errors; 11
scenarios also check that the server counted exactly the requests of the first bot, which were answered to the test,
for example, 80460 requests during 10 restarts in `policy`.

| Scenario | Outcome |
|---|---|
| `webhook` | 3 restarts while 300 updates are delivered to a webhook with 10 connections, which calls an API method before answering, under a load of requests: every update is delivered exactly once, none of about 23000 requests fails and the slowest takes 0.52 seconds, the webhook parameters and the secret token are kept |
| `polling` | An active long polling request is answered immediately; polling continues after the restart |
| `polling_pending` | Updates, which weren't confirmed before the restart, and `getUpdates` requests delayed by the restart are served by the new instance: every update is received exactly once and in order |
| `drain_timeout` | A request, which can't finish, prevents the restart: the restart is cancelled after the timeout, nothing is delayed, the bot works |
| `cancel_with_getupdates` | A `getUpdates` request delayed by a cancelled restart is answered by the old instance |
| `webhook_change` | `setWebhook` during a restart cancels it; no update is delivered twice |
| `close_during_restart` | `close` during a restart cancels it; delayed requests are answered |
| `shutdown_during_restart` | The server is stopped during a restart: it exits in time, delayed requests are answered |
| `restart_all` | `?restart=all` restarts all bots one by one, one at a time |
| `policy` | Restarts because of the memory limit and the uptime; the heap after 10 restarts is within 10% of the heap after 2 |
| `policy_limits` | The minimum uptime and the cooldown are respected |
| `malloc_trim` | Free memory is returned to the OS once after restarts |
| `options` | Invalid values of all new options are rejected |
| `metrics` | Requests by status code and reason, request durations except `getUpdates`, all types of input files, abandoned requests, rejected requests, webhook deliveries by result, filtering by bot, removal of a closed bot, a single series for several tokens of a bot |
| `metrics_disabled` | Without CPU and memory sampling their metrics aren't exported, and the bot memory limit is ignored with an error |
| `resources` | CPU time of bots and threads adds up to the CPU time of the process and continues after restarts; disk usage, disk I/O and TDLib object counts are exported |
| `memory` | Memory of each bot is estimated within 5σ of the exact heap, attributed to the right bot, and the bot memory limit restarts only the bot, which exceeds it, without memory leaks |

The offline tests can't cover what needs Telegram servers: receiving of new updates from TDLib, downloading and sending
of files, `getDifference` after reopening of an instance, and cancellation of a restart because updates arrived after
network was disabled. They don't cover two safety limits, which can't be reached in a test of reasonable size: 10000
delayed requests and a restart lasting longer than the drain timeout plus 120 seconds.

## Memory accounting

### Why the estimate is right

**Sampling.** Each thread counts allocated bytes. Sampling is a Poisson process with the rate `1/T` on the axis of
allocated bytes, where `T` is `--memory-sampling-interval`: the number of bytes until the next sample is exponentially
distributed. An allocation of `n` bytes takes `n + 16` bytes of the axis and is sampled if a sample point falls into
it. Numbers of points of a Poisson process in disjoint segments are independent, so allocations are sampled
**independently** with the probability

    p = 1 - exp(-(n + 16) / T)

The 16 extra bytes approximate the header of a chunk, so even empty allocations can be sampled, and the weight of a
small allocation, defined below, is close to `T`. With `n + 1` instead, an empty allocation in a 32-byte chunk would
have the weight `32 T`; such rare heavy samples make the estimate skewed, and the interval of ±2 reported σ contained
the real value only in about 90% of model heaps of tiny allocations instead of 95%.

**Estimate.** Let a bot have live allocations `i` with sizes `c_i` of their chunks in the real allocator, which are the
units of the exact counter. A sampled allocation has the weight `w_i = c_i / p_i`. The estimate is the sum of the
weights of live sampled allocations:

    X̂ = Σ I_i c_i / p_i, where I_i = 1 if the allocation is sampled

This is the Horvitz-Thompson estimator: `E[X̂] = Σ p_i c_i / p_i = Σ c_i = X`, so the estimate is **unbiased** for any
sizes. A freed allocation subtracts exactly the weight, which was added (sampled allocations are placed in a reserved
address range, so `free` recognizes them by the address). Which allocations are freed doesn't depend on the sampling,
so at any moment the estimate is the Horvitz-Thompson estimate of the set of live allocations, without accumulation of
errors.

**Error.** The terms are independent, so

    Var X̂ = Σ c_i² (1 - p_i) / p_i

The server doesn't know allocations, which weren't sampled, but estimates the variance without bias from the sampled
ones:

    V̂ = Σ I_i w_i (w_i - c_i),   E[V̂] = Σ p_i (c_i / p_i) (c_i / p_i - c_i) = Var X̂

`sqrt(V̂)` is exported as `tgbotapi_bot_memory_error_bytes`. For allocations much smaller than `T` the term is about
`T c² / (n + 16) ≈ T c`, and allocations much bigger than `T` are almost always sampled and add almost no error, so
`σ ≈ sqrt(T X)`:
with `T = 64 KB` the relative error σ / X is 5.7% for 20 MB, 2.6% for 100 MB, 1.1% for 500 MB and 0.6% for 2 GB.

**Distribution.** `X̂` is a sum of many independent bounded terms, so by the central limit theorem it is almost
normal if σ is much bigger than `T`: the real value is within 1σ, 2σ and 3σ of the estimate in 68.3%, 95.4% and 99.7%
of cases. Without assumptions about the distribution Chebyshev's inequality guarantees at least 89% within 3σ. With
few sampled allocations (`tgbotapi_bot_memory_samples` below about 100) `V̂` itself is rough, and errors are more often
underestimates.

**What isn't statistical.** The owner of an allocation is determined exactly: an allocation belongs to the bot, in
whose actor context it was made, until it is freed. The audit mode, in which every allocation is marked with its owner,
checks this rule. The total heap size `tgbotapi_heap_bytes` is exact.

### Model heaps: `heap-sampler-test`

```
cmake --build <build dir> --target heap-sampler-test
<build dir>/heap-sampler-test [test]...
LD_PRELOAD=<path to libjemalloc.so.2> <build dir>/heap-sampler-test [test]...
TELEGRAM_BOT_API_MEMORY_AUDIT=1 <build dir>/heap-sampler-test [test]...
```

| Test | Checks |
|---|---|
| `stress` | 8 threads call all allocation functions with sampling every 4 KB; contents of every block are checked by a pattern, `calloc` by zeros, alignment and `malloc_usable_size` by their contracts; the exact counter equals the allocator statistics after all threads finish |
| `exact` | The exact counter equals `mallinfo2` of glibc or `stats.allocated` of jemalloc with a million live blocks |
| `attribution` | Blocks stay with their owner when freed by another thread in another context; `realloc` moves a block to the current owner; parts; the last registration of a context wins; memory of destroyed owners becomes unattributed |
| `edge` | Empty and huge blocks, failed allocations, lazily committed big blocks, disabled sampling, reuse of owner slots, `fork` during sampling |
| `api` | Results and `errno` of every function on errors, all forms of `operator new` and `delete`, zeroing on every reuse path, all `realloc` transitions, the size of the cache of freed runs |
| `limits` | 65535 owners, a full table of contexts, 200000 context changes, 4200 threads |
| `drift` | After 20 million operations and freeing of everything the estimate is exactly 0 and the exact counter has no drift |
| `bias` | 200 independent heaps for each of 5 distributions of sizes: mean z = (estimate - exact) / σ within ±3/√200, standard deviation of z between 0.8 and 1.2, mean `V̂` within 10% of the theoretical variance, coverage of 1σ, 2σ, 3σ at least 58%, 90%, 98%; exact owner memory in the audit mode |
| `accuracy` | The same with continuous allocation and freeing in 4 threads |
| `overhead` | Cost of a pair of `malloc` and `free` with and without sampling |

Results of `bias` with glibc:

```
tiny sizes 0-63                    interval   64 KB, 200 heaps of 20 MB: mean error -0.476%, z mean -0.081 (must be within +-0.21), z stddev 1.00
                                   reported sigma: variance / theoretical 0.994, exact value within 1/2/3 sigma: 72.0%/95.0%/99.5%
mixed sizes                        interval   64 KB, 200 heaps of 50 MB: mean error +0.083%, z mean +0.023 (must be within +-0.21), z stddev 0.90
                                   reported sigma: variance / theoretical 1.001, exact value within 1/2/3 sigma: 72.5%/97.0%/100.0%
mixed sizes                        interval  256 KB, 200 heaps of 50 MB: mean error -0.149%, z mean -0.021 (must be within +-0.21), z stddev 0.98
                                   reported sigma: variance / theoretical 0.998, exact value within 1/2/3 sigma: 68.5%/94.5%/100.0%
heavy-tailed sizes (Pareto 1.1)    interval   64 KB, 200 heaps of 50 MB: mean error -0.003%, z mean +0.010 (must be within +-0.21), z stddev 0.90
                                   reported sigma: variance / theoretical 0.999, exact value within 1/2/3 sigma: 73.5%/97.0%/100.0%
sizes 0-200 KB around the interval interval   64 KB, 200 heaps of 200 MB: mean error +0.065%, z mean +0.062 (must be within +-0.21), z stddev 0.99
                                   reported sigma: variance / theoretical 1.001, exact value within 1/2/3 sigma: 70.5%/94.5%/100.0%
```

All tests pass with glibc and jemalloc, in the normal and audit modes. Over these 20 combinations of a distribution and
a mode mean z is between -0.15 and +0.06, the standard deviation of z is between 0.90 and 1.05, the mean reported
variance is 0.992-1.002 of the theoretical one, and the exact value is within 1σ, 2σ, 3σ of the estimate in
62-73.5%, 94.5-98% and 99-100% of heaps.

### Real servers: `verify_memory_accuracy.py offline`

```
python3 test/verify/verify_memory_accuracy.py offline <build dir> --runs=100 --intervals=64,16 --csv=accuracy.csv
```

Each run is a new server process in the audit mode with two test bots. Each bot gets a random number (10-20000) of
`sendMessage` requests with random sizes, which stay in memory of its TDLib instance and client without network; in
half of the runs the TDLib instance of the first bot is restarted before. Then the estimates are compared with the
exact values. Runs are independent, so all observations are independent, and the checks are strict: the coverage of
1σ, 2σ, 3σ must be at least the normal one minus 3 standard deviations of the binomial distribution minus 1%, mean z
must be within ±3/√n, the standard deviation of z must be at most 1 + 3/√(2n) + 0.05, and the estimate of the whole
heap must be within 4σ in every run.

Results of 100 runs for each interval (bots with at least 30 expected samples):

| Interval | Observations | Within 1σ | Within 2σ | Within 3σ | Mean z | Standard deviation of z | Total error |
|---|---|---|---|---|---|---|---|
| 64 KB | 167 | 64.7% | 95.2% | 98.8% | -0.03 (allowed ±0.23) | 1.06 (allowed 1.21) | +0.31% of 712 MB |
| 16 KB | 193 | 66.3% | 93.8% | 99.5% | -0.06 (allowed ±0.22) | 1.07 (allowed 1.20) | +0.08% of 737 MB |
| Normal distribution | | 68.3% | 95.4% | 99.7% | 0 | 1 | |

The estimate of the whole heap was within 2.7σ and 3.6σ in all runs. Test bots use only 1-10 MB of memory, so their
relative errors are big (median 7.6% with 64 KB and 3.8% with 16 KB), exactly as large as the server reports for such
amounts; the error decreases as `1/sqrt(X)`. 3 of 360 observations are beyond 3σ, while about 1 is expected; they have
11, 69 and 256 samples, and 2 of them are underestimates. Of the 131 observations with at least 100 samples one is
beyond 3σ (-3.09σ). For the bot memory limit an underestimate is the safe side: it can only postpone a restart.

### A running server: `verify_memory_accuracy.py watch`

1. Run a server with `TELEGRAM_BOT_API_MEMORY_AUDIT=1`. The mode costs 16 bytes per live allocation and a little CPU,
   so it is intended for a canary server. The estimates are the same as without the mode.
2. Run the check, for example, for an hour:

   ```
   python3 test/verify/verify_memory_accuracy.py watch http://<stat address>:<stat port>/metrics --duration=3600 --period=60
   ```

The tool computes z for every bot with enough samples in every scrape and prints `ALL CHECKS PASSED` or the failed
checks. Estimates of different bots are independent; consecutive scrapes of a bot are not, so the tolerances use the
smallest number of bots in a scrape.

## No added latency

**The fast path** of almost every allocation decrements the byte counter of the thread, reads the chunk size from the
header of the real allocator and adds it to the exact counter of the thread: no locks, system calls or allocations.

**The slow path**, once per `T` bytes on average, places the sampled block in the reserved range and updates free
lists, the context table and owner counters under a mutex: O(1) work without allocations and system calls, except
`mmap` for blocks of 32 MB and more, as glibc does. Freed sampled runs up to 1 MB are cached (up to 8 MB); other runs
are returned to the OS with `madvise` outside of the mutex. `calloc` zeroes memory outside of the mutex too. The
duration of slow operations including waiting for the mutex is exported in
`tgbotapi_memory_sampler_operation_seconds`.

**A/B test: `latency_ab.py`.** A server with memory accounting and a baseline server built with
`-DTELEGRAM_BOT_API_HEAP_SAMPLING=OFF` run at the same time and get the same load: 2000 requests per second over 64
connections (`getMe` and `getWebhookInfo` answered by Bot API, `getChatMember` and `getFile` answered by TDLib, and
requests with 100 KB bodies) and probes sent by a fixed schedule: `getMe` every 2 ms and `getChatMember` every 5 ms.
A probe latency is measured from its scheduled time, so a stall of a server delays all probes scheduled during the
stall and can't be hidden. A pause of the whole machine delays both servers at the same time, so the paired
comparison of rounds cancels it out; a probe stalled while probes of the other server weren't is an own stall of the
server.

```
python3 test/verify/latency_ab.py --build=<build dir> --config=baseline=<baseline build dir>/telegram-bot-api \
    --config=accounting=<build dir>/telegram-bot-api --rounds=40 --duration=10 --json=latency.json
python3 test/verify/latency_ab.py --analyze=latency.json   # report saved results again
```

Every statistic is compared over paired rounds: the Hodges-Lehmann estimate of the difference and the 95% confidence
interval of the Wilcoxon signed-rank test, which a single disturbed round can't spoil. `PASS` means that an increase
bigger than the allowed one is rejected at the significance level of 2.5%.

Results of 40 rounds of 10 seconds on a virtual machine with 4 cores, where the load generator shares the cores with
both servers:

| Statistic | Baseline | Accounting | Difference, 95% CI | Allowed | |
|---|---|---|---|---|---|
| Failed requests | 0 of 1080120 | 0 of 1080120 | | 0 | PASS |
| `getMe` probes, p50 | 0.808 ms | 0.812 ms | +0.005 [-0.000, +0.010] ms | +0.2 ms | PASS |
| `getMe` probes, p99 | 2.671 ms | 2.684 ms | +0.041 [-0.213, +0.281] ms | +0.5 ms | PASS |
| `getMe` probes, p99.9 | 12.93 ms | 14.30 ms | -0.20 [-1.38, +1.07] ms | +3.23 ms | PASS |
| `getChatMember` probes, p50 | 1.918 ms | 1.921 ms | -0.003 [-0.016, +0.012] ms | +0.2 ms | PASS |
| `getChatMember` probes, p99 | 4.561 ms | 4.554 ms | -0.081 [-0.409, +0.160] ms | +0.68 ms | PASS |
| `getChatMember` probes, p99.9 | 20.28 ms | 18.51 ms | -1.19 [-3.23, +0.56] ms | +5.07 ms | PASS |
| `getMe` probes delayed by more than 10 ms by the server itself | 0.000% | 0.000% | +0.000 [-0.020, +0.000]% | +0.1% | PASS |
| `getChatMember` probes delayed by more than 10 ms by the server itself | 0.050% | 0.000% | +0.000 [-0.025, +0.025]% | +0.1% | PASS |
| CPU time per request | 106.3 µs | 107.2 µs | +0.93 [+0.56, +1.30] µs | +5.3 µs | PASS |
| Peak RSS | 41.8 MB | 43.1 MB | | | |

The columns of the servers are medians over the rounds. During the measurements the server with accounting made
356271 operations with sampled allocations: 29 of them took longer than 0.1 ms and 2 longer than 1 ms, and the
longest wait for the lock was 0.87 ms. Timers of the threads were delayed by more than 10 ms 285 times in the baseline
and 269 times with accounting, and by more than 100 ms 3 and 6 times; such delays happen at the same time in both
servers and are caused by the virtual machine.

On a running server the same is visible all the time: `tgbotapi_memory_sampler_operation_seconds`,
`tgbotapi_memory_sampler_max_lock_wait_seconds`, `tgbotapi_scheduler_lag_seconds` and
`tgbotapi_bot_request_duration_seconds`.

## Metrics

Every scenario of `restart_test.py` validates every metrics page it reads: each family has one `HELP` and one `TYPE`,
counter names end with `_total`, there are no duplicate series and no bot tokens, counters aren't negative, histogram
buckets are cumulative and end with `+Inf` equal to `_count`. The scenarios `metrics`, `metrics_disabled`, `resources`
and `memory` check the values listed above. `test/test_docs.py` checks that [METRICS.md](METRICS.md) documents exactly
the metrics declared by the server.
