//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/ClientManager.h"

#include "telegram-bot-api/ClientParameters.h"
#include "telegram-bot-api/WebhookActor.h"

#include "td/telegram/ClientActor.h"
#include "td/telegram/td_api.h"

#include "td/db/binlog/Binlog.h"
#include "td/db/binlog/ConcurrentBinlog.h"
#include "td/db/BinlogKeyValue.h"
#include "td/db/DbKey.h"
#include "td/db/TQueue.h"

#include "td/net/HttpFile.h"

#include "td/actor/MultiPromise.h"
#include "td/actor/SleepActor.h"

#include "td/utils/common.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Parser.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/Stat.h"
#include "td/utils/port/thread.h"
#include "td/utils/Random.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/StackAllocator.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/Time.h"

#include "memprof/memprof.h"

#include <algorithm>
#include <atomic>
#include <tuple>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace telegram_bot_api {

void ClientManager::close(td::Promise<td::Unit> &&promise) {
  close_promises_.push_back(std::move(promise));
  if (close_flag_) {
    return;
  }

  close_flag_ = true;
  watchdog_id_.reset();
  dump_statistics();
  auto ids = clients_.ids();
  for (auto id : ids) {
    auto *client_info = clients_.get(id);
    CHECK(client_info);
    send_closure(client_info->client_, &Client::close);
  }
  if (ids.empty()) {
    close_db();
  }
}

void ClientManager::send(PromisedQueryPtr query) {
  if (close_flag_) {
    // automatically send 429
    return;
  }

  td::string token = query->token().str();
  if (token[0] == '0' || token.size() > 80u || token.find('/') != td::string::npos ||
      token.find(':') == td::string::npos) {
    return fail_query(401, "Unauthorized: invalid token specified", std::move(query));
  }
  auto r_user_id = td::to_integer_safe<td::int64>(query->token().substr(0, token.find(':')));
  if (r_user_id.is_error() || !token_range_(r_user_id.ok())) {
    return fail_query(421, "Misdirected Request: forbidden token specified", std::move(query));
  }
  auto user_id = r_user_id.ok();
  if (user_id <= 0 || user_id >= (static_cast<td::int64>(1) << 54)) {
    return fail_query(401, "Unauthorized: invalid token specified", std::move(query));
  }

  if (query->is_test_dc()) {
    token += "/test";
  }

  auto id_it = token_to_id_.find(token);
  if (id_it == token_to_id_.end()) {
    auto method = query->method();
    if (method == "close") {
      return fail_query(400, "Bad Request: the bot has already been closed", std::move(query));
    }

    td::string ip_address = query->get_peer_ip_address();
    if (!ip_address.empty()) {
      td::IPAddress tmp;
      tmp.init_host_port(ip_address, 0).ignore();
      tmp.clear_ipv6_interface();
      if (tmp.is_valid()) {
        ip_address = tmp.get_ip_str().str();
      }
    }
    LOG(DEBUG) << "Receive incoming query for new bot " << token << " from " << ip_address;
    if (!ip_address.empty()) {
      LOG(DEBUG) << "Check Client creation flood control for IP address " << ip_address;
      auto res = flood_controls_.emplace(std::move(ip_address), td::FloodControlFast());
      auto &flood_control = res.first->second;
      if (res.second) {
        flood_control.add_limit(60, 20);        // 20 in a minute
        flood_control.add_limit(60 * 60, 600);  // 600 in an hour
      }
      auto now = td::Time::now();
      auto wakeup_at = flood_control.get_wakeup_at();
      if (wakeup_at > now) {
        LOG(INFO) << "Failed to create Client from IP address " << ip_address << " with token";
        return query->set_retry_after_error(static_cast<int>(wakeup_at - now) + 1);
      }
      flood_control.add_event(now);
    }
    if (is_global_flood_control_enabled_) {
      auto now = td::Time::now();
      auto wakeup_at = global_flood_control_.get_wakeup_at();
      if (wakeup_at > now) {
        LOG(WARNING) << "Failed to create Client with token " << token;
        return query->set_retry_after_error(static_cast<int>(wakeup_at - now) + 1);
      }
      global_flood_control_.add_event(now);
    }
    auto tqueue_id = get_tqueue_id(user_id, query->is_test_dc());
    if (active_client_count_.count(tqueue_id) != 0) {
      // return query->set_retry_after_error(1);
    }

    auto id = clients_.create(ClientInfo(BotStatActor(stat_.actor_id(&stat_)), token, tqueue_id, query->token().str(),
                                         query->is_test_dc(), td::Time::now()));
    create_client_actor(id);
    auto *client_info = clients_.get(id);

    if (method != "deletewebhook" && method != "setwebhook") {
      auto bot_token_with_dc = PSTRING() << query->token() << (query->is_test_dc() ? ":T" : "");
      auto webhook_info = parameters_->shared_data_->webhook_db_->get(bot_token_with_dc);
      if (!webhook_info.empty()) {
        send_closure(client_info->client_, &Client::send,
                     get_webhook_restore_query(bot_token_with_dc, webhook_info, parameters_->shared_data_));
      }
    }

    std::tie(id_it, std::ignore) = token_to_id_.emplace(token, id);
  }
  auto *client_info = clients_.get(id_it->second);
  CHECK(client_info != nullptr);
  if (client_info->restart_state_ != ClientInfo::RestartState::None &&
      (client_info->restart_state_ != ClientInfo::RestartState::Pausing || query->method() == "getupdates")) {
    // the query will be sent to the restarted Client
    if (client_info->restart_queries_.size() >= MAX_RESTART_QUERIES) {
      return query->set_retry_after_error(1);
    }
    client_info->restart_queries_.push(std::move(query));
    return;
  }
  send_closure(client_info->client_, &Client::send, std::move(query));  // will send 429 if the client is already closed
}

