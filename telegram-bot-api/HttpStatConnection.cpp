//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/HttpStatConnection.h"

#include "td/net/HttpHeaderCreator.h"

#include "td/utils/common.h"
#include "td/utils/misc.h"
#include "td/utils/Promise.h"

namespace telegram_bot_api {

bool HttpStatConnection::is_authorized(const td::HttpQuery &http_query) const {
  if (access_token_.empty()) {
    return false;
  }
  td::Slice authorization = http_query.get_header("authorization");
  td::Slice prefix("Bearer ");
  if (!td::begins_with(authorization, prefix) || authorization.size() != prefix.size() + access_token_.size()) {
    return false;
  }
  // the comparison takes the same time for all tokens of the right size
  unsigned char difference = 0;
  for (std::size_t i = 0; i < access_token_.size(); i++) {
    difference |= static_cast<unsigned char>(authorization[prefix.size() + i] ^ access_token_[i]);
  }
  return difference == 0;
}

void HttpStatConnection::handle(td::unique_ptr<td::HttpQuery> http_query,
                                td::ActorOwn<td::HttpInboundConnection> connection) {
  CHECK(connection_.empty());
  connection_ = std::move(connection);

  if (!is_authorized(*http_query)) {
    if (access_token_.empty()) {
      return send_response(403, "text/plain",
                           td::BufferSlice("Forbidden: access is disabled, because the access token isn't set"), false);
    }
    return send_response(401, "text/plain",
                         td::BufferSlice("Unauthorized: the header \"Authorization: Bearer <token>\" is required"),
                         true);
  }

  auto promise = td::PromiseCreator::lambda([actor_id = actor_id(this)](td::Result<td::BufferSlice> result) {
    send_closure(actor_id, &HttpStatConnection::on_result, std::move(result));
  });
  is_metrics_query_ = http_query->url_path_ == "/metrics";
  if (is_metrics_query_) {
    send_closure(client_manager_, &ClientManager::get_metrics, std::move(promise), http_query->get_args());
    return;
  }
  send_closure(client_manager_, &ClientManager::get_stats, std::move(promise), http_query->get_args());
}

void HttpStatConnection::on_result(td::Result<td::BufferSlice> result) {
  if (result.is_error()) {
    send_closure(connection_.release(), &td::HttpInboundConnection::write_error,
                 td::Status::Error(500, "Internal Server Error: closing"));
    return;
  }
  send_response(200,
                is_metrics_query_ ? td::Slice("text/plain; version=0.0.4; charset=utf-8") : td::Slice("text/plain"),
                result.move_as_ok(), false);
}

void HttpStatConnection::send_response(int http_status_code, td::Slice content_type, td::BufferSlice content,
                                       bool need_authorization) {
  td::HttpHeaderCreator hc;
  hc.init_status_line(http_status_code);
  hc.set_keep_alive();
  if (need_authorization) {
    hc.add_header("WWW-Authenticate", "Bearer");
  }
  hc.set_content_type(content_type);
  hc.set_content_size(content.size());

  auto r_header = hc.finish();
  if (r_header.is_error()) {
    send_closure(connection_.release(), &td::HttpInboundConnection::write_error, r_header.move_as_error());
    return;
  }
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, std::move(content));
  send_closure(connection_.release(), &td::HttpInboundConnection::write_ok);
}

}  // namespace telegram_bot_api
