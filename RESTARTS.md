# Graceful restarts of TDLib instances

Each bot on the server is served by its own TDLib instance. The instance keeps in memory every user, chat, file and
media object the bot has ever seen, so memory usage of a long-running server grows without bound. This fork can close
and reopen the TDLib instance of one bot on the fly, which releases all its memory without restarting the process.

For the bot a restart looks like a short delay of answers: no request fails, no update is lost or sent twice, the
webhook and `allowed_updates` are kept, no new login is needed, and other bots aren't affected.

## Why memory grows

| Objects | Where | Released |
|---|---|---|
| File nodes and their indexes | TDLib `FileManager` (`file_nodes_`, `file_id_info_`, `remote_location_to_file_id_`) | Never: an integer `file_id` must stay valid for the lifetime of the instance |
| Media metadata: videos, documents, audio, animations, stickers | TDLib `VideosManager`, `DocumentsManager` and others | Never |
| Chats (`Dialog`) | TDLib `MessagesManager::dialogs_` | Never |
| Users, basic groups, supergroups and their full info | TDLib `UserManager`, `ChatManager` | Never |
| Messages | TDLib `MessagesManager` | After `message_unload_delay` (1800 seconds for bots by default) |
| Bot API cache of messages | `Client::messages_` | When TDLib deletes the messages |
| Bot API cache of users and chats | `Client::users_`, `chats_`, `groups_`, `supergroups_` | Never |

Memory of an instance grows with the number of unique users, chats and files the bot has seen since the start of the
instance. The objects are referenced from many places in TDLib, so the only correct way to release them is to destroy
the instance. The metric `tgbotapi_bot_tdlib_objects` (see [METRICS.md](METRICS.md)) shows the numbers of these objects
for each bot.

## How a restart works

A restart of a bot has three phases; `ClientManager` keeps the state of the bot, and `Client` does the work.

1. **Pausing.** `ClientManager` delays only `getUpdates` requests of the bot; all other requests are processed as
   usual. The `Client`
   * answers the active long polling `getUpdates` request with the updates, which are already in the queue;
   * pauses the webhook: no new updates are sent, and the updates, which are being sent, wait for the answer of the
     webhook, so no update is sent twice. A webhook handler can call API methods before answering, as many bot
     frameworks do;
   * waits until the bot has no active requests. The bot receives no new updates, so its work naturally finishes.
2. **Draining.** `ClientManager` delays all requests of the bot (up to 10000; further requests get
   `429 Too Many Requests: retry after 1`). The `Client` waits until all requests to TDLib are finished and all received
   updates are handled, then disables network of the instance, so no new updates are received from Telegram. After
   0.5 seconds the check is repeated; if updates arrived meanwhile, the network is enabled back and the phase
   continues. Updates, which weren't received from Telegram yet, are received by the new instance, because the instance
   stores its update state in its database.
3. **Closing.** The TDLib instance is closed, and its memory is released. Then `ClientManager` creates a new `Client`
   with the same token and database, so no new login is needed, restores the webhook from `webhooks_db.binlog`, as on
   server start, and passes it all delayed requests in their original order.

The first two phases must finish in `--tdlib-restart-drain-timeout` seconds in total, excluding the time to disable
network. Otherwise the restart is **cancelled**: the webhook and the network are resumed, delayed requests are passed
to the old instance, and the bot works as before. A restart is also cancelled if the webhook is changed during it, and
it doesn't start while `setWebhook` is being processed. If the instance is being closed for another reason (`logOut`,
`close`, revoked token, server shutdown), the restart is cancelled, and the instance is closed as without the fork.