void ClientManager::create_client_actor(td::uint64 id) {
  auto *client_info = clients_.get(id);
  CHECK(client_info != nullptr);
  CHECK(client_info->client_.empty());
  auto now = td::Time::now();
  client_info->start_time_ = now;
  client_info->scheduled_restart_time_ = get_scheduled_restart_time(now);
  client_info->client_ =
      td::create_actor<Client>(PSLICE() << "Client/" << client_info->token_, actor_shared(this, id),
                               client_info->bot_token_, client_info->is_test_dc_, client_info->tqueue_id_, parameters_,
                               client_info->stat_.actor_id(&client_info->stat_), client_info->first_start_time_);
}

void ClientManager::schedule_restart_check() {
  td::create_actor<td::SleepActor>("RestartCheckSleepActor", RESTART_CHECK_PERIOD,
                                   td::PromiseCreator::lambda([actor_id = actor_id(this)](td::Result<td::Unit>) {
                                     send_closure(actor_id, &ClientManager::on_restart_check_timer);
                                   }))
      .release();
}

void ClientManager::on_restart_check_timer() {
  if (close_flag_) {
    return;
  }
  auto now = td::Time::now();
  check_restarts(now);

#if defined(__GLIBC__)
  if (malloc_trim_time_ > 0.0 && now >= malloc_trim_time_) {
    malloc_trim_time_ = 0.0;
    td::Scheduler::instance()->run_on_scheduler(SharedData::get_file_gc_scheduler_id(), [](td::Unit) {
      auto start_time = td::Time::now();
      malloc_trim(0);
      LOG(WARNING) << "Released free memory to the OS in " << (td::Time::now() - start_time) << " seconds";
    });
  }
#endif

  schedule_restart_check();
}

double ClientManager::get_scheduled_restart_time(double now) const {
  auto restart_interval = parameters_->tdlib_restart_interval_;
  if (restart_interval <= 0.0) {
    return 0.0;
  }
  // add up to 10% of random delay to spread restarts of different bots in time
  return now + restart_interval * (1.0 + 0.1 * td::Random::fast(0, 1000) * 1e-3);
}

td::Slice ClientManager::get_restart_state_name(ClientInfo::RestartState state) {
  switch (state) {
    case ClientInfo::RestartState::None:
      return td::Slice("none");
    case ClientInfo::RestartState::Pausing:
      return td::Slice("pausing");
    case ClientInfo::RestartState::Draining:
      return td::Slice("draining");
    case ClientInfo::RestartState::Closing:
      return td::Slice("closing");
    default:
      UNREACHABLE();
      return td::Slice();
  }
}

td::int32 ClientManager::request_restart(td::Slice bot_id) {
  td::int32 result = 0;
  for (auto id : clients_.ids()) {
    auto *client_info = clients_.get(id);
    CHECK(client_info != nullptr);
    if (client_info->restart_state_ != ClientInfo::RestartState::None) {
      continue;
    }
    if (bot_id == "all" || (td::begins_with(client_info->token_, bot_id) &&
                            client_info->token_.size() > bot_id.size() && client_info->token_[bot_id.size()] == ':')) {
      LOG(WARNING) << "Graceful restart of TDLib instance of bot " << client_info->tqueue_id_ << " was requested";
      client_info->is_restart_requested_ = true;
      has_restart_requests_ = true;
      result++;
    }
  }
  return result;
}

