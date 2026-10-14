#!/usr/bin/env python3
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
"""Offline integration tests of graceful restarts of TDLib instances, restart policies, options and per-bot metrics.

Network access isn't needed: the TDLib databases of test bots are marked as authorized by restart-test-fake-auth, and
updates are added to the update queue of the server by restart-test-fake-updates. Every scenario also checks that the
server exits with the code 0 and logs no errors, and every metrics page is checked by testlib.check_metrics_page.

Usage:
  cmake --build <build dir> --target telegram-bot-api restart-test-fake-auth restart-test-fake-updates
  python3 test/restart/restart_test.py <build dir> [scenario]...
"""
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
import traceback
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import testlib  # noqa: E402
from testlib import BOT2_ID, BOT_ID, TOKEN, TOKEN2, metric_sum, wait_for  # noqa: E402

BUILD_DIR = None
WORK_ROOT = None


def log(*args):
    print('[%.3f]' % (time.time() % 1000), *args, flush=True)


class TestServer(testlib.Server):
    """A server with test bots, which counts answered requests of the first bot and checks its exit code and log."""

    def __init__(self, name, args=(), update_count=0, second_bot=False, extra_bots=(), expected_errors=()):
        self.name = name
        work = os.path.join(WORK_ROOT, name)
        shutil.rmtree(work, ignore_errors=True)
        bots = (testlib.BOTS if second_bot else testlib.BOTS[:1]) + list(extra_bots)
        testlib.prepare_work_dir(BUILD_DIR, work, bots, update_count)
        super().__init__(os.path.join(BUILD_DIR, 'telegram-bot-api'), work, args)
        self.expected_errors = expected_errors
        self.calls = collections.Counter()  # answered requests of the first bot by (method, HTTP status code)
        self.lock = threading.Lock()

    def __enter__(self):
        log('start server %s %s' % (self.name, ' '.join(self.args)))
        return super().__enter__()

    def __exit__(self, *exc):
        super().__exit__(*exc)
        if exc[0] is None:
            assert self.proc.returncode == 0, (self.name, 'exit code %d' % self.proc.returncode,
                                               [line for line in self.log_lines() if line.startswith('[ 0]')])
            errors = [line for line in self.log_lines() if line.startswith(('[ 0]', '[ 1]')) and
                      not any(expected in line for expected in self.expected_errors)]
            assert not errors, errors[:5]

    def api(self, method, token=TOKEN, http_timeout=60, **params):
        result, latency = super().api(method, token, http_timeout, **params)
        if token == TOKEN and (result.get('ok') or 'error_code' in result):
            # the server counts requests to non-existing methods as requests to the method "unknown"
            stat_method = 'unknown' if method == 'unknownMethod' else method.lower()
            with self.lock:
                self.calls[(stat_method, 200 if result.get('ok') else result['error_code'])] += 1
        return result, latency

    def log_lines(self):
        with open(os.path.join(self.work, 'log.txt'), errors='replace') as f:
            return f.read().splitlines()

    def restart_count(self):
        return int(self.stats().get('tdlib_restart_count', 0))

    def failed_restart_count(self):
        return int(self.stats().get('tdlib_failed_restart_count', 0))

    def restart_starts(self):
        """Returns (time, bot identifier, uptime) of every started restart from the log."""
        result = []
        for line in self.log_lines():
            if 'Start graceful restart of TDLib instance of bot' in line:
                words = line.split('\t')[-1].split()
                # "Start graceful restart of TDLib instance of bot <id> with uptime <seconds> because of <reason>"
                result.append((float(line.split('][')[2]), int(words[8]), float(words[11])))
        return result

    def request_restart(self, bot_id=BOT_ID):
        return int(self.stats('?restart=%s' % bot_id)['tdlib_restart_requested'])

    def wait_restart_start(self):
        wait_for(lambda: metric_sum(self.metrics(), 'tgbotapi_bot_tdlib_restart_in_progress') >= 1, 30, 'restart')

    def check_request_metrics(self):
        """Checks that the server counted exactly the requests of the first bot answered to the test."""
        values = self.metrics()
        with self.lock:
            calls = dict(self.calls)
        for (method, code), count in sorted(calls.items()):
            counted = metric_sum(values, 'tgbotapi_bot_requests_total', bot=BOT_ID, method=method, code=code)
            assert counted == count, (method, code, count, counted)
        total = metric_sum(values, 'tgbotapi_bot_requests_total', bot=BOT_ID)
        assert total == sum(calls.values()), (total, sum(calls.values()))
        log('metrics: %d answered requests are counted exactly' % total)
        return values


class Load:
    """Sends requests in several threads and records failures and latencies."""

    def __init__(self, server, methods, threads=8):
        self.server = server
        self.stop = False
        self.errors = []
        self.latencies = []
        self.lock = threading.Lock()
        self.threads = [threading.Thread(target=self.run, args=(methods[i % len(methods)],), daemon=True)
                        for i in range(threads)]
        for t in self.threads:
            t.start()

    def run(self, method):
        while not self.stop:
            result, latency = self.server.api(method)
            with self.lock:
                self.latencies.append(latency)
                if not result.get('ok'):
                    self.errors.append((method, result))

    def finish(self):
        self.stop = True
        for t in self.threads:
            t.join()
        log('requests: %d, errors: %d, max latency: %.2f' % (len(self.latencies), len(self.errors),
                                                             max(self.latencies)))
        assert not self.errors, self.errors[:5]


