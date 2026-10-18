# Image of the Bot API server, which is configured by environment variables; see the section "Docker" in README.md.
#
#   docker build -t telegram-bot-api .
#
# The submodule td must be checked out: git clone --recursive or git submodule update --init.

FROM debian:bookworm-slim AS build
RUN apt-get update \
  && apt-get install -y --no-install-recommends ca-certificates cmake g++ git gperf libssl-dev make zlib1g-dev \
  && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# compilation of TDLib needs up to 3 GB of memory per job; decrease the number of jobs if the build runs out of memory
ARG BUILD_JOBS=4
RUN test -f td/CMakeLists.txt || { echo "td is empty: run git submodule update --init"; exit 1; } \
  && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build --target telegram-bot-api -j "$BUILD_JOBS"

FROM debian:bookworm-slim
# glibc of the image is linked dynamically, which memory accounting of bots needs
RUN apt-get update \
  && apt-get install -y --no-install-recommends ca-certificates libssl3 zlib1g \
  && rm -rf /var/lib/apt/lists/* \
  && useradd --system --create-home --home-dir /var/lib/telegram-bot-api telegram-bot-api \
  && install -d -o telegram-bot-api /tmp/telegram-bot-api
COPY --from=build /src/build/telegram-bot-api /usr/local/bin/telegram-bot-api
COPY docker/entrypoint.sh /usr/local/bin/docker-entrypoint.sh
ENV TELEGRAM_DIR=/var/lib/telegram-bot-api \
    TELEGRAM_TEMP_DIR=/tmp/telegram-bot-api \
    TELEGRAM_HTTP_PORT=8081 \
    TELEGRAM_HTTP_STAT_PORT=8082
USER telegram-bot-api
VOLUME /var/lib/telegram-bot-api
# 8082 is the statistics port with /metrics; it must be accessible only from the internal network
EXPOSE 8081 8082
ENTRYPOINT ["docker-entrypoint.sh"]
