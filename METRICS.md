# Metrics

The server exports metrics of each bot and of the whole server in the Prometheus text format, which can be collected
by Prometheus or VictoriaMetrics.

```
curl http://127.0.0.1:<stat port>/metrics             # all bots and the server
curl http://127.0.0.1:<stat port>/metrics?bot=<label>  # metrics of one bot only
```

The statistics port (`--http-stat-port`) must be accessible only from the internal network.

Example of a scrape configuration:

```yaml
scrape_configs:
  - job_name: telegram-bot-api
    scrape_interval: 15s
    static_configs:
      - targets: ['bot-api-host:<stat port>']
```

## Semantics

* Events are counted by cumulative counters at the time of the event, so `increase()` and `rate()` return exact
  numbers of events for any interval regardless of the scrape interval.
* Metrics of a bot are kept by `ClientManager`, so they aren't reset by graceful restarts of the TDLib instance of the
  bot ([RESTARTS.md](RESTARTS.md)). They are removed together with the bot after `logOut`, `close` or closing of an
  inactive bot; counters start from zero if the bot returns, which `increase()` handles as any counter reset.
* Metrics are exported only for bots, which were authorized, to avoid series of invalid tokens. If several tokens of
  the same bot are in use, then only metrics of the newest one are exported.
* The label `bot` is the bot identifier, which is the part of the token before the colon, with the suffix `_test` for
  the test environment. Tokens are never exported.
* `method` is the method name in lower case; requests to unknown methods are counted as `unknown`.
* All counters are changed without locks by the thread of `ClientManager`, which also builds the page.

## Metrics of a bot

Every metric in this section has the label `bot`.

### Requests

| Metric | Labels | Meaning |
|---|---|---|
| `tgbotapi_bot_requests_total` | `method`, `code`, `reason` | Answered requests by HTTP status code and failure reason |
| `tgbotapi_bot_method_duration_seconds_total` | `method` | Total time between receiving of requests and sending of answers |
| `tgbotapi_bot_request_duration_seconds` | `le` | Histogram of the time between receiving of requests and sending of answers, except `getUpdates`, which is a long polling request |
| `tgbotapi_bot_request_bytes_total` | `method` | Total size of requests including headers, excluding uploaded files |
| `tgbotapi_bot_response_bytes_total` | `method` | Total size of answers |
| `tgbotapi_bot_abandoned_requests_total` | `method` | Requests, connection of which was closed by the client before an answer was sent; they are counted in `tgbotapi_bot_requests_total` too, when they are answered |
| `tgbotapi_bot_input_files_total` | `method`, `type` | Files passed to methods: `upload` for multipart uploads, `local_path` for local files in the `--local` mode, `file_id` for files, which were already sent, `url` for files downloaded by Telegram |
| `tgbotapi_bot_input_file_bytes_total` | `method`, `type` | Total size of the files; known only for the types `upload` and `local_path` |
| `tgbotapi_bot_active_requests` | | Requests being processed |
| `tgbotapi_bot_active_file_uploads` | | Files being uploaded in requests |
| `tgbotapi_bot_active_file_upload_bytes` | | Total size of files being uploaded in requests |

Values of `reason`:

| `reason` | Meaning |
|---|---|
| `none` | An ordinary answer, including ordinary errors |
| `telegram_flood` | 429 because of flood limits of Telegram servers |
| `server_flood` | 429 because of flood limits of the server: too many active requests or uploads |
| `set_webhook_flood` | 429 because `setWebhook` is called too often |
| `close_flood` | 429 because `close` is called too early after the start |
| `closing` | The TDLib instance of the bot is being closed or logged out |
| `restart_queue_full` | 429 because too many requests were delayed during a restart |
| `restart_timeout` | 429 because a restart lasted too long |
| `dropped` | The request was destroyed without an answer, for example, during server shutdown |

### Updates and webhook

| Metric | Labels | Meaning |
|---|---|---|
| `tgbotapi_bot_updates_total` | `type`, `result` | Updates by type (`message`, `callback_query`, ...) and result: `queued`, `filtered` by `allowed_updates` or `failed` to be added to the queue |
| `tgbotapi_bot_fetched_updates_total` | | Updates returned by `getUpdates` |
| `tgbotapi_bot_pending_updates` | | Updates waiting to be received by the bot |
| `tgbotapi_bot_webhook_deliveries_total` | `result` | Attempts to send an update to the webhook: `ok` for a 2xx answer, `http_error` for another answer, `connection_closed` if the connection failed or was closed before an answer; the update is sent again after a failed attempt |
| `tgbotapi_bot_webhook_response_seconds_total` | | Total time of waiting for answers of the webhook |
| `tgbotapi_bot_webhook_dropped_updates_total` | | Updates, which were dropped, because they expired after failed attempts |