class Hook:
    """A webhook, which answers after a delay; optionally calls API methods before answering, like many bot frameworks
    do. First requests are failed as specified by failures: an HTTP status code or None to close the connection."""

    def __init__(self, delay=0.0, server=None, failures=()):
        hook = self
        self.delay = delay
        self.server = server
        self.failures = list(failures)
        self.received = collections.Counter()  # update identifier -> number of deliveries
        self.secret_tokens = set()
        self.api_errors = []
        self.lock = threading.Lock()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'  # keep connections alive, so they are closed only on purpose

            def do_POST(self):
                update = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
                time.sleep(hook.delay)
                if hook.server is not None:
                    result, _ = hook.server.api('getWebhookInfo')
                    if not result.get('ok'):
                        hook.api_errors.append(result)
                with hook.lock:
                    hook.secret_tokens.add(self.headers.get('X-Telegram-Bot-Api-Secret-Token'))
                    status = hook.failures.pop(0) if hook.failures else 200
                    if status == 200:
                        hook.received[update['update_id']] += 1
                if status is None:
                    self.close_connection = True
                    return
                self.send_response(status)
                self.send_header('Content-Length', '0')
                self.end_headers()

            def log_message(self, *args):
                pass

        self.http_server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.url = 'http://127.0.0.1:%d/hook' % self.http_server.server_address[1]
        threading.Thread(target=self.http_server.serve_forever, daemon=True).start()

    def close(self):
        self.http_server.shutdown()
        self.http_server.server_close()


def call_in_thread(function, *args, **kwargs):
    """Calls the function in a thread; returns the thread and a dict, which will contain the result."""
    holder = {}
    thread = threading.Thread(target=lambda: holder.setdefault('result', function(*args, **kwargs)), daemon=True)
    thread.start()
    return thread, holder


def start_hanging_request(server):
    """Sends a request, which can't be answered without network access, so it is active during the test."""
    call_in_thread(server.api, 'sendMessage', http_timeout=300, chat_id=123456789, text='test')
    time.sleep(1)


def scenario_webhook():
    """Restarts while webhook updates are being delivered and the webhook handler calls API methods before answering:
    no duplicates, no losses, no failed requests; parameters of the webhook are kept."""
    update_count = 300
    hook = Hook(delay=0.3)
    try:
        with TestServer('webhook', ['--tdlib-restart-drain-timeout=10', '--tdlib-restart-cooldown=1'],
                        update_count) as server:
            hook.server = server
            result, _ = server.api('setWebhook', url=hook.url, max_connections=10, secret_token='secret',
                                   allowed_updates=['message'])
            assert result['ok'], result
            info = server.api('getWebhookInfo')[0]['result']
            wait_for(lambda: sum(hook.received.values()) >= 20, 30, 'first updates')
            load = Load(server, ['getMe', 'getWebhookInfo'])
            for i in range(3):
                log('request restart %d after %d delivered updates' % (i + 1, sum(hook.received.values())))
                assert server.request_restart() == 1
                wait_for(lambda: server.restart_count() >= i + 1, 30, 'restart %d' % (i + 1))
                new_info = server.api('getWebhookInfo')[0]['result']
                for key in ['url', 'max_connections', 'allowed_updates']:
                    assert new_info.get(key) == info.get(key), (key, info, new_info)
                time.sleep(1.5)
            wait_for(lambda: len(hook.received) >= update_count, 120, 'all updates')
            time.sleep(2)
            load.finish()
            assert not hook.api_errors, hook.api_errors[:5]
            assert hook.secret_tokens == {'secret'}, hook.secret_tokens
            assert server.failed_restart_count() == 0, 'restart was cancelled'
            duplicates = {k: v for k, v in hook.received.items() if v != 1}
            log('delivered %d unique updates, duplicates: %s' % (len(hook.received), duplicates))
            assert not duplicates, duplicates
            ids = sorted(hook.received)
            assert ids == list(range(ids[0], ids[0] + update_count)), 'lost updates'
            assert server.api('getWebhookInfo')[0]['result']['pending_update_count'] == 0
            time.sleep(0.5)  # wait for the last answers to be counted
            values = server.check_request_metrics()
            assert metric_sum(values, 'tgbotapi_bot_webhook_deliveries_total', result='ok') == update_count
            assert metric_sum(values, 'tgbotapi_bot_tdlib_restarts_total') == 3
            assert metric_sum(values, 'tgbotapi_tdlib_restarts_total') == 3
            assert metric_sum(values, 'tgbotapi_tdlib_failed_restarts_total') == 0
    finally:
        hook.close()


