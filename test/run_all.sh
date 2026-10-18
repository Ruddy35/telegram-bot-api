#!/bin/sh
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Runs all tests of the changes of the fork; see VERIFICATION.md. Exits with a non-zero code on the first failure.
#
# Usage: test/run_all.sh <build dir>
#
set -e
if [ $# -ne 1 ]; then
  echo "Usage: $0 <build dir>"
  exit 2
fi
build=$(cd "$1" && pwd)
cd "$(dirname "$0")/.."

cmake --build "$build" --target telegram-bot-api heap-sampler-test restart-test-fake-auth restart-test-fake-updates

echo "=== memory accounting with glibc"
"$build/heap-sampler-test"
echo "=== memory accounting with glibc in the audit mode"
TELEGRAM_BOT_API_MEMORY_AUDIT=1 "$build/heap-sampler-test"
jemalloc=$(find /usr/lib /usr/lib64 /usr/local/lib -name 'libjemalloc.so.2' 2>/dev/null | head -n 1)
if [ -n "$jemalloc" ]; then
  echo "=== memory accounting with jemalloc"
  LD_PRELOAD="$jemalloc" "$build/heap-sampler-test"
  echo "=== memory accounting with jemalloc in the audit mode"
  TELEGRAM_BOT_API_MEMORY_AUDIT=1 LD_PRELOAD="$jemalloc" "$build/heap-sampler-test"
else
  echo "=== SKIPPED: memory accounting with jemalloc, because libjemalloc.so.2 isn't found"
fi

echo "=== verification tools and documentation"
python3 test/verify/test_tools.py
python3 test/test_docs.py

echo "=== server"
python3 test/restart/restart_test.py "$build"

echo "=== entrypoint of the Docker image; test the image itself with: test/test_docker.py <build dir> --image <image>"
python3 test/test_docker.py "$build"

echo "ALL TESTS PASSED"
