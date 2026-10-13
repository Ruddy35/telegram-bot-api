//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/utils/common.h"
#include "td/utils/FlatHashMap.h"
#include "td/utils/Slice.h"

#include <array>
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

  enum class DownloadResult : td::int8 { Completed, Cached, TooBig, Error, Closing, Aborted, Size };

  enum class WebhookResult : td::int8 { Ok, HttpError, ConnectionError, Dropped, Size };

  static constexpr std::size_t MAX_UPDATE_TYPES = 64;
  static constexpr std::size_t DURATION_BUCKET_COUNT = 13;
  static constexpr double DURATION_BUCKETS[DURATION_BUCKET_COUNT] = {0.005, 0.01, 0.025, 0.05, 0.1,  0.25, 0.5,
                                                                     1.0,   2.5,  5.0,   10.0, 30.0, 60.0};

  void on_response(td::int32 method_id, td::int32 http_status_code, QueryFailReason fail_reason, double duration,
                   td::int64 query_size, td::int64 answer_size, td::int64 file_count, td::int64 files_size);

  void on_query_abandoned(td::int32 method_id);

  void on_input_file(td::int32 method_id, InputFileType type, td::int64 size);

  void on_update(td::int32 update_type, UpdateResult result);

  void on_updates_fetched(td::int64 count);

  void on_webhook_result(WebhookResult result, double response_time);

  void on_webhook_connection_error();

  void on_download_started();

  void on_download_finished(DownloadResult result, td::int64 size);

  void on_tdlib_update();

  void set_storage_statistics(td::int64 files_size, td::int64 file_count, td::int64 database_size, td::int64 log_size);

  struct MethodTotals {
    td::int64 count = 0;
    double duration = 0.0;
    td::int64 query_bytes = 0;
    td::int64 answer_bytes = 0;
    td::int64 file_count = 0;
    td::int64 file_bytes = 0;
  };

  // key: method_id + 1, http_status_code, fail_reason
  static std::pair<td::int32, std::pair<td::int32, QueryFailReason>> decode_request_key(td::uint64 key) {
    return {static_cast<td::int32>(key >> 24) - 1,
            {static_cast<td::int32>((key >> 8) & 0xFFFF), static_cast<QueryFailReason>(key & 0xFF)}};
  }

  static std::pair<td::int32, InputFileType> decode_input_file_key(td::uint64 key) {
    return {static_cast<td::int32>(key >> 8) - 1, static_cast<InputFileType>((key & 0xFF) - 1)};
  }

  static td::Slice get_fail_reason_name(QueryFailReason reason);

  static td::Slice get_input_file_type_name(InputFileType type);

  static td::Slice get_update_result_name(UpdateResult result);

  static td::Slice get_download_result_name(DownloadResult result);

  static td::Slice get_webhook_result_name(WebhookResult result);

  td::FlatHashMap<td::uint64, td::int64> requests_;
  td::FlatHashMap<td::int32, MethodTotals> method_totals_;  // method_id + 1 -> totals
  std::array<td::int64, DURATION_BUCKET_COUNT + 1> duration_buckets_{};
  double duration_sum_ = 0.0;
  td::int64 duration_count_ = 0;

  td::FlatHashMap<td::int32, td::int64> abandoned_queries_;  // method_id + 1 -> count

  td::FlatHashMap<td::uint64, std::pair<td::int64, td::int64>> input_files_;  // -> (count, bytes)

  std::array<std::array<td::int64, static_cast<std::size_t>(UpdateResult::Size)>, MAX_UPDATE_TYPES> updates_{};
  td::int64 fetched_updates_ = 0;

  std::array<td::int64, static_cast<std::size_t>(WebhookResult::Size)> webhook_results_{};
  double webhook_response_time_ = 0.0;
  td::int64 webhook_connection_errors_ = 0;

  td::int64 started_downloads_ = 0;
  std::array<td::int64, static_cast<std::size_t>(DownloadResult::Size)> finished_downloads_{};
  td::int64 downloaded_bytes_ = 0;
  td::int64 partially_downloaded_bytes_ = 0;

  td::int64 tdlib_updates_ = 0;

  bool has_storage_statistics_ = false;
  td::int64 storage_files_size_ = 0;
  td::int64 storage_file_count_ = 0;
  td::int64 storage_database_size_ = 0;
  td::int64 storage_log_size_ = 0;
};

// Builds metrics in the Prometheus text exposition format, grouping samples by metric families
class MetricsWriter {
 public:
  void add(td::Slice name, td::Slice type, td::Slice help, td::Slice labels, td::int64 value);

  void add(td::Slice name, td::Slice type, td::Slice help, td::Slice labels, double value);

  // adds a sample with the name <family name><suffix>, for example, a histogram bucket
  void add_sample(td::Slice name, td::Slice suffix, td::Slice type, td::Slice help, td::Slice labels, td::int64 value);

  void add_sample(td::Slice name, td::Slice suffix, td::Slice type, td::Slice help, td::Slice labels, double value);

  td::string finish();

  static td::string escape_label_value(td::Slice value);

 private:
  struct Family {
    td::string name;
    td::string type;
    td::string help;
    td::string samples;
  };
  td::vector<Family> families_;
  td::FlatHashMap<td::string, std::size_t> family_indexes_;

  td::string &get_samples(td::Slice name, td::Slice type, td::Slice help);

  void add_sample_value(td::Slice name, td::Slice suffix, td::Slice type, td::Slice help, td::Slice labels,
                        td::Slice value);
};

}  // namespace telegram_bot_api