void ClientManager::check_restarts(double now) {
  if (close_flag_) {
    return;
  }

  if (restarting_client_id_ != 0) {
    auto *client_info = clients_.get(restarting_client_id_);
    if (client_info == nullptr || client_info->restart_state_ == ClientInfo::RestartState::None) {
      restarting_client_id_ = 0;
    } else {
      if (now > client_info->restart_start_time_ + parameters_->tdlib_restart_drain_timeout_ + MAX_RESTART_DURATION &&
          !client_info->restart_queries_.empty()) {
        // must never happen; don't keep connections open forever
        LOG(ERROR) << "Restart of TDLib instance of bot " << client_info->tqueue_id_ << " lasts for "
                   << (now - client_info->restart_start_time_) << " seconds in state "
                   << get_restart_state_name(client_info->restart_state_);
        fail_restart_queries(client_info);
      }
      // restart only one TDLib instance simultaneously
      return;
    }
  }
  if (now < next_restart_time_) {
    return;
  }
  if (parameters_->tdlib_restart_interval_ <= 0.0 && parameters_->tdlib_restart_memory_limit_ <= 0 &&
      !has_restart_requests_) {
    return;
  }
  has_restart_requests_ = false;

  auto can_restart = [&](const ClientInfo *client_info) {
    return client_info->restart_state_ == ClientInfo::RestartState::None && !client_info->client_.empty() &&
           client_info->client_.get_actor_unsafe()->can_restart();
  };

  td::uint64 requested_client_id = 0;
  td::uint64 scheduled_client_id = 0;
  double min_scheduled_restart_time = now;
  auto client_ids = clients_.ids();
  for (auto id : client_ids) {
    auto *client_info = clients_.get(id);
    CHECK(client_info != nullptr);
    if (client_info->is_restart_requested_) {
      has_restart_requests_ = true;
      if (requested_client_id == 0 && can_restart(client_info)) {
        requested_client_id = id;
      }
      continue;
    }
    if (client_info->scheduled_restart_time_ > 0.0 &&
        client_info->scheduled_restart_time_ <= min_scheduled_restart_time && now >= client_info->restart_retry_time_ &&
        can_restart(client_info)) {
      min_scheduled_restart_time = client_info->scheduled_restart_time_;
      scheduled_client_id = id;
    }
  }
  if (requested_client_id != 0) {
    return start_restart(requested_client_id, "request");
  }

  auto memory_limit = parameters_->tdlib_restart_memory_limit_;
  if (memory_limit > 0 && now >= next_memory_check_time_) {
    next_memory_check_time_ = now + RESTART_MEMORY_CHECK_PERIOD;
    auto r_mem_stat = td::mem_stat();
    if (r_mem_stat.is_ok() && r_mem_stat.ok().resident_size_ > static_cast<td::uint64>(memory_limit)) {
      // restart the TDLib instance, which has received the biggest number of updates and is likely to be the biggest
      td::uint64 heaviest_client_id = 0;
      td::int64 max_update_count = -1;
      for (auto id : client_ids) {
        auto *client_info = clients_.get(id);
        CHECK(client_info != nullptr);
        if (now < client_info->start_time_ + parameters_->tdlib_restart_min_uptime_ ||
            now < client_info->restart_retry_time_ || !can_restart(client_info)) {
          continue;
        }
        auto update_count = client_info->client_.get_actor_unsafe()->get_tdlib_update_count();
        if (update_count > max_update_count) {
          max_update_count = update_count;
          heaviest_client_id = id;
        }
      }
      if (heaviest_client_id != 0) {
        LOG(WARNING) << "Memory usage " << td::format::as_size(r_mem_stat.ok().resident_size_) << " exceeds the limit "
                     << td::format::as_size(static_cast<td::uint64>(memory_limit));
        return start_restart(heaviest_client_id, "memory limit");
      }
    }
  }

  if (scheduled_client_id != 0) {
    return start_restart(scheduled_client_id, "uptime");
  }
}

void ClientManager::start_restart(td::uint64 id, td::Slice reason) {
  auto *client_info = clients_.get(id);
  CHECK(client_info != nullptr);
  CHECK(client_info->restart_state_ == ClientInfo::RestartState::None);
  auto now = td::Time::now();
  LOG(WARNING) << "Start graceful restart of TDLib instance of bot " << client_info->tqueue_id_ << " with uptime "
               << (now - client_info->start_time_) << " because of " << reason;
  client_info->restart_state_ = ClientInfo::RestartState::Pausing;
  client_info->restart_start_time_ = now;
  client_info->is_restart_requested_ = false;
  restarting_client_id_ = id;
  next_restart_time_ = now + parameters_->tdlib_restart_cooldown_;
  send_closure(client_info->client_, &Client::restart, parameters_->tdlib_restart_drain_timeout_,
               td::PromiseCreator::lambda([actor_id = actor_id(this), id](td::Result<td::Unit> result) {
                 send_closure(actor_id, &ClientManager::on_restart_paused, id, std::move(result));
               }));
}

void ClientManager::on_restart_paused(td::uint64 id, td::Result<td::Unit> result) {
  auto *client_info = clients_.get(id);
  if (client_info == nullptr || client_info->restart_state_ != ClientInfo::RestartState::Pausing) {
    return;
  }
  if (result.is_error()) {
    return cancel_restart(client_info, id, result.error());
  }

  // the bot has no active requests; delay all new requests till the end of the restart
  client_info->restart_state_ = ClientInfo::RestartState::Draining;
  send_closure(client_info->client_, &Client::continue_restart,
               td::PromiseCreator::lambda([actor_id = actor_id(this), id](td::Result<td::Unit> result) {
                 send_closure(actor_id, &ClientManager::on_restart_closing, id, std::move(result));
               }));
}