def scenario_polling():
    """Restart with an active long polling request: the request is answered, next requests are served."""
    with TestServer('polling', ['--tdlib-restart-cooldown=1'], 50) as server:
        result, _ = server.api('getUpdates', limit=10)
        assert result['ok'] and len(result['result']) == 10, result
        got = [u['update_id'] for u in result['result']]
        load = Load(server, ['getMe'], threads=4)
        result, _ = server.api('getUpdates', offset=got[-1] + 1, limit=100)
        got += [u['update_id'] for u in result['result']]
        offset = got[-1] + 1
        thread, holder = call_in_thread(server.api, 'getUpdates', offset=offset, timeout=50)
        time.sleep(1)
        server.request_restart()
        wait_for(lambda: server.restart_count() >= 1, 30, 'restart')
        thread.join(60)
        result, latency = holder['result']
        log('long polling request was answered in %.2f seconds' % latency)
        assert result['ok'] and latency < 10, result
        result, _ = server.api('getUpdates', offset=offset, timeout=1)
        assert result['ok'] and result['result'] == [], result
        load.finish()
        assert got == list(range(got[0], got[0] + 50)), got
        time.sleep(0.5)
        values = server.check_request_metrics()
        assert metric_sum(values, 'tgbotapi_bot_fetched_updates_total') == 50
        assert metric_sum(values, 'tgbotapi_bot_tdlib_restarts_total') == 1


def scenario_polling_pending():
    """Updates, which weren't confirmed before a restart, and getUpdates requests delayed by the restart are served by
    the new instance: every update is received exactly once and in order."""
    with TestServer('polling_pending', ['--tdlib-restart-cooldown=1'], 50) as server:
        result, _ = server.api('getUpdates', limit=10)  # the updates aren't confirmed
        first = result['result'][0]['update_id']
        received = []
        stop = threading.Event()

        def poll():
            offset = first
            while not stop.is_set():
                result, _ = server.api('getUpdates', offset=offset, limit=2, timeout=1)
                assert result['ok'], result
                received.extend(u['update_id'] for u in result['result'])
                if result['result']:
                    offset = result['result'][-1]['update_id'] + 1
                time.sleep(0.2)

        thread = threading.Thread(target=poll, daemon=True)
        thread.start()
        time.sleep(0.5)
        server.request_restart()
        wait_for(lambda: server.restart_count() >= 1, 30, 'restart')
        restart_received = len(received)
        wait_for(lambda: len(received) >= 50, 60, 'all updates')
        stop.set()
        thread.join(10)
        log('%d of 50 updates were received before the end of the restart' % restart_received)
        assert restart_received < 50, 'the restart was too late'
        assert received == list(range(first, first + 50)), received
        assert server.failed_restart_count() == 0
        time.sleep(0.5)
        server.check_request_metrics()


def scenario_drain_timeout():
    """A request, which can't be finished, prevents a restart; the restart is cancelled and nothing breaks."""
    with TestServer('drain_timeout', ['--tdlib-restart-drain-timeout=3', '--tdlib-restart-cooldown=1']) as server:
        assert server.api('getMe')[0]['ok']
        start_hanging_request(server)
        load = Load(server, ['getMe'], threads=4)
        server.request_restart()
        wait_for(lambda: server.failed_restart_count() >= 1, 30, 'failed restart')
        time.sleep(1)
        load.finish()
        assert server.restart_count() == 0
        assert max(load.latencies) < 6, max(load.latencies)
        time.sleep(0.5)  # wait for the last answers to be counted
        assert metric_sum(server.check_request_metrics(), 'tgbotapi_tdlib_failed_restarts_total') == 1
        assert 'active requests weren\'t finished in time' in '\n'.join(server.log_lines())


def scenario_cancel_with_getupdates():
    """A getUpdates request delayed by a restart, which is cancelled, is answered by the old instance."""
    with TestServer('cancel_with_getupdates', ['--tdlib-restart-drain-timeout=3', '--tdlib-restart-cooldown=1'],
                    3) as server:
        assert server.api('getMe')[0]['ok']
        start_hanging_request(server)
        server.request_restart()
        server.wait_restart_start()
        result, latency = server.api('getUpdates', timeout=0)
        log('delayed getUpdates was answered in %.2f seconds' % latency)
        assert result['ok'] and len(result['result']) == 3, result
        assert latency > 0.5, 'the request wasn\'t delayed'
        wait_for(lambda: server.failed_restart_count() >= 1, 30, 'failed restart')
        assert server.restart_count() == 0
        server.check_request_metrics()


def scenario_webhook_change():
    """A change of the webhook during a restart cancels the restart; updates aren't delivered twice."""
    hook = Hook(delay=3)
    try:
        with TestServer('webhook_change', ['--tdlib-restart-drain-timeout=20', '--tdlib-restart-cooldown=1'],
                        5) as server:
            assert server.api('setWebhook', url=hook.url, max_connections=1)[0]['ok']
            # the webhook answers in 3 seconds, so the restart waits for the update being sent
            wait_for(lambda: metric_sum(server.metrics(), 'tgbotapi_webhook_connections') >= 1, 30, 'connection')
            server.request_restart()
            server.wait_restart_start()
            result, _ = server.api('deleteWebhook')
            assert result['ok'], result
            wait_for(lambda: server.failed_restart_count() >= 1, 30, 'failed restart')
            assert server.restart_count() == 0
            assert 'the webhook was changed' in '\n'.join(server.log_lines())
            time.sleep(3.5)
            assert all(count == 1 for count in hook.received.values()), hook.received
            # the updates, which weren't delivered, are returned by getUpdates
            result, _ = server.api('getUpdates')
            assert result['ok'] and len(result['result']) + len(hook.received) >= 5, (result, hook.received)
            server.check_request_metrics()
    finally:
        hook.close()


