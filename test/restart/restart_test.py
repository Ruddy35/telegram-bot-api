#!/usr/bin/env python3
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Offline integration test of graceful TDLib instance restarts.
#
# Network access to Telegram isn't needed: the TDLib database of a test bot is marked as authorized by
# restart-test-fake-auth, and updates are added to the server TQueue by restart-test-fake-updates.
#
# Usage:
#   cmake --build <build dir> --target telegram-bot-api restart-test-fake-auth restart-test-fake-updates
#   python3 test/restart/restart_test.py <build dir> [webhook] [polling] [drain_timeout] [policy]
#
import collections
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BOT_ID = 1087968824  # @GroupAnonymousBot; TDLib knows the user without network access, so getMe works offline
TOKEN = '%d:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw' % BOT_ID
API_PORT = 18081
STAT_PORT = 18082
HOOK_PORT = 18083

BUILD_DIR = None
WORK_ROOT = None


def log(*args, **kwargs):
    print('[%.3f]' % (time.time() % 1000), *args, flush=True, **kwargs)


class Hook(BaseHTTPRequestHandler):
    delay = 0.0
    received = collections.Counter()
    api_errors = []
    lock = threading.Lock()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        update = json.loads(body)
        # like many bot frameworks, call API methods after processing of the update, but before the webhook request is
        # answered
        time.sleep(Hook.delay)
        result, _ = api('getWebhookInfo')
        with Hook.lock:
            Hook.received[update['update_id']] += 1
            if not result.get('ok'):
                Hook.api_errors.append(result)
        self.send_response(200)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def log_message(self, *args):
        pass


def api(method, timeout=60, **params):
    data = json.dumps(params).encode()
    req = urllib.request.Request('http://127.0.0.1:%d/bot%s/%s' % (API_PORT, TOKEN, method), data=data,
                                 headers={'Content-Type': 'application/json'})
    start = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            result = json.loads(r.read())
    except urllib.error.HTTPError as e:
        result = json.loads(e.read())
    except OSError as e:
        result = {'ok': False, 'description': str(e)}
    return result, time.time() - start


def stats(query=''):
    with urllib.request.urlopen('http://127.0.0.1:%d/%s' % (STAT_PORT, query), timeout=10) as r:
        text = r.read().decode()
    result = {}
    for line in text.splitlines():
        parts = line.split('\t')
        if len(parts) >= 2 and parts[0] not in result:
            result[parts[0]] = parts[1]
    return result


def prepare(name, update_count):
    work = os.path.join(WORK_ROOT, name)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(os.path.join(work, TOKEN))
    subprocess.run([os.path.join(BUILD_DIR, 'restart-test-fake-auth'), os.path.join(work, TOKEN, 'td.binlog'),
                    str(BOT_ID)], check=True, capture_output=True)
    if update_count:
        subprocess.run([os.path.join(BUILD_DIR, 'restart-test-fake-updates'), os.path.join(work, 'tqueue.binlog'),
                        str(BOT_ID), str(update_count)], check=True, capture_output=True)
    return work


def start_server(work, *extra):
    args = [os.path.join(BUILD_DIR, 'telegram-bot-api'), '--api-id=1', '--api-hash=test', '--local', '-p',
            str(API_PORT), '-s', str(STAT_PORT), '-d', work, '-l', os.path.join(work, 'log.txt'), '-v', '2'] + list(extra)
    log('start server with', ' '.join(extra))
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            stats()
            break
        except OSError:
            time.sleep(0.1)
    return proc


def stop_server(proc):
    proc.send_signal(signal.SIGTERM)
    proc.wait(60)


class Load:
    """Sends requests in several threads and records failures and latencies."""

    def __init__(self, methods, threads=8):
        self.stop = False
        self.errors = []
        self.latencies = []
        self.count = 0
        self.lock = threading.Lock()
        self.threads = [threading.Thread(target=self.run, args=(methods[i % len(methods)],), daemon=True)
                        for i in range(threads)]
        for t in self.threads:
            t.start()

    def run(self, method):
        while not self.stop:
            result, latency = api(method)
            with self.lock:
                self.count += 1
                self.latencies.append(latency)
                if not result.get('ok'):
                    self.errors.append((method, result))

    def finish(self):
        self.stop = True
        for t in self.threads:
            t.join()
        log('requests: %d, errors: %d, max latency: %.2f' % (self.count, len(self.errors), max(self.latencies)))
        assert not self.errors, self.errors[:5]
        return self


