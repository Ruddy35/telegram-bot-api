//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/BotMetrics.h"

#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/SliceBuilder.h"

#include <cstdio>

namespace telegram_bot_api {

void BotMetrics::on_response(td::int32 method_id, td::int32 http_status_code, QueryFailReason fail_reason,
                             double duration, td::int64 query_size, td::int64 answer_size, td::int64 file_count,
                             td::int64 files_size) {
  CHECK(method_id >= 0);
  if (http_status_code <= 0 || http_status_code > 0xFFFF) {
    http_status_code = 500;
  }
  auto key = (static_cast<td::uint64>(method_id + 1) << 24) | (static_cast<td::uint64>(http_status_code) << 8) |
             static_cast<td::uint64>(fail_reason);
  requests_[key]++;

  auto &totals = method_totals_[method_id + 1];
  totals.count++;
  totals.duration += duration;
  totals.query_bytes += query_size;
  totals.answer_bytes += answer_size;
  totals.file_count += file_count;
  totals.file_bytes += files_size;

  std::size_t bucket = 0;
  while (bucket < DURATION_BUCKET_COUNT && duration > DURATION_BUCKETS[bucket]) {
    bucket++;
  }
  duration_buckets_[bucket]++;
  duration_sum_ += duration;
  duration_count_++;
}

void BotMetrics::on_query_abandoned(td::int32 method_id) {
  CHECK(method_id >= 0);
  abandoned_queries_[method_id + 1]++;
}

void BotMetrics::on_input_file(td::int32 method_id, InputFileType type, td::int64 size) {
  CHECK(method_id >= 0);
  auto key = (static_cast<td::uint64>(method_id + 1) << 8) | static_cast<td::uint64>(static_cast<td::int32>(type) + 1);
  auto &value = input_files_[key];
  value.first++;
  value.second += td::max(size, static_cast<td::int64>(0));
}

void BotMetrics::on_update(td::int32 update_type, UpdateResult result) {
  if (update_type < 0 || static_cast<std::size_t>(update_type) >= MAX_UPDATE_TYPES) {
    LOG(ERROR) << "Receive update of unexpected type " << update_type;
    return;
  }
  updates_[update_type][static_cast<std::size_t>(result)]++;
}

void BotMetrics::on_updates_fetched(td::int64 count) {
  fetched_updates_ += count;
}

void BotMetrics::on_webhook_result(WebhookResult result, double response_time) {
  webhook_results_[static_cast<std::size_t>(result)]++;
  if (response_time > 0) {
    webhook_response_time_ += response_time;
  }
}

void BotMetrics::on_webhook_connection_error() {
  webhook_connection_errors_++;
}

void BotMetrics::on_download_started() {
  started_downloads_++;
}

void BotMetrics::on_download_finished(DownloadResult result, td::int64 size) {
  finished_downloads_[static_cast<std::size_t>(result)]++;
  if (size <= 0) {
    return;
  }
  if (result == DownloadResult::Completed) {
    downloaded_bytes_ += size;
  } else if (result != DownloadResult::Cached) {
    partially_downloaded_bytes_ += size;
  }
}

void BotMetrics::on_tdlib_update() {
  tdlib_updates_++;
}

void BotMetrics::set_storage_statistics(td::int64 files_size, td::int64 file_count, td::int64 database_size,
                                        td::int64 log_size) {
  has_storage_statistics_ = true;
  storage_files_size_ = files_size;
  storage_file_count_ = file_count;
  storage_database_size_ = database_size;
  storage_log_size_ = log_size;
}

td::Slice BotMetrics::get_fail_reason_name(QueryFailReason reason) {
  switch (reason) {
    case QueryFailReason::None:
      return td::Slice("none");
    case QueryFailReason::TelegramFlood:
      return td::Slice("telegram_flood");
    case QueryFailReason::ServerFlood:
      return td::Slice("server_flood");
    case QueryFailReason::WebhookChangeFlood:
      return td::Slice("set_webhook_flood");
    case QueryFailReason::CloseFlood:
      return td::Slice("close_flood");
    case QueryFailReason::Closing:
      return td::Slice("closing");
    case QueryFailReason::RestartQueueFull:
      return td::Slice("restart_queue_full");
    case QueryFailReason::RestartTimeout:
      return td::Slice("restart_timeout");
    case QueryFailReason::Dropped:
      return td::Slice("dropped");
    default:
      return td::Slice("unknown");
  }
}

td::Slice BotMetrics::get_input_file_type_name(InputFileType type) {
  switch (type) {
    case InputFileType::Upload:
      return td::Slice("upload");
    case InputFileType::LocalPath:
      return td::Slice("local_path");
    case InputFileType::FileId:
      return td::Slice("file_id");
    case InputFileType::Url:
      return td::Slice("url");
    default:
      return td::Slice("unknown");
  }
}

td::Slice BotMetrics::get_update_result_name(UpdateResult result) {
  switch (result) {
    case UpdateResult::Queued:
      return td::Slice("queued");
    case UpdateResult::Filtered:
      return td::Slice("filtered");
    case UpdateResult::Failed:
      return td::Slice("failed");
    default:
      return td::Slice("unknown");
  }
}

td::Slice BotMetrics::get_download_result_name(DownloadResult result) {
  switch (result) {
    case DownloadResult::Completed:
      return td::Slice("completed");
    case DownloadResult::Cached:
      return td::Slice("cached");
    case DownloadResult::TooBig:
      return td::Slice("too_big");
    case DownloadResult::Error:
      return td::Slice("error");
    case DownloadResult::Closing:
      return td::Slice("closing");
    case DownloadResult::Aborted:
      return td::Slice("aborted");
    default:
      return td::Slice("unknown");
  }
}

td::Slice BotMetrics::get_webhook_result_name(WebhookResult result) {
  switch (result) {
    case WebhookResult::Ok:
      return td::Slice("ok");
    case WebhookResult::HttpError:
      return td::Slice("http_error");
    case WebhookResult::ConnectionError:
      return td::Slice("connection_error");
    case WebhookResult::Dropped:
      return td::Slice("dropped");
    default:
      return td::Slice("unknown");
  }
}

td::string &MetricsWriter::get_samples(td::Slice name, td::Slice type, td::Slice help) {
  auto it = family_indexes_.find(name.str());
  if (it == family_indexes_.end()) {
    family_indexes_.emplace(name.str(), families_.size());
    families_.push_back(Family{name.str(), type.str(), help.str(), td::string()});
    return families_.back().samples;
  }
  return families_[it->second].samples;
}

void MetricsWriter::add_sample_value(td::Slice name, td::Slice suffix, td::Slice type, td::Slice help, td::Slice labels,
                                     td::Slice value) {
  auto &samples = get_samples(name, type, help);
  samples.append(name.begin(), name.size());
  samples.append(suffix.begin(), suffix.size());
  if (!labels.empty()) {
    samples += '{';
    samples.append(labels.begin(), labels.size());
    samples += '}';
  }
  samples += ' ';
  samples.append(value.begin(), value.size());
  samples += '\n';
}

void MetricsWriter::add_sample(td::Slice name, td::Slice suffix, td::Slice type, td::Slice help, td::Slice labels,
                               td::int64 value) {
  add_sample_value(name, suffix, type, help, labels, td::to_string(value));
}

void MetricsWriter::add_sample(td::Slice name, td::Slice suffix, td::Slice type, td::Slice help, td::Slice labels,
                               double value) {
  char buf[64];
  auto len = std::snprintf(buf, sizeof(buf), "%.6f", value);
  CHECK(len > 0 && static_cast<std::size_t>(len) < sizeof(buf));
  add_sample_value(name, suffix, type, help, labels, td::Slice(buf, static_cast<std::size_t>(len)));
}

void MetricsWriter::add(td::Slice name, td::Slice type, td::Slice help, td::Slice labels, td::int64 value) {
  add_sample(name, td::Slice(), type, help, labels, value);
}

void MetricsWriter::add(td::Slice name, td::Slice type, td::Slice help, td::Slice labels, double value) {
  add_sample(name, td::Slice(), type, help, labels, value);
}

td::string MetricsWriter::finish() {
  td::string result;
  for (auto &family : families_) {
    result += "# HELP ";
    result += family.name;
    result += ' ';
    result += family.help;
    result += "\n# TYPE ";
    result += family.name;
    result += ' ';
    result += family.type;
    result += '\n';
    result += family.samples;
  }
  families_.clear();
  family_indexes_.clear();
  return result;
}

td::string MetricsWriter::escape_label_value(td::Slice value) {
  td::string result;
  result.reserve(value.size());
  for (auto c : value) {
    switch (c) {
      case '\\':
        result += "\\\\";
        break;
      case '"':
        result += "\\\"";
        break;
      case '\n':
        result += "\\n";
        break;
      default:
        result += c;
    }
  }
  return result;
}

}  // namespace telegram_bot_api