def scenario_close_during_restart():
    """The method close during a restart cancels the restart; delayed requests are answered."""
    with TestServer('close_during_restart', ['--tdlib-restart-drain-timeout=20', '--tdlib-restart-cooldown=1'],
                    3) as server:
        assert server.api('getMe')[0]['ok']
        start_hanging_request(server)
        server.request_restart()
        server.wait_restart_start()
        thread, holder = call_in_thread(server.api, 'getUpdates', timeout=0)
        time.sleep(0.5)
        result, _ = server.api('close')
        assert result['ok'], result
        thread.join(15)
        assert 'error_code' in holder['result'][0] or holder['result'][0]['ok'], holder['result']
        wait_for(lambda: server.failed_restart_count() >= 1, 30, 'failed restart')
        assert server.restart_count() == 0
        # the bot can be used again
        wait_for(lambda: server.api('getMe')[0]['ok'], 30, 'getMe after close')


def scenario_shutdown_during_restart():
    """The server is stopped during a restart: it exits in time and delayed requests are answered."""
    with TestServer('shutdown_during_restart', ['--tdlib-restart-drain-timeout=20', '--tdlib-restart-cooldown=1'],
                    3) as server:
        assert server.api('getMe')[0]['ok']
        start_hanging_request(server)
        server.request_restart()
        server.wait_restart_start()
        thread, holder = call_in_thread(server.api, 'getUpdates', timeout=0)
        time.sleep(0.5)
        start_time = time.time()
        assert server.stop() == 0
        log('the server was stopped in %.2f seconds' % (time.time() - start_time))
        assert time.time() - start_time < 15
        thread.join(15)
        assert not thread.is_alive(), 'the delayed request hangs'


def scenario_restart_all():
    """All bots are restarted one by one on request; only one restart is done at a time."""
    with TestServer('restart_all', ['--tdlib-restart-cooldown=1'], second_bot=True) as server:
        assert server.api('getMe')[0]['ok'] and server.api('getMe', token=TOKEN2)[0]['ok']
        assert server.request_restart(123) == 0, 'unknown bot'
        assert server.request_restart('all') == 2
        wait_for(lambda: server.restart_count() >= 2, 60, 'restarts')
        time.sleep(3)
        assert server.restart_count() == 2
        values = server.metrics()
        for bot_id in [BOT_ID, BOT2_ID]:
            assert metric_sum(values, 'tgbotapi_bot_tdlib_restarts_total', bot=bot_id) == 1
        events = [line for line in server.log_lines()
                  if 'Start graceful restart of TDLib instance of bot' in line or 'for restart in' in line]
        assert len(events) == 4 and ['Start' in line for line in events] == [True, False, True, False], events
        assert server.api('getMe')[0]['ok'] and server.api('getMe', token=TOKEN2)[0]['ok']
        server.check_request_metrics()


def scenario_policy():
    """Automatic restarts because of the memory limit and the uptime; restarts don't leak memory."""
    with TestServer('memory_limit', ['--tdlib-restart-memory-limit=1', '--tdlib-restart-min-uptime=0',
                                     '--tdlib-restart-cooldown=0']) as server:
        load = Load(server, ['getMe', 'getWebhookInfo'], threads=4)
        wait_for(lambda: server.restart_count() >= 2, 30, 'restarts')
        first_heap = metric_sum(server.metrics(), 'tgbotapi_heap_bytes')
        wait_for(lambda: server.restart_count() >= 10, 60, 'restarts')
        last_heap = metric_sum(server.metrics(), 'tgbotapi_heap_bytes')
        load.finish()
        time.sleep(0.5)  # wait for the last answers to be counted
        server.check_request_metrics()
        log('heap after 2 restarts %.2f MB, after 10 restarts %.2f MB' % (first_heap / 1e6, last_heap / 1e6))
        assert last_heap < 1.1 * first_heap, 'memory leak'
        assert 'because of memory limit' in '\n'.join(server.log_lines())

    with TestServer('uptime', ['--tdlib-restart-interval=5', '--tdlib-restart-cooldown=0']) as server:
        load = Load(server, ['getMe'], threads=2)
        time.sleep(20)
        load.finish()
        count = server.restart_count()
        log('restart count:', count)
        assert 2 <= count <= 4, count


