# Telegram Bot API

The Telegram Bot API provides an HTTP API for creating [Telegram Bots](https://core.telegram.org/bots).

If you've got any questions about bots or would like to report an issue with your bot, kindly contact us at [@BotSupport](https://t.me/BotSupport) in Telegram.

Please note that only global Bot API issues that affect all bots are suitable for this repository.

## Table of Contents
- [Installation](#installation)
- [Dependencies](#dependencies)
- [Usage](#usage)
- [Documentation](#documentation)
- [Moving a bot to a local server](#switching)
- [Moving a bot from one local server to another](#moving)
- [Changes of this fork](#fork)
- [License](#license)

<a name="installation"></a>
## Installation

The simplest way to build and install `Telegram Bot API server` is to use our [Telegram Bot API server build instructions generator](https://tdlib.github.io/telegram-bot-api/build.html).
If you do that, you'll only need to choose the target operating system to receive the complete build instructions.

In general, you need to install all `Telegram Bot API server` [dependencies](#dependencies) and compile the source code using CMake:

```sh
git clone --recursive https://github.com/tdlib/telegram-bot-api.git
cd telegram-bot-api
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --target install
```

<a name="dependencies"></a>
## Dependencies
To build and run `Telegram Bot API server` you will need:

* OpenSSL
* zlib
* C++17 compatible compiler (e.g., Clang 5.0+, GCC 7.0+, MSVC 19.1+ (Visual Studio 2017.7+), Intel C++ Compiler 19+) (build only)
* gperf (build only)
* CMake (3.10+, build only)

<a name="usage"></a>
## Usage

Use `telegram-bot-api --help` to receive the list of all available options of the Telegram Bot API server.

The only mandatory options are `--api-id` and `--api-hash`. You must obtain your own `api_id` and `api_hash`
as described in https://core.telegram.org/api/obtaining_api_id and specify them using the `--api-id` and `--api-hash` options
or the `TELEGRAM_API_ID` and `TELEGRAM_API_HASH` environment variables.

To enable Bot API features not available at `https://api.telegram.org`, specify the option `--local`. In the local mode the Bot API server allows to:
* Download files without a size limit.
* Upload files up to 2000 MB.
* Upload files using their local path and [the file URI scheme](https://en.wikipedia.org/wiki/File_URI_scheme).
* Use an HTTP URL for the webhook.
* Use any local IP address for the webhook.
* Use any port for the webhook.
* Set *max_webhook_connections* up to 100000.
* Receive the absolute local path as a value of the *file_path* field without the need to download the file after a *getFile* request.

The Telegram Bot API server accepts only HTTP requests, so a TLS termination proxy needs to be used to handle remote HTTPS requests.

By default the Telegram Bot API server is launched on the port 8081, which can be changed using the option `--http-port`.

<a name="documentation"></a>
## Documentation
See [Bots: An introduction for developers](https://core.telegram.org/bots) for a brief description of Telegram Bots and their features.

See the [Telegram Bot API documentation](https://core.telegram.org/bots/api) for a description of the Bot API interface and a complete list of available classes, methods and updates.

See the [Telegram Bot API server build instructions generator](https://tdlib.github.io/telegram-bot-api/build.html) for detailed instructions on how to build the Telegram Bot API server.

Subscribe to [@BotNews](https://t.me/botnews) to be the first to know about the latest updates and join the discussion in [@BotTalk](https://t.me/bottalk).

<a name="switching"></a>
## Moving a bot to a local server

To guarantee that your bot will receive all updates, you must deregister it with the `https://api.telegram.org` server by calling the method [logOut](https://core.telegram.org/bots/api#logout).
After the bot is logged out, you can replace the address to which the bot sends requests with the address of your local server and use it in the usual way.
If the server is launched in `--local` mode, make sure that the bot can correctly handle absolute file paths in response to `getFile` requests.

<a name="moving"></a>
## Moving a bot from one local server to another

If the bot is logged in on more than one server simultaneously, there is no guarantee that it will receive all updates.
To move a bot from one local server to another you can use the method [logOut](https://core.telegram.org/bots/api#logout) to log out on the old server before switching to the new one.

If you want to avoid losing updates between logging out on the old server and launching on the new server, you can remove the bot's webhook using the method
[deleteWebhook](https://core.telegram.org/bots/api#deletewebhook), then use the method [close](https://core.telegram.org/bots/api#close) to close the bot instance.
After the instance is closed, locate the bot's subdirectory in the working directory of the old server by the bot's user ID, move the subdirectory to the working directory of the new server
and continue sending requests to the new server as usual.

<a name="fork"></a>
## Changes of this fork

* **Graceful restarts of TDLib instances.** Memory of the TDLib instance of a bot grows with every user, chat and file
  the bot has seen. The server can close and reopen the instance of a bot on the fly, which releases its memory, without
  failing requests, losing or duplicating updates or affecting other bots. See [RESTARTS.md](RESTARTS.md).
* **Metrics.** `GET http://<stat address>:<stat port>/metrics` returns metrics of every bot and of the server in the
  Prometheus text format: requests, updates, webhook deliveries, files, CPU time, memory, disk usage, objects in memory
  of TDLib instances and restarts. See [METRICS.md](METRICS.md).
* **CPU and memory accounting by bots.** CPU time of each bot is estimated by sampling with `SIGPROF`; memory of each
  bot is estimated by sampling of memory allocations with a reported standard deviation. See [METRICS.md](METRICS.md).
* **Access token of the statistics port.** The statistics page and `/metrics` are accessible only with the token
  `--http-stat-token`; without it all requests are denied. See [Security](#security).
* **Verification.** [VERIFICATION.md](VERIFICATION.md) lists the claims about the changes, how they are proven and
  tested, and how to repeat the checks; `test/run_all.sh <build dir>` runs all tests.

| Option | Default | Meaning |
|---|---|---|
| `--tdlib-restart-memory-limit-mib=<MiB>` | disabled | Restart the instance using the most memory while resident memory of the server exceeds the limit |
| `--tdlib-restart-bot-memory-limit-mib=<MiB>` | disabled | Restart the instance of a bot, which uses more memory than the limit |
| `--tdlib-restart-min-uptime-seconds=<seconds>` | 3600 | Minimum uptime of an instance before a restart because of a memory limit |
| `--tdlib-restart-interval-seconds=<seconds>` | disabled | Restart each instance after the uptime |
| `--tdlib-restart-drain-timeout-seconds=<seconds>` | 30 | Maximum time to wait for active requests of a bot before a restart is cancelled |
| `--tdlib-restart-cooldown-seconds=<seconds>` | 10 | Minimum delay between starts of restarts |
| `--malloc-trim` | disabled | Return free memory to the OS after restarts; only for glibc |
| `--message-unload-delay-seconds=<seconds>` | TDLib default | Delay after which unused messages are unloaded from memory of TDLib instances |
| `--cpu-sampling-rate-hz=<Hz>` | 100 | Samples per second of CPU time of a thread; 0 disables CPU accounting |
| `--memory-sampling-interval-kib=<KiB>` | 64 | Mean size of allocated memory between samples; 0 disables memory accounting |
| `--http-stat-token=<token>` | `TELEGRAM_HTTP_STAT_TOKEN` or empty | Access token of the statistics port, at least 16 characters; all requests are denied if it is empty |

The name of every option with a quantity ends with its unit: `-seconds`, `-mib` (1 MiB = 1048576 bytes), `-kib`
(1 KiB = 1024 bytes) or `-hz` (samples per second). The same holds for environment variables of the Docker image and
for lines added to the statistics page; metrics have their units in their names too, for example, `_bytes` or
`_seconds`. Options of the original server keep their names.

`GET http://<stat address>:<stat port>/?restart=<bot_id>|all` requests a restart manually.

Memory accounting replaces memory allocation functions and works only on Linux with dynamically linked glibc; build with
`-DTELEGRAM_BOT_API_HEAP_SAMPLING=OFF` to keep the original functions. CPU accounting works only on Linux. Numbers of
objects in memory of TDLib instances require a small patch of TDLib from [td-patches](td-patches/README.md), which is
applied by CMake automatically, so `git` is needed for the build.

<a name="security"></a>
### Security

Check every port and path that is opened to the internet: the server, its log and its data contain tokens of bots and
other secrets, and an opened port or directory leaks them. Open only the API port and only through a TLS proxy.

* **The statistics port** (`--http-stat-port`) must never be opened to the internet. The page `/` shows tokens of bots
  and URLs of their webhooks, `/metrics` shows identifiers and usernames of bots and their load, `/?v=<level>` changes
  the verbosity level of the log and `/?restart=` restarts TDLib instances. Every request must have the header
  `Authorization: Bearer <token>` with the token `--http-stat-token` (environment variable `TELEGRAM_HTTP_STAT_TOKEN`).
  The token is empty by default, so all requests are denied with 403 until it is set; generate it, for example, with
  `openssl rand -hex 32`. Wrong or missing tokens are rejected with 401. The token is sent in plain text, so it doesn't
  replace network isolation: publish the port only to the internal network, for example, to `127.0.0.1` or to the
  network of Prometheus.
* **The API port** (`--http-port`) accepts plain HTTP, so tokens of bots and contents of requests must reach it only
  through a TLS termination proxy or the internal network. The server works with any valid bot token sent to it, and
  in the local mode (`--local`) a request with any bot token, for example, of a just created bot, can send to Telegram
  any file the server can read (`file://` URIs), receives absolute paths of files and can set a webhook to any local IP
  address, so the server sends requests to the internal network. Don't open the API port of a server in the local mode
  to the internet.
* **The log** contains tokens: log lines of a bot have its token in the name of its actor; at the verbosity level 3
  and higher requests and answers are logged with their contents, and on a crash the server writes the last 1 MiB of
  messages of the level `--memory-verbosity` (3 by default) to the log. Store and ship the log as a secret.
* **The working directory** (`--dir`) contains directories named by tokens of bots, databases of TDLib instances with
  messages and files, and `webhooks_db.binlog` with webhook URLs and secret tokens. Never serve it or `--temp-dir` by
  an HTTP server, and keep them readable only by the server.

### Docker

The image is configured by environment variables: `TELEGRAM_<NAME>=<value>` becomes the option `--<name>=<value>`,
where `<name>` is in lower case with `-` instead of `_`; the value `true` passes an option without a value, such as
`--local`, and `false` or an empty value omits the option. `TELEGRAM_API_ID` and `TELEGRAM_API_HASH` are required; the
server reads them and `TELEGRAM_HTTP_STAT_TOKEN` itself, so the secrets don't appear in the command line. Arguments after the image name are passed
after these options and override them.

```
git clone --recursive https://github.com/Ruddy35/telegram-bot-api.git
cd telegram-bot-api
docker build -t telegram-bot-api .
docker run -d --name telegram-bot-api --stop-timeout 60 \
  -e TELEGRAM_API_ID=<api_id> -e TELEGRAM_API_HASH=<api_hash> -e TELEGRAM_LOCAL=true \
  -e TELEGRAM_TDLIB_RESTART_MEMORY_LIMIT_MIB=5500 -e TELEGRAM_TDLIB_RESTART_BOT_MEMORY_LIMIT_MIB=1500 \
  -e TELEGRAM_HTTP_STAT_TOKEN=<token> \
  -p 8081:8081 -p 127.0.0.1:8082:8082 -v telegram-bot-api:/var/lib/telegram-bot-api telegram-bot-api
```

The same with Docker Compose:

```yaml
services:
  telegram-bot-api:
    build: .
    restart: unless-stopped
    stop_grace_period: 60s
    environment:
      TELEGRAM_API_ID: <api_id>
      TELEGRAM_API_HASH: <api_hash>
      TELEGRAM_LOCAL: "true"
      TELEGRAM_TDLIB_RESTART_MEMORY_LIMIT_MIB: 5500
      TELEGRAM_TDLIB_RESTART_BOT_MEMORY_LIMIT_MIB: 1500
      TELEGRAM_HTTP_STAT_TOKEN: <token>
    ports:
      - "8081:8081"
      - "127.0.0.1:8082:8082"
    volumes:
      - telegram-bot-api:/var/lib/telegram-bot-api
volumes:
  telegram-bot-api:
```

* The workflow `.github/workflows/docker.yml` builds the image on every push and publishes it to the GitHub Container
  Registry for the branch `master` as `ghcr.io/ruddy35/telegram-bot-api:latest` and for tags `v*` with the tag name;
  every published image is also tagged `sha-<commit>`. Use `image: ghcr.io/ruddy35/telegram-bot-api:latest` instead of
  `build: .` to run the published image.
* The image sets `TELEGRAM_DIR=/var/lib/telegram-bot-api`, `TELEGRAM_TEMP_DIR=/tmp/telegram-bot-api`,
  `TELEGRAM_HTTP_PORT=8081` and `TELEGRAM_HTTP_STAT_PORT=8082`. Data of bots is kept in the volume.
* The port 8082 serves statistics and `/metrics` and allows to restart TDLib instances and to change the verbosity
  level, so publish it only to the internal network, for example, as `127.0.0.1:8082:8082`, and set
  `TELEGRAM_HTTP_STAT_TOKEN`; without it all requests to the port are denied. For example,
  `curl -H "Authorization: Bearer <token>" http://127.0.0.1:8082/metrics`. See [Security](#security).
* Environment variables of a container, including the API hash and the token, are shown by `docker inspect` to
  everyone who has access to the Docker daemon.
* The server runs as the user `telegram-bot-api`, so a mounted host directory must be writable by it.
* The server needs time to close TDLib instances on stop, hence `--stop-timeout 60` and `stop_grace_period: 60s`.
* The log is written to the standard error stream, which `docker logs` shows. By default it contains only errors;
  `TELEGRAM_VERBOSITY=2` adds warnings, including messages about restarts of TDLib instances.
* Building of the image takes about 20 minutes on 4 cores and needs up to 3 GB of memory per compilation job;
  `--build-arg BUILD_JOBS=2` decreases the number of jobs from 4.

<a name="license"></a>
## License
`Telegram Bot API server` source code is licensed under the terms of the Boost Software License. See [LICENSE_1_0.txt](http://www.boost.org/LICENSE_1_0.txt) for more information.
