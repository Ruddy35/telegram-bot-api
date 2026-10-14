//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/MetricsWriter.h"

#include "td/utils/common.h"
#include "td/utils/Slice.h"

#include <array>
#include <map>
#include <utility>

namespace telegram_bot_api {

// the reason of a failed query, which isn't determined by the error code alone; used only for metrics
enum class QueryFailReason : td::int8 {
  None,
  TelegramFlood,       // flood limit of Telegram servers
  ServerFlood,         // flood limit of the Bot API server
  WebhookChangeFlood,  // too frequent setWebhook requests
  CloseFlood,          // the method close was called too early
  Closing,             // the TDLib instance of the bot is closing or logging out
  RestartQueueFull,    // too many queries were delayed during restart of the TDLib instance
  RestartTimeout,      // restart of the TDLib instance took too long
  Dropped,             // the query was destroyed without an answer
  Size
};

// Cumulative metrics of a bot. Counters are never decreased while the bot has a ClientManager entry, including
// graceful restarts of its TDLib instance. Must be used only from the client scheduler.
class BotMetrics {
 public:
  enum class InputFileType : td::int8 { Upload, LocalPath, FileId, Url, Size };

  enum class UpdateResult : td::int8 { Queued, Filtered, Failed, Size };

  enum class DownloadResult : td::int8 { Completed, Cached, TooBig, Error, Closing, Size };

  enum class WebhookResult : td::int8 { Ok, HttpError, ConnectionClosed, Size };

  enum class CpuPart : td::int8 { Client, Tdlib, Size };

  // memory is accounted by HeapSampler; Updates is memory of the update queue, which isn't freed by restarts
  enum class MemoryPart : td::int8 { Client, Tdlib, Updates, Size };

  static constexpr std::size_t MAX_UPDATE_TYPES = 64;

  BotMetrics();
  BotMetrics(const BotMetrics &) = delete;
  BotMetrics &operator=(const BotMetrics &) = delete;
  BotMetrics(BotMetrics &&) = delete;
  BotMetrics &operator=(BotMetrics &&) = delete;
  ~BotMetrics();

  // method_id is a result of Client::get_method_stat_id; query_size excludes uploaded files
  void on_response(td::int32 method_id, td::int32 http_status_code, QueryFailReason fail_reason, double duration,
                   td::int64 query_size, td::int64 answer_size);

  // the HTTP connection was closed by the client before the answer was sent
  void on_query_abandoned(td::int32 method_id);

  // size is 0 if unknown
  void on_input_file(td::int32 method_id, InputFileType type, td::int64 size);

  void on_update(td::int32 update_type, UpdateResult result);

  void on_updates_fetched(td::int64 count);

  void on_webhook_result(WebhookResult result, double response_time);

  void on_webhook_update_dropped();

  // size is the size of the downloaded file; used only for completed downloads
  void on_download_finished(DownloadResult result, td::int64 size);

  void on_tdlib_update();

  void set_storage_statistics(td::int64 files_size, td::int64 file_count, td::int64 database_size);

  void set_tdlib_object_counts(td::vector<std::pair<td::string, td::int64>> object_counts);

  // registers an actor context, CPU time and memory of which are consumed by the bot; the context must be unregistered
  // by the same owner before the BotMetrics object is destroyed
  void register_context(td::uint64 context, CpuPart part, const void *owner);

  static void unregister_context(td::uint64 context, const void *owner);

  // returns false if the context isn't registered
  static bool add_cpu_samples(td::uint64 context, td::uint64 count);

  td::int64 get_cpu_sample_count() const;

  // adds the numbers of answered requests by HTTP status code
  void add_response_counts(std::map<td::int32, td::int64> &counts) const;

  // estimated memory used by the bot; zeros if memory accounting is disabled
  struct MemoryStats {
    td::int64 bytes[static_cast<std::size_t>(MemoryPart::Size)] = {};
    td::int64 sample_count = 0;
    double variance = 0.0;                                                   // of the sum of bytes
    td::int64 exact_bytes[static_cast<std::size_t>(MemoryPart::Size)] = {};  // only in audit mode
  };
  MemoryStats get_memory_stats() const;

  // memory, which can be released by a restart of the TDLib instance
  td::int64 get_restartable_memory() const;

  static void declare_metrics(MetricsWriter &writer);

  // bot_label identifies the bot, for example, 'bot="123"'; cpu_sampling_period is 0 if CPU sampling is disabled
  void write_metrics(MetricsWriter &writer, td::Slice bot_label, double cpu_sampling_period) const;

 private:
  static constexpr std::size_t INPUT_FILE_TYPE_COUNT = static_cast<std::size_t>(InputFileType::Size);
  static constexpr std::size_t UPDATE_RESULT_COUNT = static_cast<std::size_t>(UpdateResult::Size);
  static constexpr std::size_t DOWNLOAD_RESULT_COUNT = static_cast<std::size_t>(DownloadResult::Size);
  static constexpr std::size_t WEBHOOK_RESULT_COUNT = static_cast<std::size_t>(WebhookResult::Size);
  static constexpr std::size_t CPU_PART_COUNT = static_cast<std::size_t>(CpuPart::Size);
  static constexpr std::size_t MEMORY_PART_COUNT = static_cast<std::size_t>(MemoryPart::Size);

  struct MethodStats {
    std::map<std::pair<td::int32, QueryFailReason>, td::int64> responses;  // (HTTP status code, fail reason) -> count
    double duration = 0.0;
    td::int64 query_bytes = 0;
    td::int64 answer_bytes = 0;
    td::int64 abandoned_count = 0;
    std::array<td::int64, INPUT_FILE_TYPE_COUNT> input_file_counts{};
    std::array<td::int64, INPUT_FILE_TYPE_COUNT> input_file_bytes{};
  };
  std::map<td::int32, MethodStats> methods_;  // method_id -> statistics

  // durations of answered requests except getUpdates
  static constexpr double DURATION_BUCKETS[] = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 30.0, 60.0};
  std::array<td::uint64, sizeof(DURATION_BUCKETS) / sizeof(DURATION_BUCKETS[0]) + 1> duration_counts_{};
  double duration_sum_ = 0.0;

  std::array<std::array<td::int64, UPDATE_RESULT_COUNT>, MAX_UPDATE_TYPES> updates_{};
  td::int64 fetched_updates_ = 0;

  std::array<td::int64, WEBHOOK_RESULT_COUNT> webhook_results_{};
  double webhook_response_time_ = 0.0;
  td::int64 webhook_dropped_updates_ = 0;

  std::array<td::int64, DOWNLOAD_RESULT_COUNT> downloads_{};
  td::int64 downloaded_bytes_ = 0;

  td::int64 tdlib_updates_ = 0;

  std::array<td::int64, CPU_PART_COUNT> cpu_samples_{};

  td::int32 heap_owner_ = 0;  // owner in HeapSampler or 0

  bool has_storage_statistics_ = false;
  td::int64 storage_files_size_ = 0;
  td::int64 storage_file_count_ = 0;
  td::int64 storage_database_size_ = 0;

  // numbers of objects in memory of the current TDLib instance by type
  td::vector<std::pair<td::string, td::int64>> tdlib_object_counts_;
};

}  // namespace telegram_bot_api
