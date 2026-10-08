//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

// Test-only tool: marks a TDLib database as one of an authorized bot, so the TDLib instance becomes ready without
// network access. Must never be used with real bot databases.

#include "td/mtproto/AuthKey.h"

#include "td/db/binlog/Binlog.h"
#include "td/db/binlog/BinlogEvent.h"
#include "td/db/BinlogKeyValue.h"
#include "td/db/DbKey.h"

#include "td/utils/common.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/tl_helpers.h"

#include <memory>

int main(int argc, char **argv) {
  if (argc != 3) {
    LOG(PLAIN) << "Usage: " << argv[0] << " <path to td.binlog> <bot user identifier>";
    return 1;
  }

  constexpr td::int32 BINLOG_PMC_MAGIC = 0x4327;  // LogEvent::HandlerType::BinlogPmcMagic
  auto binlog = std::make_shared<td::Binlog>();
  td::BinlogKeyValue<td::Binlog> pmc;
  pmc.external_init_begin(BINLOG_PMC_MAGIC);
  binlog
      ->init(
          argv[1],
          [&](const td::BinlogEvent &event) {
            if (event.type_ == BINLOG_PMC_MAGIC) {
              pmc.external_init_handle(event);
            }
          },
          td::DbKey::raw_key("cucumber"))
      .ensure();
  pmc.external_init_finish(binlog);
  pmc.set("auth", "ok");
  pmc.set("auth_is_bot", "true");
  pmc.set("my_id", argv[2]);
  pmc.set("main_dc_id", "2");
  for (int dc_id = 1; dc_id <= 5; dc_id++) {
    td::mtproto::AuthKey auth_key(static_cast<td::uint64>(0x1234567890 + dc_id),
                                  td::string(256, static_cast<char>('a' + dc_id)));
    auth_key.set_auth_flag(true);
    pmc.set("auth" + td::to_string(dc_id), td::serialize(auth_key));
  }
  binlog->close().ensure();
  return 0;
}