### File downloads by `getFile`

| Metric | Labels | Meaning |
|---|---|---|
| `tgbotapi_bot_file_downloads_total` | `result` | Finished downloads: `completed`, `cached` if the file was already downloaded, `too_big` without `--local`, `error`, `closing` if the TDLib instance was closed. A download is counted once even if many requests wait for it |
| `tgbotapi_bot_downloaded_bytes_total` | | Total size of completed downloads |
| `tgbotapi_bot_active_file_downloads` | | Files being downloaded |

### TDLib instance and caches

| Metric | Labels | Meaning |
|---|---|---|
| `tgbotapi_bot_info` | `username`, `webhook` | Always 1; `webhook` is 1 if a webhook is set |
| `tgbotapi_bot_instance_uptime_seconds` | | Time since the start of the current TDLib instance |
| `tgbotapi_bot_tdlib_updates_total` | | Updates received from TDLib |
| `tgbotapi_bot_tdlib_restarts_total` | | Graceful restarts of the TDLib instance |
| `tgbotapi_bot_tdlib_restart_in_progress` | | 1 during a restart, otherwise 0 |
| `tgbotapi_bot_tdlib_restart_delayed_requests` | | Requests delayed by the current restart |
| `tgbotapi_bot_tdlib_objects` | `type` | Objects in memory of the TDLib instance, which are never deleted or deleted only after some time; updated once a minute, see below |
| `tgbotapi_bot_storage_bytes` | `type` | Size of `files` and `database` of the bot on the disk; updated once a minute |
| `tgbotapi_bot_storage_files` | | Number of files of the bot on the disk; updated once a minute |
| `tgbotapi_bot_cached_objects` | `type` | Objects in the Bot API cache: `message`, `user`, `group`, `supergroup`, `chat`, `community`, `business_connection`, `sticker_set_name` |
| `tgbotapi_bot_pending_update_chats` | | Chats with partially handled updates |
| `tgbotapi_bot_pending_sent_messages` | | Requests waiting for messages to be sent |

Values of `type` of `tgbotapi_bot_tdlib_objects`:

| `type` | Objects |
|---|---|
| `file_id`, `file_node` | File identifiers and file nodes of `FileManager`; never deleted |
| `remote_file_location`, `local_file_location` | Known files on Telegram servers and on the disk |
| `file_source`, `file_reference_node`, `message_file_source` | Sources of file references; never deleted |
| `chat` | Chats; never deleted |
| `message` | Messages in memory; unloaded after `message_unload_delay` |
| `user`, `user_full_info` | Users and their full information; never deleted |
| `basic_group`, `basic_group_full_info`, `supergroup`, `min_supergroup`, `supergroup_full_info` | Groups and channels; never deleted |
| `animation`, `audio`, `document`, `sticker`, `sticker_set`, `video`, `video_note`, `voice_note` | Media; never deleted |
| `web_page`, `poll`, `story` | Link previews, polls and stories |

The numbers are sizes of containers of TDLib, so they are cheap to get. They need a small patch of TDLib from
[td-patches](td-patches/README.md).

### Resources