def wait_for(predicate, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError('timeout waiting for ' + what)


def restart_count():
    return int(stats().get('tdlib_restart_count', 0))


def scenario_webhook():
    """Restarts while webhook updates are being delivered and the webhook handler calls API methods before answering:
    no duplicates, no losses, no failed requests."""
    update_count = 300
    work = prepare('webhook', update_count)
    Hook.delay = 0.3
    Hook.received.clear()
    hook = ThreadingHTTPServer(('127.0.0.1', HOOK_PORT), Hook)
    threading.Thread(target=hook.serve_forever, daemon=True).start()
    proc = start_server(work, '--tdlib-restart-drain-timeout=10', '--tdlib-restart-cooldown=1')
    try:
        url = 'http://127.0.0.1:%d/hook' % HOOK_PORT
        result, _ = api('setWebhook', url=url, max_connections=10)
        assert result['ok'], result
        wait_for(lambda: sum(Hook.received.values()) >= 20, 30, 'first updates')
        load = Load(['getMe', 'getWebhookInfo'])
        for i in range(3):
            log('request restart %d after %d delivered updates' % (i + 1, sum(Hook.received.values())))
            stats('?restart=%d' % BOT_ID)
            wait_for(lambda: restart_count() >= i + 1, 30, 'restart %d' % (i + 1))
            info, _ = api('getWebhookInfo')
            assert info['result']['url'] == url, info
            time.sleep(1.5)
        wait_for(lambda: len(Hook.received) >= update_count, 120, 'all updates')
        time.sleep(2)
        load.finish()
        assert not Hook.api_errors, Hook.api_errors[:5]
        assert stats()['tdlib_failed_restart_count'] == '0', 'restart was cancelled'
        duplicates = {k: v for k, v in Hook.received.items() if v != 1}
        log('delivered %d unique updates, duplicates: %s' % (len(Hook.received), duplicates))
        assert not duplicates, duplicates
        ids = sorted(Hook.received)
        assert ids == list(range(ids[0], ids[0] + update_count)), 'lost updates'
        info, _ = api('getWebhookInfo')
        assert info['result']['pending_update_count'] == 0, info
    finally:
        stop_server(proc)
        hook.shutdown()
        hook.server_close()


def scenario_polling():
    """Restart with an active long polling request: the request is answered, next requests are served."""
    work = prepare('polling', 50)
    proc = start_server(work, '--tdlib-restart-cooldown=1')
    try:
        result, _ = api('getUpdates', limit=10)
        assert result['ok'] and len(result['result']) == 10, result
        got = [u['update_id'] for u in result['result']]
        load = Load(['getMe'], threads=4)
        result, _ = api('getUpdates', offset=got[-1] + 1, limit=100)
        got += [u['update_id'] for u in result['result']]
        offset = got[-1] + 1
        holder = {}
        t = threading.Thread(target=lambda: holder.setdefault('result', api('getUpdates', offset=offset, timeout=50)))
        t.start()
        time.sleep(1)
        stats('?restart=%d' % BOT_ID)
        wait_for(lambda: restart_count() >= 1, 30, 'restart')
        t.join(60)
        result, latency = holder['result']
        log('long polling request was answered in %.2f seconds' % latency)
        assert result['ok'] and latency < 10, result
        result, _ = api('getUpdates', offset=offset, timeout=1)
        assert result['ok'] and result['result'] == [], result
        load.finish()
        assert got == list(range(got[0], got[0] + 50)), got
    finally:
        stop_server(proc)


def scenario_drain_timeout():
    """A request, which can't be finished, prevents restart; the restart is cancelled and nothing breaks."""
    work = prepare('drain_timeout', 0)
    proc = start_server(work, '--tdlib-restart-drain-timeout=3', '--tdlib-restart-cooldown=1')
    try:
        assert api('getMe')[0]['ok']
        # the request requires network access, so it isn't finished during the test
        threading.Thread(target=lambda: api('sendMessage', timeout=120, chat_id=123456789, text='test'),
                         daemon=True).start()
        time.sleep(1)
        load = Load(['getMe'], threads=4)
        stats('?restart=%d' % BOT_ID)
        wait_for(lambda: int(stats().get('tdlib_failed_restart_count', 0)) >= 1, 30, 'failed restart')
        time.sleep(1)
        load.finish()
        assert restart_count() == 0
        assert max(load.latencies) < 6, max(load.latencies)
    finally:
        stop_server(proc)


def scenario_policy():
    """Automatic restarts because of memory limit and uptime; memory must not grow because of restarts."""
    work = prepare('memory_limit', 0)
    proc = start_server(work, '--tdlib-restart-memory-limit=1', '--tdlib-restart-min-uptime=0',
                        '--tdlib-restart-cooldown=0')
    try:
        load = Load(['getMe', 'getWebhookInfo'], threads=4)
        rss = []
        deadline = time.time() + 60
        while time.time() < deadline:
            s = stats()
            rss.append((int(s['tdlib_restart_count']), s['rss']))
            time.sleep(5)
        load.finish()
        log('restart count and RSS:', rss)
        assert rss[-1][0] >= 10, rss
    finally:
        stop_server(proc)

    work = prepare('uptime', 0)
    proc = start_server(work, '--tdlib-restart-interval=5', '--tdlib-restart-cooldown=0')
    try:
        load = Load(['getMe'], threads=2)
        time.sleep(20)
        load.finish()
        count = restart_count()
        log('restart count:', count)
        assert 2 <= count <= 4, count
    finally:
        stop_server(proc)


def main():
    global BUILD_DIR, WORK_ROOT
    if len(sys.argv) < 2:
        print(__doc__ or 'Usage: restart_test.py <build dir> [scenario]...')
        sys.exit(1)
    BUILD_DIR = os.path.abspath(sys.argv[1])
    scenarios = sys.argv[2:] or ['webhook', 'polling', 'drain_timeout', 'policy']
    WORK_ROOT = tempfile.mkdtemp(prefix='telegram-bot-api-restart-test-')
    try:
        for scenario in scenarios:
            log('run scenario', scenario)
            globals()['scenario_' + scenario]()
            log('scenario', scenario, 'PASSED')
    finally:
        shutil.rmtree(WORK_ROOT, ignore_errors=True)


if __name__ == '__main__':
    main()
