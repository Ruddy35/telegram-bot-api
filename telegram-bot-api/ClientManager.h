//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/Client.h"
#include "telegram-bot-api/Query.h"
#include "telegram-bot-api/Stats.h"
#include "telegram-bot-api/Watchdog.h"

#include "td/actor/actor.h"

#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/Container.h"
#include "td/utils/FlatHashMap.h"
#include "td/utils/FloodControlFast.h"
#include "td/utils/Promise.h"
#include "td/utils/Slice.h"

#include <memory>
#include <queue>
#include <utility>

namespace telegram_bot_api {

struct ClientParameters;
struct SharedData;

class ClientManager final : public td::Actor {
 public:
  struct TokenRange {
    td::uint64 rem;
    td::uint64 mod;
    bool operator()(td::uint64 x) {
      return x % mod == rem;
    }
  };
  ClientManager(std::shared_ptr<const ClientParameters> parameters, TokenRange token_range)
      : parameters_(std::move(parameters)), token_range_(token_range) {
  }

  void dump_statistics();

  void send(PromisedQueryPtr query);

  void get_stats(td::Promise<td::BufferSlice> promise, td::vector<std::pair<td::string, td::string>> args);

  void close(td::Promise<td::Unit> &&promise);

 private:
  class ClientInfo {
   public:
    ClientInfo() = default;
    ClientInfo(BotStatActor &&stat, td::string token, td::int64 tqueue_id, td::string bot_token, bool is_test_dc,
               double first_start_time)
        : stat_(std::move(stat))
        , token_(std::move(token))
        , tqueue_id_(tqueue_id)
        , bot_token_(std::move(bot_token))
        , is_test_dc_(is_test_dc)
        , first_start_time_(first_start_time) {
    }

    BotStatActor stat_;
    td::string token_;
    td::int64 tqueue_id_ = 0;
    td::ActorOwn<Client> client_;

    td::string bot_token_;
    bool is_test_dc_ = false;
    double first_start_time_ = 0.0;  // creation time of the first Client
    double start_time_ = 0.0;        // creation time of the current Client

    // graceful restart of the TDLib instance
    enum class RestartState : td::int8 { None, Draining, Closing };
    RestartState restart_state_ = RestartState::None;
    std::queue<PromisedQueryPtr> restart_queries_;  // queries received during the restart
    double restart_start_time_ = 0.0;
    double scheduled_restart_time_ = 0.0;  // time of the next periodic restart; 0 if none
    double restart_retry_time_ = 0.0;      // the restart can't be automatically retried before the time
    td::int32 restart_count_ = 0;
    td::int32 failed_restart_count_ = 0;
    bool is_restart_requested_ = false;
  };
  td::Container<ClientInfo> clients_;
  BotStatActor stat_{td::ActorId<BotStatActor>()};

  std::shared_ptr<const ClientParameters> parameters_;
  TokenRange token_range_;

  td::FlatHashMap<td::string, td::uint64> token_to_id_;
  td::FlatHashMap<td::string, td::FloodControlFast> flood_controls_;
  td::FloodControlFast global_flood_control_;
  bool is_global_flood_control_enabled_ = false;
  td::FlatHashMap<td::int64, td::uint64> active_client_count_;

  bool close_flag_ = false;
  td::vector<td::Promise<td::Unit>> close_promises_;

  td::ActorOwn<Watchdog> watchdog_id_;
  double next_tqueue_gc_time_ = 0.0;
  td::int64 tqueue_deleted_events_ = 0;
  td::int64 last_tqueue_deleted_events_ = 0;

  td::uint64 restarting_client_id_ = 0;
  double next_restart_time_ = 0.0;
  double next_memory_check_time_ = 0.0;
  double malloc_trim_time_ = 0.0;
  td::int64 total_restart_count_ = 0;
  td::int64 total_failed_restart_count_ = 0;

  static constexpr double WATCHDOG_TIMEOUT = 0.25;
  static constexpr std::size_t MAX_RESTART_QUERIES = 10000;
  static constexpr double RESTART_CHECK_PERIOD = 1.0;
  static constexpr double RESTART_MEMORY_CHECK_PERIOD = 5.0;
  static constexpr double RESTART_RETRY_DELAY = 300.0;
  static constexpr double MAX_RESTART_DURATION = 120.0;  // in addition to the drain timeout
  static constexpr double MALLOC_TRIM_DELAY = 10.0;

  static td::int64 get_tqueue_id(td::int64 user_id, bool is_test_dc);

  static PromisedQueryPtr get_webhook_restore_query(td::Slice token, td::Slice webhook_info,
                                                    std::shared_ptr<SharedData> shared_data);

  struct TopClients {
    td::int32 active_count = 0;
    td::vector<td::uint64> top_client_ids;
  };
  TopClients get_top_clients(std::size_t max_count, td::Slice token_filter);

  void create_client_actor(td::uint64 id);

  void schedule_restart_check();

  void on_restart_check_timer();

  double get_scheduled_restart_time(double now) const;

  td::int32 request_restart(td::Slice bot_id);

  void check_restarts(double now);

  void start_restart(td::uint64 id, td::Slice reason);

  void on_restart_closing(td::uint64 id, td::Result<td::Unit> result);

  void finish_restart(td::uint64 id);

  void send_restart_queries(ClientInfo *client_info);

  void fail_restart_queries(ClientInfo *client_info);

  static td::Slice get_restart_state_name(ClientInfo::RestartState state);

  void start_up() final;
  void raw_event(const td::Event::Raw &event) final;
  void timeout_expired() final;
  void hangup_shared() final;
  void close_db();
  void finish_close();
};

}  // namespace telegram_bot_api