void ClientManager::on_restart_closing(td::uint64 id, td::Result<td::Unit> result) {
  auto *client_info = clients_.get(id);
  if (client_info == nullptr || client_info->restart_state_ != ClientInfo::RestartState::Draining) {
    return;
  }
  if (result.is_error()) {
    return cancel_restart(client_info, id, result.error());
  }
  client_info->restart_state_ = ClientInfo::RestartState::Closing;
}

void ClientManager::cancel_restart(ClientInfo *client_info, td::uint64 id, const td::Status &error) {
  LOG(WARNING) << "Failed to restart TDLib instance of bot " << client_info->tqueue_id_ << ": " << error.message();
  client_info->restart_state_ = ClientInfo::RestartState::None;
  client_info->failed_restart_count_++;
  client_info->restart_retry_time_ =
      td::Time::now() + RESTART_RETRY_DELAY * td::min(client_info->failed_restart_count_, 12);
  total_failed_restart_count_++;
  if (restarting_client_id_ == id) {
    restarting_client_id_ = 0;
  }
  if (close_flag_) {
    fail_restart_queries(client_info);
  } else {
    send_restart_queries(client_info);
  }
}

void ClientManager::finish_restart(td::uint64 id) {
  auto *client_info = clients_.get(id);
  CHECK(client_info != nullptr);
  CHECK(client_info->restart_state_ == ClientInfo::RestartState::Closing);
  auto now = td::Time::now();
  LOG(WARNING) << "Closed TDLib instance of bot " << client_info->tqueue_id_ << " for restart in "
               << (now - client_info->restart_start_time_) << " seconds; have " << client_info->restart_queries_.size()
               << " delayed queries";
  client_info->restart_state_ = ClientInfo::RestartState::None;
  client_info->restart_count_++;
  client_info->failed_restart_count_ = 0;
  client_info->restart_retry_time_ = 0.0;
  total_restart_count_++;
  if (restarting_client_id_ == id) {
    restarting_client_id_ = 0;
  }

  create_client_actor(id);

  // the webhook must be restored before any other query is processed
  auto bot_token_with_dc = PSTRING() << client_info->bot_token_ << (client_info->is_test_dc_ ? ":T" : "");
  auto webhook_info = parameters_->shared_data_->webhook_db_->get(bot_token_with_dc);
  if (!webhook_info.empty()) {
    send_closure(client_info->client_, &Client::send,
                 get_webhook_restore_query(bot_token_with_dc, webhook_info, parameters_->shared_data_));
  }

  send_restart_queries(client_info);

  if (parameters_->malloc_trim_) {
    // return memory released by the closed TDLib instance to the OS
    malloc_trim_time_ = now + MALLOC_TRIM_DELAY;
  }
}

void ClientManager::send_restart_queries(ClientInfo *client_info) {
  std::queue<PromisedQueryPtr> queries;
  std::swap(queries, client_info->restart_queries_);
  while (!queries.empty()) {
    send_closure(client_info->client_, &Client::send_delayed, std::move(queries.front()));
    queries.pop();
  }
}

void ClientManager::fail_restart_queries(ClientInfo *client_info) {
  std::queue<PromisedQueryPtr> queries;
  std::swap(queries, client_info->restart_queries_);
  while (!queries.empty()) {
    queries.front()->set_retry_after_error(1);
    queries.pop();
  }
}

ClientManager::TopClients ClientManager::get_top_clients(std::size_t max_count, td::Slice token_filter) {
  auto now = td::Time::now();
  TopClients result;
  td::vector<std::pair<td::int64, td::uint64>> top_client_ids;
  for (auto id : clients_.ids()) {
    auto *client_info = clients_.get(id);
    CHECK(client_info);

    if (client_info->stat_.is_active(now)) {
      result.active_count++;
    }

    if (!td::begins_with(client_info->token_, token_filter)) {
      continue;
    }

    auto score = static_cast<td::int64>(client_info->stat_.get_score(now) * -1e9);
    if (score == 0 && top_client_ids.size() >= max_count) {
      continue;
    }
    top_client_ids.emplace_back(score, id);
  }
  if (top_client_ids.size() < max_count) {
    max_count = top_client_ids.size();
  }
  std::partial_sort(top_client_ids.begin(), top_client_ids.begin() + max_count, top_client_ids.end());
  result.top_client_ids.reserve(max_count);
  for (std::size_t i = 0; i < max_count; i++) {
    result.top_client_ids.push_back(top_client_ids[i].second);
  }
  return result;
}

