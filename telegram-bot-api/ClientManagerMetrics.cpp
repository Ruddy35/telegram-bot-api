//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Metrics of ClientManager in the Prometheus text exposition format; see METRICS.md
//
#include "telegram-bot-api/ClientManager.h"

#include "telegram-bot-api/ClientParameters.h"
#include "telegram-bot-api/HeapSampler.h"
#include "telegram-bot-api/MetricsWriter.h"
#include "telegram-bot-api/WebhookActor.h"

#include "td/telegram/ClientActor.h"

#include "td/utils/buffer.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/Stat.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Time.h"

#include <array>
#include <cmath>
#include <map>

#if TD_LINUX
#include <unistd.h>
#endif

namespace telegram_bot_api {

namespace {
const char *const REJECT_REASON_NAMES[] = {"closing",    "invalid_token",  "misdirected_token",
                                           "closed_bot", "ip_flood_limit", "global_flood_limit"};

td::string get_scheduler_label(std::size_t scheduler_id) {
  return PSTRING() << "scheduler=\"" << scheduler_id << '"';
}
}  // namespace

void ClientManager::on_query_abandoned(td::string token, td::string method) {
  auto id_it = token_to_id_.find(token);
  if (id_it == token_to_id_.end()) {
    return;
  }
  auto *client_info = clients_.get(id_it->second);
  CHECK(client_info != nullptr);
  client_info->metrics_->on_query_abandoned(Client::get_method_stat_id(method));
}

void ClientManager::start_cpu_sampling() {
  if (parameters_->cpu_sampling_rate_ <= 0) {
    return;
  }
  auto status = CpuSampler::init();
  if (status.is_error()) {
    LOG(ERROR) << "Failed to start CPU sampling: " << status;
    return;
  }
  cpu_sampling_period_ = 1.0 / parameters_->cpu_sampling_rate_;
  auto thread_count = static_cast<std::size_t>(SharedData::get_thread_count());
  threads_.resize(thread_count);
  for (std::size_t scheduler_id = 0; scheduler_id < thread_count; scheduler_id++) {
    cpu_samplers_.push_back(td::create_actor_on_scheduler<CpuSampler>(
        "CpuSampler", static_cast<td::int32>(scheduler_id), cpu_sampling_period_,
        [actor_id = actor_id(this)](CpuSampler::Samples samples) {
          send_closure(actor_id, &ClientManager::on_cpu_samples, std::move(samples));
        }));
  }
}

void ClientManager::on_cpu_samples(CpuSampler::Samples samples) {
  for (auto &context : samples.contexts_) {
    if (!BotMetrics::add_cpu_samples(context.first, context.second)) {
      unattributed_cpu_samples_ += static_cast<td::int64>(context.second);
    }
  }
  CHECK(0 <= samples.scheduler_id && static_cast<std::size_t>(samples.scheduler_id) < threads_.size());
  auto &thread = threads_[samples.scheduler_id];
  thread.cpu_time = samples.thread_cpu_time;
  thread.thread_id = samples.thread_id;
  for (std::size_t i = 0; i < CpuSampler::LAG_BUCKET_COUNT; i++) {
    thread.lag_counts[i] += samples.lag_counts[i];
  }
  thread.lag_sum += samples.lag_sum;
  thread.recent_max_lags[thread.recent_max_lag_pos] = samples.max_lag;
  thread.recent_max_lag_pos = (thread.recent_max_lag_pos + 1) % thread.recent_max_lags.size();
}

void ClientManager::declare_metrics(MetricsWriter &writer) {
  using Type = MetricsWriter::Type;
  // process
  writer.declare("tgbotapi_uptime_seconds", Type::Gauge, "Time since the server start");
  writer.declare("tgbotapi_process_resident_memory_bytes", Type::Gauge, "Resident memory size of the process");
  writer.declare("tgbotapi_process_virtual_memory_bytes", Type::Gauge, "Virtual memory size of the process");
  writer.declare("tgbotapi_process_cpu_seconds_total", Type::Counter, "CPU time used by the process by mode");
  writer.declare("tgbotapi_process_disk_read_bytes_total", Type::Counter, "Bytes read from the disk by the process");
  writer.declare("tgbotapi_process_disk_written_bytes_total", Type::Counter,
                 "Bytes written to the disk by the process");
  writer.declare("tgbotapi_buffer_memory_bytes", Type::Gauge, "Memory used by network buffers");

  // memory accounting
  writer.declare("tgbotapi_heap_bytes", Type::Gauge, "Exact size of all live memory allocations of the process");
  writer.declare("tgbotapi_heap_estimated_bytes", Type::Gauge,
                 "Estimated size of all live memory allocations, which must be close to tgbotapi_heap_bytes");
  writer.declare("tgbotapi_heap_estimate_error_bytes", Type::Gauge,
                 "Estimated standard deviation of tgbotapi_heap_estimated_bytes");
  writer.declare("tgbotapi_heap_audit_bytes", Type::Gauge,
                 "Exact size of all live memory allocations in the units of the estimates; only in the audit mode");
  writer.declare("tgbotapi_memory_unattributed_bytes", Type::Gauge,
                 "Estimated size of memory used outside of the bots listed on the page, for example, by HTTP "
                 "connections");
  writer.declare("tgbotapi_memory_unattributed_exact_bytes", Type::Gauge,
                 "Exact size of memory used outside of the bots listed on the page; only in the audit mode");
  writer.declare("tgbotapi_memory_sampling_interval_bytes", Type::Gauge,
                 "Mean size of allocated memory between samples");
  writer.declare("tgbotapi_memory_samples", Type::Gauge, "Number of live sampled memory allocations");
  writer.declare("tgbotapi_memory_sampler_overhead_bytes", Type::Gauge, "Memory used by the memory accounting itself");
  writer.declare("tgbotapi_memory_sampler_operation_seconds", Type::Histogram,
                 "Duration of operations with sampled allocations, which are the only operations of memory accounting "
                 "taking a lock, including waiting for the lock");
  writer.declare("tgbotapi_memory_sampler_max_operation_seconds", Type::Gauge,
                 "Maximum duration of an operation with a sampled allocation since the start");
  writer.declare("tgbotapi_memory_sampler_max_lock_wait_seconds", Type::Gauge,
                 "Maximum time of waiting for the lock of memory accounting since the start");

  // CPU accounting and threads
  writer.declare("tgbotapi_cpu_sampling_period_seconds", Type::Gauge, "CPU time of a thread between samples");
  writer.declare("tgbotapi_cpu_unattributed_seconds_total", Type::Counter,
                 "Estimated CPU time consumed outside of bots, for example, by HTTP connections, including bots, "
                 "which were closed before authorization");
  writer.declare("tgbotapi_thread_cpu_seconds_total", Type::Counter, "CPU time consumed by each scheduler thread");
  writer.declare("tgbotapi_thread_disk_read_bytes_total", Type::Counter,
                 "Bytes read from the disk by each scheduler thread");
  writer.declare("tgbotapi_thread_disk_written_bytes_total", Type::Counter,
                 "Bytes written to the disk by each scheduler thread");
  writer.declare("tgbotapi_scheduler_lag_seconds", Type::Histogram,
                 "Delay of timer events of each scheduler thread, during which the thread was busy; 20 events per "
                 "second");
  writer.declare("tgbotapi_scheduler_max_lag_seconds", Type::Gauge,
                 "Maximum delay of a timer event of each scheduler thread during the last minute");

  // server
  writer.declare("tgbotapi_active_requests", Type::Gauge, "Number of requests being processed");
  writer.declare("tgbotapi_active_network_queries", Type::Gauge, "Number of active queries to Telegram servers");
  writer.declare("tgbotapi_webhook_connections", Type::Gauge, "Number of active webhook connections");
  writer.declare("tgbotapi_tqueue_expired_updates_total", Type::Counter,
                 "Number of updates deleted from the update queue after expiration");
  writer.declare("tgbotapi_tdlib_restarts_total", Type::Counter, "Number of graceful restarts of TDLib instances");
  writer.declare("tgbotapi_tdlib_failed_restarts_total", Type::Counter,
                 "Number of cancelled graceful restarts of TDLib instances");
  writer.declare("tgbotapi_rejected_requests_total", Type::Counter,
                 "Number of requests, which weren't sent to a bot, by reason");
  writer.declare("tgbotapi_bots", Type::Gauge, "Number of bots by state: authorized or unauthorized");
  writer.declare("tgbotapi_unauthorized_bot_requests_total", Type::Counter,
                 "Number of answered requests of bots, which were closed before authorization, by HTTP status code");

  // bots
  BotMetrics::declare_metrics(writer);
  writer.declare("tgbotapi_bot_info", Type::Gauge, "Information about the bot; the value is always 1");
  writer.declare("tgbotapi_bot_instance_uptime_seconds", Type::Gauge,
                 "Time since the start of the current TDLib instance");
  writer.declare("tgbotapi_bot_tdlib_restarts_total", Type::Counter,
                 "Number of graceful restarts of the TDLib instance");
  writer.declare("tgbotapi_bot_tdlib_restart_in_progress", Type::Gauge,
                 "1 if the TDLib instance is being restarted, otherwise 0");
  writer.declare("tgbotapi_bot_tdlib_restart_delayed_requests", Type::Gauge,
                 "Number of requests delayed by the restart");
  writer.declare("tgbotapi_bot_active_requests", Type::Gauge, "Number of requests being processed");
  writer.declare("tgbotapi_bot_active_file_uploads", Type::Gauge, "Number of files being uploaded in requests");
  writer.declare("tgbotapi_bot_active_file_upload_bytes", Type::Gauge,
                 "Total size of files being uploaded in requests");
  writer.declare("tgbotapi_bot_active_file_downloads", Type::Gauge, "Number of files being downloaded by getFile");
  writer.declare("tgbotapi_bot_pending_updates", Type::Gauge, "Number of updates waiting to be received by the bot");
  writer.declare("tgbotapi_bot_pending_update_chats", Type::Gauge, "Number of chats with partially handled updates");
  writer.declare("tgbotapi_bot_pending_sent_messages", Type::Gauge,
                 "Number of requests waiting for sending of messages");
  writer.declare("tgbotapi_bot_cached_objects", Type::Gauge, "Number of objects in the Bot API cache of the bot");
}

void ClientManager::add_server_metrics(MetricsWriter &writer, double now, td::int64 bot_memory,
                                       td::int64 bot_exact_memory) {
  // process
  writer.add("tgbotapi_uptime_seconds", td::Slice(), now - parameters_->start_time_);
  auto r_mem_stat = td::mem_stat();
  if (r_mem_stat.is_ok()) {
    writer.add("tgbotapi_process_resident_memory_bytes", td::Slice(),
               static_cast<td::int64>(r_mem_stat.ok().resident_size_));
    writer.add("tgbotapi_process_virtual_memory_bytes", td::Slice(),
               static_cast<td::int64>(r_mem_stat.ok().virtual_size_));
  }
#if TD_LINUX
  auto r_cpu_stat = td::cpu_stat();
  auto ticks_per_second = static_cast<double>(sysconf(_SC_CLK_TCK));
  if (r_cpu_stat.is_ok() && ticks_per_second > 0) {
    writer.add("tgbotapi_process_cpu_seconds_total", "mode=\"user\"",
               static_cast<double>(r_cpu_stat.ok().process_user_ticks_) / ticks_per_second);
    writer.add("tgbotapi_process_cpu_seconds_total", "mode=\"system\"",
               static_cast<double>(r_cpu_stat.ok().process_system_ticks_) / ticks_per_second);
  }
#endif
  auto r_io_stat = CpuSampler::get_io_stat(0);
  if (r_io_stat.is_ok()) {
    writer.add("tgbotapi_process_disk_read_bytes_total", td::Slice(), r_io_stat.ok().read_bytes);
    writer.add("tgbotapi_process_disk_written_bytes_total", td::Slice(), r_io_stat.ok().written_bytes);
  }
  writer.add("tgbotapi_buffer_memory_bytes", td::Slice(),
             static_cast<td::int64>(td::BufferAllocator::get_buffer_mem()));

  // memory accounting
  if (HeapSampler::get_sampling_interval() > 0) {
    auto heap_stats = HeapSampler::get_stats();
    writer.add("tgbotapi_heap_bytes", td::Slice(), heap_stats.live_bytes);
    writer.add("tgbotapi_heap_estimated_bytes", td::Slice(), heap_stats.estimated_live_bytes);
    writer.add("tgbotapi_heap_estimate_error_bytes", td::Slice(),
               std::sqrt(td::max(heap_stats.estimate_variance, 0.0)));
    writer.add("tgbotapi_memory_unattributed_bytes", td::Slice(), heap_stats.estimated_live_bytes - bot_memory);
    if (HeapSampler::is_audit_enabled()) {
      writer.add("tgbotapi_heap_audit_bytes", td::Slice(), heap_stats.audit_live_bytes);
      writer.add("tgbotapi_memory_unattributed_exact_bytes", td::Slice(),
                 heap_stats.audit_live_bytes - bot_exact_memory);
    }
    writer.add("tgbotapi_memory_sampling_interval_bytes", td::Slice(), HeapSampler::get_sampling_interval());
    writer.add("tgbotapi_memory_samples", td::Slice(), heap_stats.sample_count);
    writer.add("tgbotapi_memory_sampler_overhead_bytes", td::Slice(), heap_stats.sampler_overhead_bytes);
    std::array<double, HeapSampler::DURATION_BUCKET_COUNT - 1> bounds;
    for (std::size_t i = 0; i < bounds.size(); i++) {
      bounds[i] = static_cast<double>(HeapSampler::DURATION_BUCKETS_NS[i]) * 1e-9;
    }
    writer.add_histogram("tgbotapi_memory_sampler_operation_seconds", td::Slice(), bounds, heap_stats.operation_counts,
                         static_cast<double>(heap_stats.total_operation_ns) * 1e-9);
    writer.add("tgbotapi_memory_sampler_max_operation_seconds", td::Slice(),
               static_cast<double>(heap_stats.max_operation_ns) * 1e-9);
    writer.add("tgbotapi_memory_sampler_max_lock_wait_seconds", td::Slice(),
               static_cast<double>(heap_stats.max_lock_wait_ns) * 1e-9);
  }

  // CPU accounting and threads
  if (cpu_sampling_period_ > 0.0) {
    writer.add("tgbotapi_cpu_sampling_period_seconds", td::Slice(), cpu_sampling_period_);
    writer.add("tgbotapi_cpu_unattributed_seconds_total", td::Slice(),
               static_cast<double>(unattributed_cpu_samples_) * cpu_sampling_period_);
    for (std::size_t scheduler_id = 0; scheduler_id < threads_.size(); scheduler_id++) {
      const auto &thread = threads_[scheduler_id];
      auto label = get_scheduler_label(scheduler_id);
      writer.add("tgbotapi_thread_cpu_seconds_total", label, thread.cpu_time);
      if (thread.thread_id != 0) {
        auto r_thread_io_stat = CpuSampler::get_io_stat(thread.thread_id);
        if (r_thread_io_stat.is_ok()) {
          writer.add("tgbotapi_thread_disk_read_bytes_total", label, r_thread_io_stat.ok().read_bytes);
          writer.add("tgbotapi_thread_disk_written_bytes_total", label, r_thread_io_stat.ok().written_bytes);
        }
      }
      writer.add_histogram("tgbotapi_scheduler_lag_seconds", label, CpuSampler::LAG_BUCKETS, thread.lag_counts,
                           thread.lag_sum);
      double max_lag = 0.0;
      for (auto lag : thread.recent_max_lags) {
        max_lag = td::max(max_lag, lag);
      }
      writer.add("tgbotapi_scheduler_max_lag_seconds", label, max_lag);
    }
  }

  // server
  writer.add("tgbotapi_active_requests", td::Slice(),
             static_cast<td::int64>(parameters_->shared_data_->query_count_.load(std::memory_order_relaxed)));
  writer.add("tgbotapi_active_network_queries", td::Slice(),
             static_cast<td::int64>(td::get_pending_network_query_count(*parameters_->net_query_stats_)));
  writer.add("tgbotapi_webhook_connections", td::Slice(),
             static_cast<td::int64>(WebhookActor::get_total_connection_count()));
  writer.add("tgbotapi_tqueue_expired_updates_total", td::Slice(), tqueue_deleted_events_);
  writer.add("tgbotapi_tdlib_restarts_total", td::Slice(), total_restart_count_);
  writer.add("tgbotapi_tdlib_failed_restarts_total", td::Slice(), total_failed_restart_count_);
  static_assert(
      sizeof(REJECT_REASON_NAMES) / sizeof(REJECT_REASON_NAMES[0]) == static_cast<std::size_t>(RejectReason::Size), "");
  for (std::size_t reason = 0; reason < rejected_queries_.size(); reason++) {
    writer.add("tgbotapi_rejected_requests_total", PSLICE() << "reason=\"" << REJECT_REASON_NAMES[reason] << '"',
               rejected_queries_[reason]);
  }
  td::int64 authorized_bot_count = 0;
  for (auto id : clients_.ids()) {
    if (clients_.get(id)->was_authorized_) {
      authorized_bot_count++;
    }
  }
  writer.add("tgbotapi_bots", "state=\"authorized\"", authorized_bot_count);
  writer.add("tgbotapi_bots", "state=\"unauthorized\"", static_cast<td::int64>(clients_.size()) - authorized_bot_count);
  for (auto &it : unauthorized_bot_requests_) {
    writer.add("tgbotapi_unauthorized_bot_requests_total", PSLICE() << "code=\"" << it.first << '"', it.second);
  }
}

void ClientManager::add_bot_metrics(MetricsWriter &writer, const ClientInfo &client_info, td::Slice bot_label,
                                    double now) {
  client_info.metrics_->write_metrics(writer, bot_label, cpu_sampling_period_);

  writer.add("tgbotapi_bot_instance_uptime_seconds", bot_label, now - client_info.start_time_);
  writer.add("tgbotapi_bot_tdlib_restarts_total", bot_label, static_cast<td::int64>(client_info.restart_count_));
  writer.add("tgbotapi_bot_tdlib_restart_in_progress", bot_label,
             static_cast<td::int64>(client_info.restart_state_ != ClientInfo::RestartState::None));
  writer.add("tgbotapi_bot_tdlib_restart_delayed_requests", bot_label,
             static_cast<td::int64>(client_info.restart_queries_.size()));
  writer.add("tgbotapi_bot_active_requests", bot_label, client_info.stat_.get_active_request_count());
  writer.add("tgbotapi_bot_active_file_uploads", bot_label, client_info.stat_.get_active_file_upload_count());
  writer.add("tgbotapi_bot_active_file_upload_bytes", bot_label, client_info.stat_.get_active_file_upload_bytes());

  if (client_info.client_.empty()) {
    return;  // the TDLib instance is being restarted
  }
  const Client *client = client_info.client_.get_actor_unsafe();
  auto bot_info = client->get_bot_info();
  writer.add("tgbotapi_bot_info",
             PSLICE() << bot_label << ",username=\"" << MetricsWriter::escape_label_value(bot_info.username_)
                      << "\",webhook=\"" << (bot_info.webhook_.empty() ? 0 : 1) << '"',
             static_cast<td::int64>(1));
  writer.add("tgbotapi_bot_pending_updates", bot_label, static_cast<td::int64>(bot_info.pending_update_count_));

  auto cache_stats = client->get_cache_stats();
  writer.add("tgbotapi_bot_active_file_downloads", bot_label,
             static_cast<td::int64>(cache_stats.active_download_count));
  writer.add("tgbotapi_bot_pending_update_chats", bot_label,
             static_cast<td::int64>(cache_stats.pending_update_chat_count));
  writer.add("tgbotapi_bot_pending_sent_messages", bot_label,
             static_cast<td::int64>(cache_stats.pending_send_message_count));
  std::pair<const char *, std::size_t> cached_objects[] = {
      {"message", cache_stats.message_count},
      {"user", cache_stats.user_count},
      {"group", cache_stats.group_count},
      {"supergroup", cache_stats.supergroup_count},
      {"chat", cache_stats.chat_count},
      {"community", cache_stats.community_count},
      {"business_connection", cache_stats.business_connection_count},
      {"sticker_set_name", cache_stats.sticker_set_name_count}};
  for (auto &cached_object : cached_objects) {
    writer.add("tgbotapi_bot_cached_objects", PSLICE() << bot_label << ",type=\"" << cached_object.first << '"',
               static_cast<td::int64>(cached_object.second));
  }
}

void ClientManager::get_metrics(td::Promise<td::BufferSlice> promise,
                                td::vector<std::pair<td::string, td::string>> args) {
  if (close_flag_) {
    return promise.set_error(td::Status::Error(500, "Closing"));
  }
  td::string bot_filter;
  for (auto &arg : args) {
    if (arg.first == "bot") {
      bot_filter = arg.second;
    }
  }

  // only authorized bots are listed to avoid metrics of invalid tokens; if there are many tokens of the same bot,
  // then only the newest one is listed
  std::map<td::string, const ClientInfo *> bots;
  for (auto id : clients_.ids()) {
    const auto *client_info = clients_.get(id);
    CHECK(client_info != nullptr);
    if (client_info->was_authorized_) {
      auto &bot = bots[get_bot_label(*client_info)];
      if (bot == nullptr || bot->first_start_time_ < client_info->first_start_time_) {
        bot = client_info;
      }
    }
  }

  auto now = td::Time::now();
  MetricsWriter writer;
  declare_metrics(writer);
  td::int64 bot_memory = 0;
  td::int64 bot_exact_memory = 0;
  for (auto &bot : bots) {
    auto memory = bot.second->metrics_->get_memory_stats();
    for (std::size_t part = 0; part < static_cast<std::size_t>(BotMetrics::MemoryPart::Size); part++) {
      bot_memory += memory.bytes[part];
      bot_exact_memory += memory.exact_bytes[part];
    }
    if (bot_filter.empty() || bot.first == bot_filter) {
      add_bot_metrics(writer, *bot.second, PSLICE() << "bot=\"" << bot.first << '"', now);
    }
  }
  if (bot_filter.empty()) {
    add_server_metrics(writer, now, bot_memory, bot_exact_memory);
  }
  promise.set_value(td::BufferSlice(writer.finish()));
}

}  // namespace telegram_bot_api
