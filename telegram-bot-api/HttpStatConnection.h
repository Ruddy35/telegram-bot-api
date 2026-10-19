//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/ClientManager.h"

#include "td/net/HttpInboundConnection.h"
#include "td/net/HttpQuery.h"

#include "td/actor/actor.h"

#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"

namespace telegram_bot_api {

// Serves the statistics page and /metrics. Every request must have the header "Authorization: Bearer <access_token>";
// if the access token is empty, then all requests are denied.
class HttpStatConnection final : public td::HttpInboundConnection::Callback {
 public:
  HttpStatConnection(td::ActorId<ClientManager> client_manager, td::string access_token)
      : client_manager_(client_manager), access_token_(std::move(access_token)) {
  }

  void handle(td::unique_ptr<td::HttpQuery> http_query, td::ActorOwn<td::HttpInboundConnection> connection) final;

 private:
  td::ActorId<ClientManager> client_manager_;
  td::string access_token_;
  td::ActorOwn<td::HttpInboundConnection> connection_;
  bool is_metrics_query_ = false;

  bool is_authorized(const td::HttpQuery &http_query) const;

  void on_result(td::Result<td::BufferSlice> result);

  void send_response(int http_status_code, td::Slice content_type, td::BufferSlice content, bool need_authorization);

  void hangup() final {
    connection_.release();
    stop();
  }
};

}  // namespace telegram_bot_api