void ClientManager::get_stats(td::Promise<td::BufferSlice> promise,
                              td::vector<std::pair<td::string, td::string>> args) {
  if (close_flag_) {
    promise.set_value(td::BufferSlice("Closing"));
    return;
  }
  size_t buf_size = 1 << 14;
  auto buf = td::StackAllocator::alloc(buf_size);
  td::StringBuilder sb(buf.as_slice());

  td::Slice id_filter;
  td::Slice restart_filter;
  int new_verbosity_level = -1;
  td::string tag;
  for (auto &arg : args) {
    if (arg.first == "id") {
      id_filter = arg.second;
    }
    if (arg.first == "restart") {
      restart_filter = arg.second;
    }
    if (arg.first == "v") {
      auto r_new_verbosity_level = td::to_integer_safe<int>(arg.second);
      if (r_new_verbosity_level.is_ok()) {
        new_verbosity_level = r_new_verbosity_level.ok();
      }
    }
    if (arg.first == "tag") {
      tag = arg.second;
    }
  }
  if (new_verbosity_level > 0) {
    if (tag.empty()) {
      parameters_->shared_data_->next_verbosity_level_ = new_verbosity_level;
    } else {
      td::ClientActor::execute(td::td_api::make_object<td::td_api::setLogTagVerbosityLevel>(tag, new_verbosity_level));
    }
  }

  if (!restart_filter.empty()) {
    sb << "tdlib_restart_requested\t" << request_restart(restart_filter) << '\n';
  }

  auto now = td::Time::now();
  auto top_clients = get_top_clients(50, id_filter);
  sb << BotStatActor::get_description() << '\n';
  if (id_filter.empty()) {
    sb << "uptime\t" << now - parameters_->start_time_ << '\n';
    sb << "bot_count\t" << clients_.size() << '\n';
    sb << "active_bot_count\t" << top_clients.active_count << '\n';
    auto r_mem_stat = td::mem_stat();
    if (r_mem_stat.is_ok()) {
      auto mem_stat = r_mem_stat.move_as_ok();
      sb << "rss\t" << td::format::as_size(mem_stat.resident_size_) << '\n';
      sb << "vm\t" << td::format::as_size(mem_stat.virtual_size_) << '\n';
      sb << "rss_peak\t" << td::format::as_size(mem_stat.resident_size_peak_) << '\n';
      sb << "vm_peak\t" << td::format::as_size(mem_stat.virtual_size_peak_) << '\n';
    } else {
      LOG(INFO) << "Failed to get memory statistics: " << r_mem_stat.error();
    }

    auto cpu_stats = ServerCpuStat::instance().as_vector(td::Time::now());
    for (auto &stat : cpu_stats) {
      sb << stat.key_ << "\t" << stat.value_ << '\n';
    }

    sb << "buffer_memory\t" << td::format::as_size(td::BufferAllocator::get_buffer_mem()) << '\n';
    sb << "tdlib_restart_count\t" << total_restart_count_ << '\n';
    sb << "tdlib_failed_restart_count\t" << total_failed_restart_count_ << '\n';
    sb << "active_webhook_connections\t" << WebhookActor::get_total_connection_count() << '\n';
    sb << "active_requests\t" << parameters_->shared_data_->query_count_.load(std::memory_order_relaxed) << '\n';
    sb << "active_network_queries\t" << td::get_pending_network_query_count(*parameters_->net_query_stats_) << '\n';
    auto stats = stat_.as_vector(now);
    for (auto &stat : stats) {
      sb << stat.key_ << "\t" << stat.value_ << '\n';
    }
  }

  for (auto top_client_id : top_clients.top_client_ids) {
    auto *client_info = clients_.get(top_client_id);
    CHECK(client_info);

    auto bot_info = client_info->client_.get_actor_unsafe()->get_bot_info();
    auto active_request_count = client_info->stat_.get_active_request_count();
    auto active_file_upload_bytes = client_info->stat_.get_active_file_upload_bytes();
    auto active_file_upload_count = client_info->stat_.get_active_file_upload_count();
    sb << '\n';
    sb << "id\t" << bot_info.id_ << '\n';
    sb << "uptime\t" << now - bot_info.start_time_ << '\n';
    sb << "token\t" << bot_info.token_ << '\n';
    sb << "username\t" << bot_info.username_ << '\n';
    if (active_request_count != 0) {
      sb << "active_request_count\t" << active_request_count << '\n';
    }
    if (active_file_upload_bytes != 0) {
      sb << "active_file_upload_bytes\t" << active_file_upload_bytes << '\n';
    }
    if (active_file_upload_count != 0) {
      sb << "active_file_upload_count\t" << active_file_upload_count << '\n';
    }
    if (!bot_info.webhook_.empty()) {
      sb << "webhook\t" << bot_info.webhook_ << '\n';
      if (bot_info.has_webhook_certificate_) {
        sb << "has_custom_certificate\t" << bot_info.has_webhook_certificate_ << '\n';
      }
      if (bot_info.webhook_max_connections_ != parameters_->default_max_webhook_connections_) {
        sb << "webhook_max_connections\t" << bot_info.webhook_max_connections_ << '\n';
      }
    }
    sb << "head_update_id\t" << bot_info.head_update_id_ << '\n';
    if (bot_info.pending_update_count_ != 0) {
      sb << "tail_update_id\t" << bot_info.tail_update_id_ << '\n';
      sb << "pending_update_count\t" << bot_info.pending_update_count_ << '\n';
    }
    sb << "tdlib_update_count\t" << client_info->client_.get_actor_unsafe()->get_tdlib_update_count() << '\n';
    if (client_info->restart_count_ != 0) {
      sb << "tdlib_restart_count\t" << client_info->restart_count_ << '\n';
    }
    if (client_info->scheduled_restart_time_ > 0.0) {
      sb << "tdlib_next_restart_in\t" << td::max(client_info->scheduled_restart_time_ - now, 0.0) << '\n';
    }
    if (client_info->restart_state_ != ClientInfo::RestartState::None) {
      sb << "tdlib_restart_state\t" << get_restart_state_name(client_info->restart_state_) << '\n';
      sb << "tdlib_restart_duration\t" << now - client_info->restart_start_time_ << '\n';
      sb << "tdlib_restart_delayed_query_count\t" << client_info->restart_queries_.size() << '\n';
    }

    auto stats = client_info->stat_.as_vector(now);
    for (auto &stat : stats) {
      if (stat.key_ == "update_count" || stat.key_ == "request_count") {
        sb << stat.key_ << "/sec\t" << stat.value_ << '\n';
      }
    }

    if (sb.is_error()) {
      break;
    }
  }
  // ignore sb overflow
  promise.set_value(td::BufferSlice(sb.as_cslice()));
}

