//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

// Test-only tool: adds fake message updates for a bot to tqueue.binlog of a stopped Bot API server.

#include "td/db/binlog/Binlog.h"
#include "td/db/binlog/BinlogEvent.h"
#include "td/db/TQueue.h"

#include "td/utils/common.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"

#include <ctime>
#include <memory>

int main(int argc, char **argv) {
  if (argc != 4) {
    LOG(PLAIN) << "Usage: " << argv[0] << " <path to tqueue.binlog> <bot user identifier> <update count>";
    return 1;
  }
  auto queue_id = td::to_integer<td::int64>(td::Slice(argv[2]));
  auto count = td::to_integer<int>(td::Slice(argv[3]));

  auto tqueue = td::TQueue::create();
  auto tqueue_binlog = td::make_unique<td::TQueueBinlog<td::Binlog>>();
  auto binlog = std::make_shared<td::Binlog>();
  binlog->init(argv[1], [&](const td::BinlogEvent &event) { tqueue_binlog->replay(event, *tqueue).ignore(); }).ensure();
  tqueue_binlog->set_binlog(binlog);
  tqueue->set_callback(std::move(tqueue_binlog));

  auto now = static_cast<td::int32>(std::time(nullptr));
  for (int i = 1; i <= count; i++) {
    auto data = PSTRING() << "\"message\":{\"message_id\":" << i << ",\"date\":" << now << ",\"chat\":{\"id\":" << i
                          << ",\"type\":\"private\",\"first_name\":\"Test\"},\"text\":\"test " << i << "\"}";
    tqueue->push(queue_id, data, now + 86400, 0, td::TQueue::EventId()).ensure();
  }
  binlog->close().ensure();
  return 0;
}
