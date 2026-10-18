//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/BotMetrics.h"

#include "telegram-bot-api/Client.h"
#include "telegram-bot-api/HeapSampler.h"

#include "td/utils/FlatHashMap.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/SliceBuilder.h"

#include <cmath>

namespace telegram_bot_api {

namespace {
// label values of enumerations in the order of their values
const char *const FAIL_REASON_NAMES[] = {"none",        "telegram_flood", "server_flood",       "set_webhook_flood",
                                         "close_flood", "closing",        "restart_queue_full", "restart_timeout",
                                         "dropped"};
const char *const INPUT_FILE_TYPE_NAMES[] = {"upload", "local_path", "file_id", "url"};
const char *const UPDATE_RESULT_NAMES[] = {"queued", "filtered", "failed"};
const char *const DOWNLOAD_RESULT_NAMES[] = {"completed", "cached", "too_big", "error", "closing"};
const char *const WEBHOOK_RESULT_NAMES[] = {"ok", "http_error", "connection_closed"};
const char *const PART_NAMES[] = {"client", "tdlib", "updates"};

template <class Enum, std::size_t N>
constexpr bool has_all_names(const char *const (&)[N]) {
  return static_cast<std::size_t>(Enum::Size) == N;
}
static_assert(has_all_names<QueryFailReason>(FAIL_REASON_NAMES), "");
static_assert(has_all_names<BotMetrics::InputFileType>(INPUT_FILE_TYPE_NAMES), "");
static_assert(has_all_names<BotMetrics::UpdateResult>(UPDATE_RESULT_NAMES), "");
static_assert(has_all_names<BotMetrics::DownloadResult>(DOWNLOAD_RESULT_NAMES), "");
static_assert(has_all_names<BotMetrics::WebhookResult>(WEBHOOK_RESULT_NAMES), "");
static_assert(has_all_names<BotMetrics::MemoryPart>(PART_NAMES), "");
static_assert(static_cast<int>(BotMetrics::MemoryPart::Size) == HeapSampler::PART_COUNT, "");
static_assert(static_cast<int>(BotMetrics::CpuPart::Client) == static_cast<int>(BotMetrics::MemoryPart::Client), "");
static_assert(static_cast<int>(BotMetrics::CpuPart::Tdlib) == static_cast<int>(BotMetrics::MemoryPart::Tdlib), "");

struct CpuContextInfo {
  BotMetrics *metrics = nullptr;
  BotMetrics::CpuPart part = BotMetrics::CpuPart::Client;
  const void *owner = nullptr;
};

// actor context pointer -> the bot consuming its CPU time; must be used only from the client scheduler
td::FlatHashMap<td::uint64, CpuContextInfo> &get_cpu_contexts() {
  static auto *cpu_contexts = new td::FlatHashMap<td::uint64, CpuContextInfo>();
  return *cpu_contexts;
}
}  // namespace

BotMetrics::BotMetrics() : heap_owner_(HeapSampler::create_owner()) {
}

BotMetrics::~BotMetrics() {
  HeapSampler::destroy_owner(heap_owner_);
}

void BotMetrics::on_response(td::int32 method_id, td::int32 http_status_code, QueryFailReason fail_reason,
                             double duration, td::int64 query_size, td::int64 answer_size) {
  auto &method = methods_[method_id];
  method.responses[{http_status_code, fail_reason}]++;
  method.duration += duration;
  method.query_bytes += query_size;
  method.answer_bytes += answer_size;

  if (Client::get_method_stat_name(method_id) == "getupdates") {
    return;  // long polling would hide durations of other requests
  }
  std::size_t bucket = 0;
  while (bucket + 1 < duration_counts_.size() && duration > DURATION_BUCKETS[bucket]) {
    bucket++;
  }
  duration_counts_[bucket]++;
  duration_sum_ += duration;
}

void BotMetrics::on_query_abandoned(td::int32 method_id) {
  methods_[method_id].abandoned_count++;
}

void BotMetrics::on_input_file(td::int32 method_id, InputFileType type, td::int64 size) {
  auto &method = methods_[method_id];
  method.input_file_counts[static_cast<std::size_t>(type)]++;
  method.input_file_bytes[static_cast<std::size_t>(type)] += td::max(size, static_cast<td::int64>(0));
}

void BotMetrics::on_update(td::int32 update_type, UpdateResult result) {
  CHECK(0 <= update_type && static_cast<std::size_t>(update_type) < MAX_UPDATE_TYPES);
  updates_[update_type][static_cast<std::size_t>(result)]++;
}

void BotMetrics::on_updates_fetched(td::int64 count) {
  fetched_updates_ += count;
}

void BotMetrics::on_webhook_result(WebhookResult result, double response_time) {
  webhook_results_[static_cast<std::size_t>(result)]++;
  webhook_response_time_ += td::max(response_time, 0.0);
}

void BotMetrics::on_webhook_update_dropped() {
  webhook_dropped_updates_++;
}

void BotMetrics::on_download_finished(DownloadResult result, td::int64 size) {
  downloads_[static_cast<std::size_t>(result)]++;
  if (result == DownloadResult::Completed) {
    downloaded_bytes_ += td::max(size, static_cast<td::int64>(0));
  }
}

void BotMetrics::on_tdlib_update() {
  tdlib_updates_++;
}

void BotMetrics::set_storage_statistics(td::int64 files_size, td::int64 file_count, td::int64 database_size) {
  has_storage_statistics_ = true;
  storage_files_size_ = files_size;
  storage_file_count_ = file_count;
  storage_database_size_ = database_size;
}

void BotMetrics::set_tdlib_object_counts(td::vector<std::pair<td::string, td::int64>> object_counts) {
  tdlib_object_counts_ = std::move(object_counts);
}

void BotMetrics::register_context(td::uint64 context, CpuPart part, const void *owner) {
  CHECK(context != 0);
  CHECK(owner != nullptr);
  // memory of a destroyed context can be reused for a new context, so the last registration wins
  get_cpu_contexts()[context] = CpuContextInfo{this, part, owner};
  HeapSampler::set_context_owner(context, heap_owner_, static_cast<td::int32>(part));
}

void BotMetrics::unregister_context(td::uint64 context, const void *owner) {
  auto &cpu_contexts = get_cpu_contexts();
  auto it = cpu_contexts.find(context);
  if (it != cpu_contexts.end() && it->second.owner == owner) {
    HeapSampler::remove_context(context, it->second.metrics->heap_owner_);
    cpu_contexts.erase(it);
  }
}

bool BotMetrics::add_cpu_samples(td::uint64 context, td::uint64 count) {
  if (context == 0) {
    return false;
  }
  auto &cpu_contexts = get_cpu_contexts();
  auto it = cpu_contexts.find(context);
  if (it == cpu_contexts.end()) {
    return false;
  }
  it->second.metrics->cpu_samples_[static_cast<std::size_t>(it->second.part)] += static_cast<td::int64>(count);
  return true;
}

td::int64 BotMetrics::get_cpu_sample_count() const {
  td::int64 result = 0;
  for (auto count : cpu_samples_) {
    result += count;
  }
  return result;
}

void BotMetrics::add_response_counts(std::map<td::int32, td::int64> &counts) const {
  for (auto &method : methods_) {
    for (auto &response : method.second.responses) {
      counts[response.first.first] += response.second;
    }
  }
}

BotMetrics::MemoryStats BotMetrics::get_memory_stats() const {
  MemoryStats result;
  if (heap_owner_ == 0 || HeapSampler::get_sampling_interval() <= 0) {
    return result;
  }
  auto stats = HeapSampler::get_owner_stats(heap_owner_);
  for (std::size_t part = 0; part < MEMORY_PART_COUNT; part++) {
    result.bytes[part] = stats.bytes[part];
    result.exact_bytes[part] = stats.exact_bytes[part];
  }
  result.sample_count = stats.sample_count;
  result.variance = stats.variance;
  return result;
}

td::int64 BotMetrics::get_restartable_memory() const {
  auto stats = get_memory_stats();
  return stats.bytes[static_cast<std::size_t>(MemoryPart::Client)] +
         stats.bytes[static_cast<std::size_t>(MemoryPart::Tdlib)];
}

void BotMetrics::declare_metrics(MetricsWriter &writer) {
  using Type = MetricsWriter::Type;
  // requests
  writer.declare("tgbotapi_bot_requests_total", Type::Counter,
                 "Number of answered requests by method, HTTP status code and failure reason");
  writer.declare("tgbotapi_bot_method_duration_seconds_total", Type::Counter,
                 "Total time between receiving of requests and sending of answers");
  writer.declare("tgbotapi_bot_request_duration_seconds", Type::Histogram,
                 "Time between receiving of requests except getUpdates and sending of answers");
  writer.declare("tgbotapi_bot_request_bytes_total", Type::Counter,
                 "Total size of requests including headers, excluding uploaded files");
  writer.declare("tgbotapi_bot_response_bytes_total", Type::Counter, "Total size of answers");
  writer.declare("tgbotapi_bot_abandoned_requests_total", Type::Counter,
                 "Number of requests, connection of which was closed by the client before an answer was sent");
  writer.declare("tgbotapi_bot_input_files_total", Type::Counter,
                 "Number of files passed to methods by type: upload, local_path, file_id or url");
  writer.declare("tgbotapi_bot_input_file_bytes_total", Type::Counter,
                 "Total size of uploaded and local files passed to methods");

  // updates
  writer.declare("tgbotapi_bot_updates_total", Type::Counter,
                 "Number of updates by type and result: queued, filtered by allowed_updates or failed");
  writer.declare("tgbotapi_bot_fetched_updates_total", Type::Counter, "Number of updates returned by getUpdates");
  writer.declare("tgbotapi_bot_webhook_deliveries_total", Type::Counter,
                 "Number of attempts to send an update to the webhook by result: ok, http_error or connection_closed");
  writer.declare("tgbotapi_bot_webhook_response_seconds_total", Type::Counter,
                 "Total time of waiting for webhook responses");
  writer.declare("tgbotapi_bot_webhook_dropped_updates_total", Type::Counter,
                 "Number of updates, which were dropped after failed attempts to send them to the webhook");

  // downloads
  writer.declare("tgbotapi_bot_file_downloads_total", Type::Counter,
                 "Number of finished file downloads requested by getFile by result: completed, cached, too_big, "
                 "error or closing");
  writer.declare("tgbotapi_bot_downloaded_bytes_total", Type::Counter, "Total size of completely downloaded files");

  // TDLib instance
  writer.declare("tgbotapi_bot_tdlib_updates_total", Type::Counter, "Number of updates received from TDLib");
  writer.declare("tgbotapi_bot_storage_bytes", Type::Gauge,
                 "Size of files and databases of the bot on the disk; updated once a minute");
  writer.declare("tgbotapi_bot_storage_files", Type::Gauge,
                 "Number of files of the bot on the disk; updated once a minute");
  writer.declare("tgbotapi_bot_tdlib_objects", Type::Gauge,
                 "Number of objects of different types in memory of the TDLib instance, which are never deleted or "
                 "deleted only after some time; updated once a minute");

  // resources
  writer.declare("tgbotapi_bot_cpu_seconds_total", Type::Counter,
                 "Estimated CPU time consumed by the bot: client for the Bot API part, tdlib for the TDLib instance; "
                 "updated once a second");
  writer.declare("tgbotapi_bot_memory_bytes", Type::Gauge,
                 "Estimated size of memory used by the bot: client for the Bot API part, tdlib for the TDLib instance, "
                 "updates for pending updates");
  writer.declare("tgbotapi_bot_memory_error_bytes", Type::Gauge,
                 "Estimated standard deviation of the sum of tgbotapi_bot_memory_bytes");
  writer.declare("tgbotapi_bot_memory_samples", Type::Gauge, "Number of live sampled memory allocations of the bot");
  writer.declare("tgbotapi_bot_memory_exact_bytes", Type::Gauge,
                 "Exact size of memory used by the bot; only in the audit mode");
}

void BotMetrics::write_metrics(MetricsWriter &writer, td::Slice bot_label, double cpu_sampling_period) const {
  // requests
  for (auto &it : methods_) {
    td::string labels = PSTRING() << bot_label << ",method=\"" << Client::get_method_stat_name(it.first) << '"';
    const auto &method = it.second;
    for (auto &response : method.responses) {
      writer.add("tgbotapi_bot_requests_total",
                 PSLICE() << labels << ",code=\"" << response.first.first << "\",reason=\""
                          << FAIL_REASON_NAMES[static_cast<std::size_t>(response.first.second)] << '"',
                 response.second);
    }
    if (!method.responses.empty()) {
      writer.add("tgbotapi_bot_method_duration_seconds_total", labels, method.duration);
      writer.add("tgbotapi_bot_request_bytes_total", labels, method.query_bytes);
      writer.add("tgbotapi_bot_response_bytes_total", labels, method.answer_bytes);
    }
    if (method.abandoned_count != 0) {
      writer.add("tgbotapi_bot_abandoned_requests_total", labels, method.abandoned_count);
    }
    for (std::size_t type = 0; type < INPUT_FILE_TYPE_COUNT; type++) {
      if (method.input_file_counts[type] != 0) {
        td::string file_labels = PSTRING() << labels << ",type=\"" << INPUT_FILE_TYPE_NAMES[type] << '"';
        writer.add("tgbotapi_bot_input_files_total", file_labels, method.input_file_counts[type]);
        writer.add("tgbotapi_bot_input_file_bytes_total", file_labels, method.input_file_bytes[type]);
      }
    }
  }
  writer.add_histogram("tgbotapi_bot_request_duration_seconds", bot_label, DURATION_BUCKETS, duration_counts_,
                       duration_sum_);

  // updates
  for (td::int32 type = 0; type < Client::get_update_type_count(); type++) {
    for (std::size_t result = 0; result < UPDATE_RESULT_COUNT; result++) {
      if (updates_[type][result] != 0) {
        writer.add("tgbotapi_bot_updates_total",
                   PSLICE() << bot_label << ",type=\"" << Client::get_update_type_stat_name(type) << "\",result=\""
                            << UPDATE_RESULT_NAMES[result] << '"',
                   updates_[type][result]);
      }
    }
  }
  writer.add("tgbotapi_bot_fetched_updates_total", bot_label, fetched_updates_);
  for (std::size_t result = 0; result < WEBHOOK_RESULT_COUNT; result++) {
    writer.add("tgbotapi_bot_webhook_deliveries_total",
               PSLICE() << bot_label << ",result=\"" << WEBHOOK_RESULT_NAMES[result] << '"', webhook_results_[result]);
  }
  writer.add("tgbotapi_bot_webhook_response_seconds_total", bot_label, webhook_response_time_);
  writer.add("tgbotapi_bot_webhook_dropped_updates_total", bot_label, webhook_dropped_updates_);

  // downloads
  for (std::size_t result = 0; result < DOWNLOAD_RESULT_COUNT; result++) {
    writer.add("tgbotapi_bot_file_downloads_total",
               PSLICE() << bot_label << ",result=\"" << DOWNLOAD_RESULT_NAMES[result] << '"', downloads_[result]);
  }
  writer.add("tgbotapi_bot_downloaded_bytes_total", bot_label, downloaded_bytes_);

  // TDLib instance
  writer.add("tgbotapi_bot_tdlib_updates_total", bot_label, tdlib_updates_);
  if (has_storage_statistics_) {
    writer.add("tgbotapi_bot_storage_bytes", PSLICE() << bot_label << ",type=\"files\"", storage_files_size_);
    writer.add("tgbotapi_bot_storage_bytes", PSLICE() << bot_label << ",type=\"database\"", storage_database_size_);
    writer.add("tgbotapi_bot_storage_files", bot_label, storage_file_count_);
  }
  for (auto &object_count : tdlib_object_counts_) {
    writer.add("tgbotapi_bot_tdlib_objects",
               PSLICE() << bot_label << ",type=\"" << MetricsWriter::escape_label_value(object_count.first) << '"',
               object_count.second);
  }

  // resources
  if (cpu_sampling_period > 0.0) {
    for (std::size_t part = 0; part < CPU_PART_COUNT; part++) {
      writer.add("tgbotapi_bot_cpu_seconds_total", PSLICE() << bot_label << ",part=\"" << PART_NAMES[part] << '"',
                 static_cast<double>(cpu_samples_[part]) * cpu_sampling_period);
    }
  }
  if (heap_owner_ != 0 && HeapSampler::get_sampling_interval() > 0) {
    auto memory = get_memory_stats();
    for (std::size_t part = 0; part < MEMORY_PART_COUNT; part++) {
      writer.add("tgbotapi_bot_memory_bytes", PSLICE() << bot_label << ",part=\"" << PART_NAMES[part] << '"',
                 memory.bytes[part]);
    }
    writer.add("tgbotapi_bot_memory_error_bytes", bot_label, std::sqrt(td::max(memory.variance, 0.0)));
    writer.add("tgbotapi_bot_memory_samples", bot_label, memory.sample_count);
    if (HeapSampler::is_audit_enabled()) {
      for (std::size_t part = 0; part < MEMORY_PART_COUNT; part++) {
        writer.add("tgbotapi_bot_memory_exact_bytes", PSLICE() << bot_label << ",part=\"" << PART_NAMES[part] << '"',
                   memory.exact_bytes[part]);
      }
    }
  }
}

}  // namespace telegram_bot_api