td::int64 ClientManager::get_tqueue_id(td::int64 user_id, bool is_test_dc) {
  return user_id + (static_cast<td::int64>(is_test_dc) << 54);
}

void ClientManager::start_up() {
  // init tqueue
  {
    auto load_start_time = td::Time::now();
    auto tqueue_binlog = td::make_unique<td::TQueueBinlog<td::Binlog>>();
    auto binlog = td::make_unique<td::Binlog>();
    auto tqueue = td::TQueue::create();
    td::vector<td::uint64> failed_to_replay_log_event_ids;
    td::int64 loaded_event_count = 0;
    binlog
        ->init(parameters_->working_directory_ + "tqueue.binlog",
               [&](const td::BinlogEvent &event) {
                 if (tqueue_binlog->replay(event, *tqueue).is_error()) {
                   failed_to_replay_log_event_ids.push_back(event.id_);
                 } else {
                   loaded_event_count++;
                 }
               })
        .ensure();
    tqueue_binlog.reset();

    if (!failed_to_replay_log_event_ids.empty()) {
      LOG(ERROR) << "Failed to replay " << failed_to_replay_log_event_ids.size() << " TQueue events";
      for (auto &log_event_id : failed_to_replay_log_event_ids) {
        binlog->erase(log_event_id);
      }
    }

    auto concurrent_binlog =
        std::make_shared<td::ConcurrentBinlog>(std::move(binlog), SharedData::get_binlog_scheduler_id());
    auto concurrent_tqueue_binlog = td::make_unique<td::TQueueBinlog<td::BinlogInterface>>();
    concurrent_tqueue_binlog->set_binlog(std::move(concurrent_binlog));
    tqueue->set_callback(std::move(concurrent_tqueue_binlog));

    parameters_->shared_data_->tqueue_ = std::move(tqueue);

    LOG(WARNING) << "Loaded " << loaded_event_count << " TQueue events in " << (td::Time::now() - load_start_time)
                 << " seconds";
    next_tqueue_gc_time_ = td::Time::now() + 600;
  }

  // init webhook_db
  auto concurrent_webhook_db = td::make_unique<td::BinlogKeyValue<td::ConcurrentBinlog>>();
  auto status = concurrent_webhook_db->init(parameters_->working_directory_ + "webhooks_db.binlog", td::DbKey::empty(),
                                            SharedData::get_binlog_scheduler_id());
  LOG_IF(FATAL, status.is_error()) << "Can't open webhooks_db.binlog " << status;
  parameters_->shared_data_->webhook_db_ = std::move(concurrent_webhook_db);

  auto &webhook_db = *parameters_->shared_data_->webhook_db_;
  for (const auto &key_value : webhook_db.get_all()) {
    if (!token_range_(td::to_integer<td::uint64>(key_value.first))) {
      LOG(WARNING) << "DROP WEBHOOK: " << key_value.first << " ---> " << key_value.second;
      webhook_db.erase(key_value.first);
      continue;
    }

    auto query = get_webhook_restore_query(key_value.first, key_value.second, parameters_->shared_data_);
    send_closure_later(actor_id(this), &ClientManager::send, std::move(query));
  }

  // launch watchdog
  watchdog_id_ = td::create_actor_on_scheduler<Watchdog>("ManagerWatchdog", SharedData::get_watchdog_scheduler_id(),
                                                         td::this_thread::get_id(), WATCHDOG_TIMEOUT);
  set_timeout_in(600.0);

  schedule_restart_check();
}