Requests are delayed from the start of the second phase until the new instance is ready: in the tests of
[VERIFICATION.md](VERIFICATION.md#graceful-restarts) at most 0.52 seconds, most of which is the 0.5-second wait after
network is disabled; closing and opening of a big database takes longer.

Delayed requests fail with `429 Too Many Requests: retry after 1` only if a restart lasts more than
`--tdlib-restart-drain-timeout` plus 120 seconds, which doesn't happen in practice, because closing and opening an
instance takes from milliseconds to a few seconds.

## When restarts happen

Only one instance is restarted at a time, and starts of restarts are separated by at least `--tdlib-restart-cooldown`
seconds. A cancelled automatic restart is retried after 5 minutes multiplied by the number of failures in a row, but
not later than in an hour.

| Option | Default | Meaning |
|---|---|---|
| `--tdlib-restart-memory-limit=<MB>` | disabled | While resident memory of the server exceeds the limit, restart the instance, which uses the most memory, checking every 5 seconds. Memory of an instance is estimated by memory accounting (part `client` plus part `tdlib` in [METRICS.md](METRICS.md)), or by the number of updates received from TDLib if the accounting is disabled |
| `--tdlib-restart-bot-memory-limit=<MB>` | disabled | Restart the instance of a bot, which uses more memory than the limit, checking every 5 seconds. Requires memory accounting |
| `--tdlib-restart-min-uptime=<seconds>` | 3600 | Instances, which work less than this time, aren't restarted because of the memory limits. It prevents endless restarts if memory isn't released, for example, because of heap fragmentation |
| `--tdlib-restart-interval=<seconds>` | disabled | Restart each instance after it works for the specified time plus a random delay of up to 10% |
| `--tdlib-restart-drain-timeout=<seconds>` | 30 | Maximum duration of the first two phases, 1-3600 |
| `--tdlib-restart-cooldown=<seconds>` | 10 | Minimum delay between starts of restarts, including requested ones |
| `--malloc-trim` | disabled | Call `malloc_trim(0)` 10 seconds after the last restart to return free memory to the OS; only for the glibc allocator |
| `--message-unload-delay=<seconds>` | TDLib default | Delay after which unused messages are unloaded from memory of TDLib instances, 60-86400 |

Without these options the server behaves as the original server.

Example for a server with dozens of bots and a memory limit of 8 GB:

```
telegram-bot-api ... --tdlib-restart-memory-limit=5500 --tdlib-restart-bot-memory-limit=1500
```

Choose the bot memory limit above the memory, which ordinary bots use after a day of work, using
`tgbotapi_bot_memory_bytes{part=~"client|tdlib"}`. A bot, which uses more memory than the limit right after a
restart, is restarted once in `--tdlib-restart-min-uptime` seconds; such restarts are visible in
`tgbotapi_bot_tdlib_restarts_total`.

## Manual restarts and monitoring

The statistics port (`--http-stat-port`) must be accessible only from the internal network; this is already required by
the original server, because the port allows to change the verbosity level.

```
curl 'http://127.0.0.1:<stat port>/?restart=<bot_id>'   # restart the instance of the bot
curl 'http://127.0.0.1:<stat port>/?restart=all'        # restart all instances one by one
```

`<bot_id>` is the part of the token before the colon. The answer contains `tdlib_restart_requested` with the number of
requested restarts. Requested restarts are started before other restarts.

The statistics page has new lines `tdlib_restart_count` and `tdlib_failed_restart_count` for the server and
`tdlib_update_count`, `tdlib_restart_count`, `tdlib_next_restart_in` for each bot; during a restart also
`tdlib_restart_state` (`pausing`, `draining` or `closing`), `tdlib_restart_duration` and
`tdlib_restart_delayed_query_count`. `uptime` of a bot is the uptime of its current instance. Metrics of restarts are
described in [METRICS.md](METRICS.md).

Log messages with the verbosity level 2 (WARNING):

```
Start graceful restart of TDLib instance of bot <id> with uptime <seconds> because of <request|memory limit|bot memory limit|uptime>
Close TDLib instance for restart
Closed TDLib instance of bot <id> for restart in <seconds> seconds; have <N> delayed queries
Failed to restart TDLib instance of bot <id>: <reason>
Released free memory to the OS in <seconds> seconds
```

## Limitations

* A bot, which sends requests continuously regardless of updates, for example, a mailing, may not finish its requests
  in `--tdlib-restart-drain-timeout` seconds. Then the restart is cancelled and retried later. During such an attempt
  only receiving of updates is delayed, not requests.
* If a long request was received right before the second phase, requests of the bot wait for it, but not more than
  `--tdlib-restart-drain-timeout` seconds since the start of the restart.
* While an instance is being restarted, the bot receives no new updates. They are received after the restart.
* With glibc resident memory may decrease only partially after a restart because of heap fragmentation, but the freed
  memory is reused. `--malloc-trim` returns it to the OS.
* TDLib aborts with `LOG_CHECK(storage_count_ == 0)` in `ObjectPool` at the exit of the process, if thousands of network
  requests are still pending. This happens in the original server too and doesn't depend on restarts.

## Tests

`test/restart/restart_test.py` runs a real server without network access: test bots are marked as authorized in their
databases by `restart-test-fake-auth`, and updates are added to the update queue by `restart-test-fake-updates`.
Every scenario checks that the server exits with the code 0 and logs no errors. The outcomes of restarts are covered by
the scenarios as described in [VERIFICATION.md](VERIFICATION.md#graceful-restarts).

```
cmake --build <build dir> --target telegram-bot-api restart-test-fake-auth restart-test-fake-updates
python3 test/restart/restart_test.py <build dir> [scenario]...
```

The offline tests can't reproduce receiving of updates from Telegram, downloading and sending of files and
`getDifference` after reopening of an instance; check them on test bots with a real load before using restarts in
production.