def scenario_policy_limits():
    """The minimum uptime and the cooldown limit automatic restarts."""
    with TestServer('min_uptime', ['--tdlib-restart-memory-limit=1', '--tdlib-restart-min-uptime=6',
                                   '--tdlib-restart-cooldown=0'], second_bot=True) as server:
        assert server.api('getMe')[0]['ok'] and server.api('getMe', token=TOKEN2)[0]['ok']
        wait_for(lambda: {bot_id for _, bot_id, _ in server.restart_starts()} == {BOT_ID, BOT2_ID}, 60,
                 'restarts of both bots')
        time.sleep(10)
        starts = server.restart_starts()
        log('restarts with the minimum uptime of 6 seconds (time, bot, uptime):', starts)
        assert all(uptime >= 6 for _, _, uptime in starts), starts

    with TestServer('cooldown', ['--tdlib-restart-memory-limit=1', '--tdlib-restart-min-uptime=0',
                                 '--tdlib-restart-cooldown=8'], second_bot=True) as server:
        assert server.api('getMe')[0]['ok'] and server.api('getMe', token=TOKEN2)[0]['ok']
        wait_for(lambda: len(server.restart_starts()) >= 3, 60, '3 restarts')
        times = [start[0] for start in server.restart_starts()]
        log('intervals between restarts with the cooldown of 8 seconds:', [b - a for a, b in zip(times, times[1:])])
        assert all(b - a >= 8 for a, b in zip(times, times[1:])), times


def scenario_malloc_trim():
    """Free memory is returned to the OS once after restarts."""
    with TestServer('malloc_trim', ['--malloc-trim', '--tdlib-restart-cooldown=1']) as server:
        assert server.api('getMe')[0]['ok']
        server.request_restart()
        wait_for(lambda: server.restart_count() >= 1, 30, 'restart')
        wait_for(lambda: 'Released free memory' in '\n'.join(server.log_lines()), 20, 'malloc_trim')
        time.sleep(1)
        assert sum('Released free memory' in line for line in server.log_lines()) == 1


def scenario_options():
    """Invalid values of options are rejected."""
    work = os.path.join(WORK_ROOT, 'options')
    os.makedirs(work, exist_ok=True)
    for option in ['--tdlib-restart-drain-timeout=0', '--tdlib-restart-drain-timeout=3601',
                   '--tdlib-restart-interval=-1', '--tdlib-restart-memory-limit=-1',
                   '--tdlib-restart-bot-memory-limit=-1', '--tdlib-restart-min-uptime=-1', '--tdlib-restart-cooldown=-1',
                   '--cpu-sampling-rate=-1', '--cpu-sampling-rate=1001', '--memory-sampling-interval=-1',
                   '--memory-sampling-interval=1048577', '--message-unload-delay=59']:
        result = subprocess.run([os.path.join(BUILD_DIR, 'telegram-bot-api'), '--api-id=1', '--api-hash=test', '--local',
                                 '-p', str(testlib.free_port()), '-s', str(testlib.free_port()), '-d', work, option],
                                capture_output=True, timeout=30)
        assert result.returncode != 0, option


