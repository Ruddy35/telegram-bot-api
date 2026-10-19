#!/bin/bash
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Starts telegram-bot-api with options from environment variables:
#  TELEGRAM_<NAME>=<value>         -> --<name>=<value>, where <name> is in lower case with '-' instead of '_';
#  TELEGRAM_<NAME>=true            -> --<name>, for options without a value, such as --local;
#  TELEGRAM_<NAME>=false or empty  -> the option isn't passed.
# TELEGRAM_API_ID, TELEGRAM_API_HASH, TELEGRAM_HTTP_STAT_TOKEN and TELEGRAM_BOT_API_* are read by the server itself,
# so the secrets don't appear in the command line. Arguments of the container are passed after the options, so they
# override them.
#
set -euo pipefail

options=()
for name in $(compgen -e); do
  case "$name" in
    TELEGRAM_API_ID | TELEGRAM_API_HASH | TELEGRAM_HTTP_STAT_TOKEN | TELEGRAM_BOT_API_*) continue ;;
    TELEGRAM_*) ;;
    *) continue ;;
  esac
  value="${!name}"
  option="${name#TELEGRAM_}"
  option="--${option,,}"
  option="${option//_/-}"
  case "$value" in
    true) options+=("$option") ;;
    false | '') ;;
    *) options+=("$option=$value") ;;
  esac
done

exec telegram-bot-api "${options[@]}" "$@"