PromisedQueryPtr ClientManager::get_webhook_restore_query(td::Slice token, td::Slice webhook_info,
                                                          std::shared_ptr<SharedData> shared_data) {
  // create Query with empty promise
  td::vector<td::BufferSlice> containers;
  auto add_string = [&containers](td::Slice str) {
    containers.emplace_back(str);
    return containers.back().as_mutable_slice();
  };

  token = add_string(token);

  LOG(WARNING) << "WEBHOOK: " << token << " ---> " << webhook_info;

  bool is_test_dc = false;
  if (td::ends_with(token, ":T")) {
    token.remove_suffix(2);
    is_test_dc = true;
  }

  td::ConstParser parser{webhook_info};
  td::vector<std::pair<td::MutableSlice, td::MutableSlice>> args;
  if (parser.try_skip("cert/")) {
    args.emplace_back(add_string("certificate"), add_string("previous"));
  }

  if (parser.try_skip("#maxc")) {
    args.emplace_back(add_string("max_connections"), add_string(parser.read_till('/')));
    parser.skip('/');
  }

  if (parser.try_skip("#ip")) {
    args.emplace_back(add_string("ip_address"), add_string(parser.read_till('/')));
    parser.skip('/');
  }

  if (parser.try_skip("#fix_ip")) {
    args.emplace_back(add_string("fix_ip_address"), add_string("1"));
    parser.skip('/');
  }

  if (parser.try_skip("#secret")) {
    args.emplace_back(add_string("secret_token"), add_string(parser.read_till('/')));
    parser.skip('/');
  }

  if (parser.try_skip("#allow")) {
    args.emplace_back(add_string("allowed_updates"), add_string(parser.read_till('/')));
    parser.skip('/');
  }

  args.emplace_back(add_string("url"), add_string(parser.read_all()));

  const auto method = add_string("setwebhook");
  auto query = td::make_unique<Query>(std::move(containers), token, is_test_dc, method, std::move(args),
                                      td::vector<std::pair<td::MutableSlice, td::MutableSlice>>(),
                                      td::vector<td::HttpFile>(), std::move(shared_data), td::IPAddress(), true);
  return PromisedQueryPtr(query.release(), PromiseDeleter(td::Promise<td::unique_ptr<Query>>()));
}

void ClientManager::dump_statistics() {
  if (is_memprof_on()) {
    LOG(WARNING) << "Memory dump:";
    td::vector<AllocInfo> v;
    dump_alloc([&](const AllocInfo &info) { v.push_back(info); });
    std::sort(v.begin(), v.end(), [](const AllocInfo &a, const AllocInfo &b) { return a.size > b.size; });
    size_t total_size = 0;
    size_t other_size = 0;
    int count = 0;
    for (auto &info : v) {
      if (count++ < 50) {
        LOG(WARNING) << td::format::as_size(info.size) << td::format::as_array(info.backtrace);
      } else {
        other_size += info.size;
      }
      total_size += info.size;
    }
    LOG(WARNING) << td::tag("other", td::format::as_size(other_size));
    LOG(WARNING) << td::tag("total size", td::format::as_size(total_size));
    LOG(WARNING) << td::tag("total traces", get_ht_size());
    LOG(WARNING) << td::tag("fast_backtrace_success_rate", get_fast_backtrace_success_rate());
  }
  auto r_mem_stat = td::mem_stat();
  if (r_mem_stat.is_ok()) {
    auto mem_stat = r_mem_stat.move_as_ok();
    LOG(WARNING) << td::tag("rss", td::format::as_size(mem_stat.resident_size_));
    LOG(WARNING) << td::tag("vm", td::format::as_size(mem_stat.virtual_size_));
    LOG(WARNING) << td::tag("rss_peak", td::format::as_size(mem_stat.resident_size_peak_));
    LOG(WARNING) << td::tag("vm_peak", td::format::as_size(mem_stat.virtual_size_peak_));
  }
  LOG(WARNING) << td::tag("buffer_mem", td::format::as_size(td::BufferAllocator::get_buffer_mem()));
  LOG(WARNING) << td::tag("buffer_slice_size", td::format::as_size(td::BufferAllocator::get_buffer_slice_size()));

  const auto &shared_data = parameters_->shared_data_;
  auto query_list_size = shared_data->query_list_size_.load(std::memory_order_relaxed);
  auto query_count = shared_data->query_count_.load(std::memory_order_relaxed);
  LOG(WARNING) << td::tag("pending queries", query_count) << td::tag("pending requests", query_list_size);

  td::uint64 i = 0;
  bool was_gap = false;
  for (auto end = &shared_data->query_list_, cur = end->prev; cur != end; cur = cur->prev, i++) {
    if (i < 20 || i > query_list_size - 20 || i % (query_list_size / 50 + 1) == 0) {
      if (was_gap) {
        LOG(WARNING) << "...";
        was_gap = false;
      }
      LOG(WARNING) << static_cast<const Query &>(*cur);
    } else {
      was_gap = true;
    }
  }

  td::dump_pending_network_queries(*parameters_->net_query_stats_);

  auto now = td::Time::now();
  auto top_clients = get_top_clients(10, {});
  for (auto top_client_id : top_clients.top_client_ids) {
    auto *client_info = clients_.get(top_client_id);
    CHECK(client_info);

    auto bot_info = client_info->client_.get_actor_unsafe()->get_bot_info();
    td::string update_count;
    td::string request_count;
    auto replace_tabs = [](td::string &str) {
      for (auto &c : str) {
        if (c == '\t') {
          c = ' ';
        }
      }
    };
    auto stats = client_info->stat_.as_vector(now);
    for (auto &stat : stats) {
      if (stat.key_ == "update_count") {
        replace_tabs(stat.value_);
        update_count = std::move(stat.value_);
      }
      if (stat.key_ == "request_count") {
        replace_tabs(stat.value_);
        request_count = std::move(stat.value_);
      }
    }
    LOG(WARNING) << td::tag("id", bot_info.id_) << td::tag("update_count", update_count)
                 << td::tag("request_count", request_count);
  }
}