| Metric | Labels | Meaning |
|---|---|---|
| `tgbotapi_bot_cpu_seconds_total` | `part` | Estimated CPU time: `client` for the Bot API part, `tdlib` for the TDLib instance including its work in the database and file threads; see [CPU accounting](#cpu-accounting) |
| `tgbotapi_bot_memory_bytes` | `part` | Estimated memory: `client` for the Bot API part, `tdlib` for the TDLib instance, `updates` for pending updates; see [memory accounting](#memory-accounting) |
| `tgbotapi_bot_memory_error_bytes` | | Estimated standard deviation of the sum of `tgbotapi_bot_memory_bytes` |
| `tgbotapi_bot_memory_samples` | | Number of live sampled allocations of the bot |
| `tgbotapi_bot_memory_exact_bytes` | `part` | Exact memory of the bot; only in the audit mode |

## Metrics of the server

| Metric | Labels | Meaning |
|---|---|---|
| `tgbotapi_uptime_seconds` | | Time since the server start |
| `tgbotapi_process_resident_memory_bytes`, `tgbotapi_process_virtual_memory_bytes` | | Memory of the process |
| `tgbotapi_process_cpu_seconds_total` | `mode` | CPU time of the process: `user` or `system`; Linux only |
| `tgbotapi_process_disk_read_bytes_total`, `tgbotapi_process_disk_written_bytes_total` | | Disk I/O of the process; Linux only |
| `tgbotapi_buffer_memory_bytes` | | Memory of network buffers |
| `tgbotapi_active_requests` | | Requests being processed |
| `tgbotapi_active_network_queries` | | Queries to Telegram servers being processed |
| `tgbotapi_webhook_connections` | | Open webhook connections |
| `tgbotapi_tqueue_expired_updates_total` | | Updates deleted from the update queue, because they expired before they were received |
| `tgbotapi_tdlib_restarts_total`, `tgbotapi_tdlib_failed_restarts_total` | | Finished and cancelled graceful restarts of TDLib instances |
| `tgbotapi_rejected_requests_total` | `reason` | Requests, which weren't passed to a bot: `invalid_token`, `misdirected_token` (the token doesn't belong to the server), `closed_bot` (`close` of a bot, which isn't running), `ip_flood_limit` and `global_flood_limit` (too many new bots), `closing` (the server is closing) |
| `tgbotapi_bots` | `state` | Bot tokens in use: `authorized` or `unauthorized` |
| `tgbotapi_unauthorized_bot_requests_total` | `code` | Answered requests of bots, which were closed before authorization, by HTTP status code |
| `tgbotapi_heap_bytes` | | Exact size of all live memory allocations of the process |
| `tgbotapi_heap_estimated_bytes`, `tgbotapi_heap_estimate_error_bytes` | | Sum of the estimates of all memory and its standard deviation; must be close to `tgbotapi_heap_bytes` |
| `tgbotapi_memory_unattributed_bytes` | | Estimated memory outside of the bots listed on the page: HTTP connections, uploaded files, shared data |
| `tgbotapi_memory_samples`, `tgbotapi_memory_sampling_interval_bytes`, `tgbotapi_memory_sampler_overhead_bytes` | | Number of live sampled allocations, the sampling interval and memory of the accounting itself |
| `tgbotapi_memory_sampler_operation_seconds` | `le` | Histogram of duration of operations with sampled allocations including waiting for the lock; they are the only operations of the accounting, which take a lock |
| `tgbotapi_memory_sampler_max_operation_seconds`, `tgbotapi_memory_sampler_max_lock_wait_seconds` | | Maximum duration of such an operation and maximum waiting for the lock since the start |
| `tgbotapi_heap_audit_bytes`, `tgbotapi_memory_unattributed_exact_bytes` | | Exact size of all allocations and of the unattributed memory in the units of the estimates; only in the audit mode |
| `tgbotapi_cpu_sampling_period_seconds` | | CPU time of a thread between samples |
| `tgbotapi_cpu_unattributed_seconds_total` | | Estimated CPU time outside of bots: HTTP connections, the update queue, `ClientManager` and bots, which were closed before authorization |
| `tgbotapi_thread_cpu_seconds_total` | `scheduler` | Exact CPU time of each thread of the server |
| `tgbotapi_thread_disk_read_bytes_total`, `tgbotapi_thread_disk_written_bytes_total` | `scheduler` | Disk I/O of each thread |
| `tgbotapi_scheduler_lag_seconds` | `scheduler`, `le` | Histogram of delays of a timer, which each thread must handle 20 times per second; the delay is the time, during which the thread was busy or didn't get a CPU |
| `tgbotapi_scheduler_max_lag_seconds` | `scheduler` | Maximum delay of the timer during the last minute |

Memory metrics are exported only if memory accounting is enabled; CPU, thread and lag metrics only if CPU sampling is
enabled.

Threads (`scheduler`): `0` runs all TDLib instances, `1` their databases, `2` garbage collection and deletion of files,
`3` file uploads and downloads, `4` the HTTP server, `ClientManager` and the Bot API part of all bots, `6` and `7`
slow incoming HTTP connections and webhook connections, `9` the update queue and the webhook database, other threads
run the watchdog, DNS resolving, webhook certificates and statistics.

## CPU accounting

All bots work in shared threads, so the OS can't split their CPU time. Each thread has a timer, which interrupts the
thread after every 10 milliseconds of its CPU time (`--cpu-sampling-rate`, 100 samples per second by default); the
signal handler only increments the number of samples of the current actor context. Once a second the samples are
attributed to bots, whose actors use the contexts, so `tgbotapi_bot_cpu_seconds_total` lags behind by up to a second.
Each sample represents 10 milliseconds, so the relative error is about `1 / sqrt(number of samples)`: 10% for one
second of CPU time.

Work, which is shared by all bots, isn't attributed: receiving and parsing of HTTP requests, writing of answers, the
update queue and `ClientManager`. It is counted in `tgbotapi_cpu_unattributed_seconds_total` and can be split between
bots in proportion to their requests, as shown below. The sum of CPU time of bots and of the unattributed CPU time
is equal to the sum of `tgbotapi_thread_cpu_seconds_total` with the accuracy of the sampling, except for CPU time of
bots, which aren't authorized yet or were removed.

CPU sampling uses `SIGPROF` and works only on Linux. Use `--cpu-sampling-rate=0` to disable it, for example, to use
other profilers based on `SIGPROF`.

## Memory accounting

Memory allocation functions (`malloc`, `free`, `realloc` and others, and the C++ operators `new` and `delete`) are
replaced with wrappers, which forward calls to glibc or to an allocator loaded with `LD_PRELOAD`, for example jemalloc,
and

1. count the total size of live allocations exactly: `tgbotapi_heap_bytes`;
2. sample allocations: an allocation of `n` bytes is sampled with the probability `1 - exp(-(n + 16) / T)`, where `T` is
   `--memory-sampling-interval` (64 KB by default), and is counted with the weight `size / probability` for the bot,
   in which actor context it was made. The sum of the weights of live sampled allocations is an unbiased estimate of
   memory of the bot.

Memory belongs to the bot, which allocated it, until it is freed. New updates are counted in the part `updates`; they
aren't released by restarts, so restart limits use only the parts `client` and `tdlib`.

The server reports the standard deviation of the estimate of each bot in `tgbotapi_bot_memory_error_bytes`. It is
about `sqrt(T * memory)`: 2.6% for 100 MB and 0.6% for 2 GB with the default interval. The real value is within 3
standard deviations of the estimate in about 99.7% of cases, unless the bot has less than about 100 sampled
allocations. [VERIFICATION.md](VERIFICATION.md) proves this and describes how to check it on a running server.

With the environment variable `TELEGRAM_BOT_API_MEMORY_AUDIT=1` memory of each bot is also counted exactly at the cost
of a 16-byte header for every allocation; the mode is intended for verification of the estimates.

The accounting works only on Linux with dynamically linked glibc. `--memory-sampling-interval=0` disables it, and
`-DTELEGRAM_BOT_API_HEAP_SAMPLING=OFF` builds the server without the wrappers. Almost every allocation takes only a few
arithmetic operations without locks; once per `T` bytes of allocated memory on average a sampled allocation takes a
lock for a few microseconds. The accounting uses about 1 MB of memory plus a cache of up to 8 MB of freed sampled
allocations. In the A/B test of [VERIFICATION.md](VERIFICATION.md#no-added-latency) it adds 0.9% of CPU time per
request and no measurable latency.

## Example queries

```
# top 10 bots by requests in the last hour
topk(10, sum by (bot) (increase(tgbotapi_bot_requests_total[1h])))

# share of 429 answers by bot and reason
sum by (bot, reason) (rate(tgbotapi_bot_requests_total{code="429"}[15m]))
  / ignoring(reason) group_left sum by (bot) (rate(tgbotapi_bot_requests_total[15m]))

# 99th percentile of answer time of each bot
histogram_quantile(0.99, sum by (bot, le) (rate(tgbotapi_bot_request_duration_seconds_bucket[5m])))

# CPU usage of bots in cores, including a share of the unattributed CPU time in proportion to their requests
sum by (bot) (rate(tgbotapi_bot_cpu_seconds_total[5m]))
+ scalar(rate(tgbotapi_cpu_unattributed_seconds_total[5m]))
  * sum by (bot) (rate(tgbotapi_bot_requests_total[5m])) / scalar(sum(rate(tgbotapi_bot_requests_total[5m])))

# load of the thread of TDLib instances; 1 means that the thread is the bottleneck of the server
rate(tgbotapi_thread_cpu_seconds_total{scheduler="0"}[1m])

# top 10 bots by memory and bounds, which contain the real value with the probability of about 99.7%
topk(10, sum by (bot) (tgbotapi_bot_memory_bytes))
sum by (bot) (tgbotapi_bot_memory_bytes) - 3 * tgbotapi_bot_memory_error_bytes
sum by (bot) (tgbotapi_bot_memory_bytes) + 3 * tgbotapi_bot_memory_error_bytes

# memory kept free by the allocator, and the deviation of the estimate in standard deviations
tgbotapi_process_resident_memory_bytes - tgbotapi_heap_bytes
abs(tgbotapi_heap_estimated_bytes - tgbotapi_heap_bytes) / tgbotapi_heap_estimate_error_bytes

# threads, which didn't handle the timer for more than 100 milliseconds during the last minute
tgbotapi_scheduler_max_lag_seconds > 0.1
```

## Limitations

* Memory of a bot is a statistical estimate. Free memory, which the allocator didn't return to the OS, belongs to
  nobody: `tgbotapi_process_resident_memory_bytes - tgbotapi_heap_bytes`.
* Disk I/O isn't split by bots: databases of all bots are written by one thread through the shared page cache.
* Values of a page are read at slightly different times, so sums of related metrics may differ by recent changes.