def upload(server, method, field, content, http_timeout, **params):
    """Calls the method with the file uploaded in the field as multipart/form-data."""
    boundary = 'test-boundary-1234567890'
    body = b''
    for name, value in params.items():
        body += ('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n%s\r\n' % (boundary, name, value)).encode()
    body += ('--%s\r\nContent-Disposition: form-data; name="%s"; filename="file.bin"\r\n'
             'Content-Type: application/octet-stream\r\n\r\n' % (boundary, field)).encode() + content + b'\r\n'
    body += ('--%s--\r\n' % boundary).encode()
    request = urllib.request.Request('http://127.0.0.1:%d/bot%s/%s' % (server.api_port, TOKEN, method), data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        with urllib.request.urlopen(request, timeout=http_timeout) as r:
            return json.loads(r.read())
    except OSError as e:
        return {'ok': False, 'description': str(e)}


def scenario_metrics():
    """All kinds of requests, files, rejections and webhook results are counted; the page is valid, can be filtered by
    bot and doesn't list closed bots or several tokens of the same bot."""
    alternative_token = '%d:AAFalternativeTokenOfTheSameBot12A' % BOT_ID
    hook = Hook(failures=[500, None])
    try:
        with TestServer('metrics', [], 3, second_bot=True, extra_bots=[(alternative_token, BOT_ID)]) as server:
            # requests, failure reasons and durations
            me = server.api('getMe')[0]['result']
            assert server.api('unknownMethod')[0]['error_code'] == 404
            result, _ = server.api('getUpdates')
            assert len(result['result']) == 3, result

            # webhook deliveries: the first attempts get HTTP 500 and a closed connection; the webhook can't be changed
            # more often than once a second
            assert server.api('setWebhook', url=hook.url)[0]['ok']
            assert server.api('setWebhook', url=hook.url + '?b')[0]['error_code'] == 429
            wait_for(lambda: len(hook.received) == 3, 60, 'webhook deliveries')

            # files of each type; sending requires network, so the client gives up after 1 second
            local_file = os.path.join(server.work, 'local.bin')
            with open(local_file, 'wb') as f:
                f.write(b'x' * 2000)
            results = [server.api('sendDocument', http_timeout=1, chat_id=123456789, document=document)[0]
                       for document in ['https://example.com/file.pdf', 'file://' + local_file, 'BAADBAADbadFileId']]
            results.append(upload(server, 'sendDocument', 'document', b'y' * 1000, 1, chat_id=123456789))
            abandoned = sum('error_code' not in result for result in results)
            assert abandoned >= 2, results
            for result in results:
                if 'error_code' in result:
                    server.calls[('senddocument', result['error_code'])] += 1

            # requests rejected before reaching a bot
            assert server.api('getMe', token='invalid')[0]['error_code'] == 401
            assert server.api('getMe', token='bot:token')[0]['error_code'] == 421
            assert server.api('close', token='%d:AAFnotStartedBotToken' % (BOT_ID + 1))[0]['error_code'] == 400

            time.sleep(0.5)  # wait for the answers to be counted
            values = server.check_request_metrics()

            def bot_value(name, **labels):
                return metric_sum(values, name, bot=BOT_ID, **labels)

            assert bot_value('tgbotapi_bot_requests_total', method='setwebhook', code=429,
                             reason='set_webhook_flood') == 1
            assert bot_value('tgbotapi_bot_requests_total', method='unknown', code=404, reason='none') == 1
            calls = sum(server.calls.values())
            get_updates = sum(count for (method, _), count in server.calls.items() if method == 'getupdates')
            assert bot_value('tgbotapi_bot_request_duration_seconds_count') == calls - get_updates
            for method in {method for method, _ in server.calls}:
                assert ('tgbotapi_bot_method_duration_seconds_total',
                        frozenset({('bot', str(BOT_ID)), ('method', method)})) in values, method
            assert bot_value('tgbotapi_bot_request_bytes_total', method='getme') > 0
            assert bot_value('tgbotapi_bot_response_bytes_total', method='getme') > 0
            assert bot_value('tgbotapi_bot_abandoned_requests_total', method='senddocument') == abandoned
            for file_type, size in [('url', 0), ('local_path', 2000), ('file_id', 0), ('upload', 1000)]:
                assert bot_value('tgbotapi_bot_input_files_total', method='senddocument', type=file_type) == 1, file_type
                assert bot_value('tgbotapi_bot_input_file_bytes_total', method='senddocument', type=file_type) == size
            for reason in ['invalid_token', 'misdirected_token', 'closed_bot']:
                assert metric_sum(values, 'tgbotapi_rejected_requests_total', reason=reason) == 1, reason
            assert bot_value('tgbotapi_bot_fetched_updates_total') == 3
            deliveries = {result: bot_value('tgbotapi_bot_webhook_deliveries_total', result=result)
                          for result in ['ok', 'http_error', 'connection_closed']}
            assert deliveries == {'ok': 3, 'http_error': 1, 'connection_closed': 1}, deliveries
            assert bot_value('tgbotapi_bot_webhook_response_seconds_total') > 0
            assert bot_value('tgbotapi_bot_tdlib_updates_total') > 0
            assert bot_value('tgbotapi_bot_info', username=me['username'], webhook=1) == 1
            assert 0 < bot_value('tgbotapi_bot_instance_uptime_seconds') < metric_sum(values, 'tgbotapi_uptime_seconds')
            assert metric_sum(values, 'tgbotapi_bots', state='authorized') == 1  # the second bot wasn't used yet

            # the page filtered by bot contains the same series of the bot
            filtered = server.metrics('?bot=%d' % BOT_ID)
            assert set(filtered) == {key for key in values if ('bot', str(BOT_ID)) in key[1]}

            # only the newest token of a bot is listed
            assert server.api('getMe', token=alternative_token)[0]['ok']
            assert server.api('getMe', token=TOKEN2)[0]['ok']
            values = server.metrics()
            assert metric_sum(values, 'tgbotapi_bots', state='authorized') == 3
            assert metric_sum(values, 'tgbotapi_bot_requests_total', bot=BOT_ID) == 1

            # metrics of a closed bot are removed
            assert server.api('close', token=TOKEN2)[0]['ok']
            wait_for(lambda: metric_sum(server.metrics(), 'tgbotapi_bots', state='authorized') == 2, 30, 'closed bot')
            assert not [key for key in server.metrics() if ('bot', str(BOT2_ID)) in key[1]]
    finally:
        hook.close()


def scenario_metrics_disabled():
    """Without CPU and memory sampling their metrics aren't exported, and the bot memory limit is ignored."""
    error = 'TDLib restart bot memory limit is ignored'
    with TestServer('metrics_disabled', ['--cpu-sampling-rate=0', '--memory-sampling-interval=0',
                                         '--tdlib-restart-bot-memory-limit=1'], expected_errors=[error]) as server:
        assert server.api('getMe')[0]['ok']
        time.sleep(1.5)
        names = {name for name, _ in server.metrics()}
        assert 'tgbotapi_bot_requests_total' in names and 'tgbotapi_process_cpu_seconds_total' in names
        prefixes = ('tgbotapi_heap_', 'tgbotapi_memory_', 'tgbotapi_cpu_', 'tgbotapi_thread_', 'tgbotapi_scheduler_',
                    'tgbotapi_bot_cpu_', 'tgbotapi_bot_memory_')
        assert not [name for name in names if name.startswith(prefixes)], names
        assert any(error in line for line in server.log_lines())


def check_cpu_metrics(values):
    """Checks that CPU time of all threads is attributed either to bots or to the server."""
    client = metric_sum(values, 'tgbotapi_bot_cpu_seconds_total', part='client')
    tdlib = metric_sum(values, 'tgbotapi_bot_cpu_seconds_total', part='tdlib')
    unattributed = metric_sum(values, 'tgbotapi_cpu_unattributed_seconds_total')
    threads = metric_sum(values, 'tgbotapi_thread_cpu_seconds_total')
    process = metric_sum(values, 'tgbotapi_process_cpu_seconds_total')
    log('CPU: client %.2f, tdlib %.2f, unattributed %.2f, threads %.2f, process %.2f' %
        (client, tdlib, unattributed, threads, process))
    assert client > 0 and tdlib > 0, (client, tdlib)
    # samples are taken each 10 ms of CPU time of a thread and collected each second
    assert abs(client + tdlib + unattributed - threads) <= 0.2 * threads + 0.3, (client, tdlib, unattributed, threads)
    assert threads <= process + 0.1, (threads, process)
    # each thread checks its timer delay 20 times per second
    lag_counts = [value for (name, labels), value in values.items() if name == 'tgbotapi_scheduler_lag_seconds_count']
    assert len(lag_counts) == 12 and min(lag_counts) > 0, lag_counts
    return client + tdlib


def scenario_resources():
    """CPU time, storage and memory usage of the bot are exported and CPU time is accumulated across restarts."""
    with TestServer('resources', ['--tdlib-restart-cooldown=1']) as server:
        assert server.api('getMe')[0]['ok']

        def tdlib_load():
            # invalid file identifiers are checked by TDLib
            for _ in range(300):
                server.api('getFile', file_id='AgADBAADbad')

        def run_tdlib_load():
            threads = [threading.Thread(target=tdlib_load) for _ in range(8)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            time.sleep(1.5)  # wait for CPU samples to be collected

        run_tdlib_load()
        values = server.check_request_metrics()
        cpu_before_restart = check_cpu_metrics(values)

        assert metric_sum(values, 'tgbotapi_bot_storage_bytes', type='database') > 0
        for name in ['tgbotapi_process_disk_read_bytes_total', 'tgbotapi_process_disk_written_bytes_total']:
            assert (name, frozenset()) in values, name
        assert metric_sum(values, 'tgbotapi_thread_disk_written_bytes_total') <= metric_sum(
            values, 'tgbotapi_process_disk_written_bytes_total')
        assert ('tgbotapi_bot_storage_files', frozenset({('bot', str(BOT_ID))})) in values
        assert metric_sum(values, 'tgbotapi_bot_tdlib_objects', type='user') >= 1
        object_types = {dict(labels)['type'] for (name, labels) in values if name == 'tgbotapi_bot_tdlib_objects'}
        for object_type in ['file_id', 'file_node', 'chat', 'message', 'message_file_source', 'video', 'document']:
            assert object_type in object_types, (object_type, object_types)

        server.request_restart()
        wait_for(lambda: server.restart_count() >= 1, 30, 'restart')
        assert server.api('getMe')[0]['ok']
        run_tdlib_load()
        values = server.check_request_metrics()
        cpu_after_restart = check_cpu_metrics(values)
        assert cpu_after_restart > cpu_before_restart, (cpu_before_restart, cpu_after_restart)
        assert metric_sum(values, 'tgbotapi_bot_tdlib_objects', type='user') >= 1


def check_memory_metrics(values):
    """Checks consistency of memory accounting and returns memory of the bots."""
    exact = metric_sum(values, 'tgbotapi_heap_bytes')
    estimated = metric_sum(values, 'tgbotapi_heap_estimated_bytes')
    samples = metric_sum(values, 'tgbotapi_memory_samples')
    unattributed = metric_sum(values, 'tgbotapi_memory_unattributed_bytes')
    bots = {}
    for bot_id in [BOT_ID, BOT2_ID]:
        bots[bot_id] = {part: metric_sum(values, 'tgbotapi_bot_memory_bytes', bot=bot_id, part=part)
                        for part in ['client', 'tdlib', 'updates']}
    log('memory: heap %.1f MB, estimated %.1f MB (%d samples), unattributed %.1f MB, bots %s' % (
        exact / 1e6, estimated / 1e6, samples, unattributed / 1e6,
        {bot_id: {part: round(value / 1e6, 2) for part, value in parts.items()} for bot_id, parts in bots.items()}))
    error = metric_sum(values, 'tgbotapi_heap_estimate_error_bytes')
    assert exact > 0 and samples > 50 and error > 0, (exact, samples, error)
    # the server reports the standard deviation of the estimate
    assert abs(estimated - exact) < 5 * error, (estimated, exact, error)
    for bot_id in [BOT_ID, BOT2_ID]:
        assert ('tgbotapi_bot_memory_error_bytes', frozenset({('bot', str(bot_id))})) in values, bot_id
    assert abs(sum(sum(parts.values()) for parts in bots.values()) + unattributed - estimated) <= 1, 'parts sum'
    return bots


def scenario_memory():
    """Memory of each bot is estimated, attributed to the right bot and is used to restart TDLib instances."""
    with TestServer('memory', ['--memory-sampling-interval=16'], second_bot=True) as server:
        assert server.api('getMe')[0]['ok'] and server.api('getMe', token=TOKEN2)[0]['ok']
        time.sleep(1)
        values = server.metrics()
        heap_before = metric_sum(values, 'tgbotapi_heap_bytes')
        before = check_memory_metrics(values)
        for bot_id in [BOT_ID, BOT2_ID]:
            assert before[bot_id]['tdlib'] > 0, before

        # messages, which are being sent by the first bot, stay in memory of its TDLib instance and Bot API client
        def send_messages(thread_index):
            for i in range(250):
                server.api('sendMessage', http_timeout=0.1, chat_id=BOT_ID,
                           text='message %d %d %s' % (thread_index, i, 'x' * 2000))

        threads = [threading.Thread(target=send_messages, args=(i,)) for i in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        time.sleep(2)
        values = server.metrics()
        heap_growth = metric_sum(values, 'tgbotapi_heap_bytes') - heap_before
        after = check_memory_metrics(values)
        growth = {bot_id: sum(after[bot_id].values()) - sum(before[bot_id].values()) for bot_id in after}
        log('memory growth: heap %.1f MB, bot 1 %.1f MB, bot 2 %.1f MB' % (
            heap_growth / 1e6, growth[BOT_ID] / 1e6, growth[BOT2_ID] / 1e6))
        # the rest is used by HTTP connections and requests, which aren't attributed to bots
        assert heap_growth > 1e6 and growth[BOT_ID] > 0.5 * heap_growth, (heap_growth, growth)
        assert abs(growth[BOT2_ID]) < 0.1 * growth[BOT_ID], growth
        # TDLib aborts at the exit of the process if thousands of network requests are still pending, which happens
        # without network access; the original server behaves the same, so the instance is closed before the exit, and
        # its destruction is finished in a few seconds after the bot is removed
        assert server.api('close', http_timeout=60)[0]['ok']
        wait_for(lambda: metric_sum(server.metrics(), 'tgbotapi_bots', state='authorized') == 1, 30, 'closed bot')
        time.sleep(3)

    # only the bot, which exceeds the memory limit, is restarted, and restarts don't leak memory
    with TestServer('memory_limit', ['--memory-sampling-interval=4', '--tdlib-restart-bot-memory-limit=1',
                                     '--tdlib-restart-min-uptime=2', '--tdlib-restart-cooldown=1'],
                    second_bot=True) as server:
        assert server.api('getMe')[0]['ok'] and server.api('getMe', token=TOKEN2)[0]['ok']
        load = Load(server, ['getMe', 'getWebhookInfo'], threads=4)
        wait_for(lambda: server.restart_count() >= 1, 60, 'restart because of the bot memory limit')
        time.sleep(1)
        values = server.metrics()
        first_heap = metric_sum(values, 'tgbotapi_heap_bytes')
        first = check_memory_metrics(values)
        wait_for(lambda: server.restart_count() >= 6, 60, 'restarts because of the bot memory limit')
        time.sleep(1)
        load.finish()
        values = server.check_request_metrics()
        last_heap = metric_sum(values, 'tgbotapi_heap_bytes')
        last = check_memory_metrics(values)
        log('after 1 restart: heap %.2f MB, bot %.2f MB; after 6 restarts: heap %.2f MB, bot %.2f MB' % (
            first_heap / 1e6, sum(first[BOT_ID].values()) / 1e6, last_heap / 1e6, sum(last[BOT_ID].values()) / 1e6))
        assert metric_sum(values, 'tgbotapi_bot_tdlib_restarts_total', bot=BOT_ID) >= 6
        assert sum(first[BOT2_ID].values()) < 1 << 20, first
        assert metric_sum(values, 'tgbotapi_bot_tdlib_restarts_total', bot=BOT2_ID) == 0, 'bot below the limit'
        assert last_heap < 1.1 * first_heap, 'memory leak'
        assert abs(sum(last[BOT_ID].values()) - sum(first[BOT_ID].values())) < 0.25 * sum(first[BOT_ID].values())
        assert 'because of bot memory limit' in '\n'.join(server.log_lines())


SCENARIOS = ['webhook', 'polling', 'polling_pending', 'drain_timeout', 'cancel_with_getupdates', 'webhook_change',
             'close_during_restart', 'shutdown_during_restart', 'restart_all', 'policy', 'policy_limits',
             'malloc_trim', 'options', 'metrics', 'metrics_disabled', 'resources', 'memory']


def main():
    global BUILD_DIR, WORK_ROOT
    if len(sys.argv) < 2 or any(name not in SCENARIOS for name in sys.argv[2:]):
        print(__doc__ + '\nScenarios: ' + ' '.join(SCENARIOS))
        sys.exit(2)
    BUILD_DIR = os.path.abspath(sys.argv[1])
    WORK_ROOT = tempfile.mkdtemp(prefix='telegram-bot-api-restart-test-')
    failed = []
    try:
        for name in sys.argv[2:] or SCENARIOS:
            log('run scenario', name)
            try:
                globals()['scenario_' + name]()
                log('scenario', name, 'PASSED')
            except Exception as e:
                traceback.print_exc()
                log('scenario', name, 'FAILED:', repr(e))
                failed.append(name)
    finally:
        shutil.rmtree(WORK_ROOT, ignore_errors=True)
    print('\nALL SCENARIOS PASSED' if not failed else '\nFAILED SCENARIOS: ' + ' '.join(failed))
    sys.exit(0 if not failed else 1)


if __name__ == '__main__':
    main()