void ClientManager::raw_event(const td::Event::Raw &event) {
  auto id = get_link_token();
  auto *info = clients_.get(id);
  CHECK(info != nullptr);
  CHECK(info->tqueue_id_ != 0);
  auto &value = active_client_count_[info->tqueue_id_];
  if (event.ptr != nullptr) {
    value++;
  } else {
    CHECK(value > 0);
    if (--value == 0) {
      active_client_count_.erase(info->tqueue_id_);
    }
  }
}

void ClientManager::timeout_expired() {
  send_closure(watchdog_id_, &Watchdog::kick);
  set_timeout_in(WATCHDOG_TIMEOUT / 10);

  double now = td::Time::now();

  if (now > next_tqueue_gc_time_) {
    auto unix_time = parameters_->shared_data_->get_unix_time(now);
    LOG(INFO) << "Run TQueue GC at " << unix_time;
    td::int64 deleted_events;
    bool is_finished;
    std::tie(deleted_events, is_finished) = parameters_->shared_data_->tqueue_->run_gc(unix_time);
    LOG(INFO) << "TQueue GC deleted " << deleted_events << " events";
    next_tqueue_gc_time_ = td::Time::now() + (is_finished ? 60.0 : 1.0);

    tqueue_deleted_events_ += deleted_events;
    if (tqueue_deleted_events_ > last_tqueue_deleted_events_ + 10000) {
      LOG(WARNING) << "TQueue GC already deleted " << tqueue_deleted_events_ << " events since the start";
      last_tqueue_deleted_events_ = tqueue_deleted_events_;
    }
  }

  if (!is_global_flood_control_enabled_ && !parameters_->local_mode_) {
    is_global_flood_control_enabled_ = true;
    global_flood_control_.add_limit(60, 1000);        // 1000 in a minute
    global_flood_control_.add_limit(60 * 60, 10000);  // 10000 in an hour
  }
}

void ClientManager::hangup_shared() {
  auto id = get_link_token();
  auto *info = clients_.get(id);
  CHECK(info != nullptr);
  info->client_.release();

  if (info->restart_state_ == ClientInfo::RestartState::Closing && !close_flag_) {
    // the TDLib instance was closed for restart; create a new Client and send it all delayed queries
    return finish_restart(id);
  }
  if (restarting_client_id_ == id) {
    restarting_client_id_ = 0;
  }

  token_to_id_.erase(info->token_);
  clients_.erase(id);

  if (close_flag_ && clients_.empty()) {
    CHECK(active_client_count_.empty());
    close_db();
  }
}

void ClientManager::close_db() {
  LOG(WARNING) << "Closing databases";
  td::MultiPromiseActorSafe mpas("close binlogs");
  mpas.add_promise(td::PromiseCreator::lambda(
      [actor_id = actor_id(this)](td::Unit) { send_closure(actor_id, &ClientManager::finish_close); }));
  mpas.set_ignore_errors(true);

  auto lock = mpas.get_promise();
  parameters_->shared_data_->tqueue_->close(mpas.get_promise());
  parameters_->shared_data_->webhook_db_->close(mpas.get_promise());
  lock.set_value(td::Unit());
}

void ClientManager::finish_close() {
  LOG(WARNING) << "Stop ClientManager";
  auto promises = std::move(close_promises_);
  for (auto &promise : promises) {
    promise.set_value(td::Unit());
  }
  stop();
}

}  // namespace telegram_bot_api
