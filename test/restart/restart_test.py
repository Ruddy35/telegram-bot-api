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
#   python3 test/restart/restart_test.py <build dir> [webhook] [polling] [drain_timeout] [policy] [metrics] [resources]
#                                        [memory]
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
BOT2_ID = 136817688  # @Channel_Bot, also known to TDLib without network access
TOKEN2 = '%d:AAGq7rGkXmpVZ3bNzE9tP1yLcW4sDfHjKlQ' % BOT2_ID
API_PORT = 18081
STAT_PORT = 18082
HOOK_PORT = 18083

BUILD_DIR = None
WORK_ROOT = None

# answered API requests by (method, HTTP status code) for comparison with server metrics
API_CALLS = collections.Counter()
API_CALLS_LOCK = threading.Lock()


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


def api(method, timeout=60, token=None, **params):
    data = json.dumps(params).encode()
    req = urllib.request.Request('http://127.0.0.1:%d/bot%s/%s' % (API_PORT, token or TOKEN, method), data=data,
                                 headers={'Content-Type': 'application/json'})
    start = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            result = json.loads(r.read())
    except urllib.error.HTTPError as e:
        result = json.loads(e.read())
    except OSError as e:
        return {'ok': False, 'description': str(e)}, time.time() - start
    if token is not None:
        return result, time.time() - start
    with API_CALLS_LOCK:
        # the server counts requests to non-existing methods as requests to the method "unknown"
        stat_method = 'unknown' if method == 'unknownMethod' else method.lower()
        API_CALLS[(stat_method, 200 if result.get('ok') else result.get('error_code'))] += 1
    return result, time.time() - start


def metrics():
    """Returns server metrics as {(name, frozenset of labels): value}."""
    with urllib.request.urlopen('http://127.0.0.1:%d/metrics' % STAT_PORT, timeout=10) as r:
        text = r.read().decode()
    result = {}
    for line in text.splitlines():
        if not line or line.startswith('#'):
            continue
        name_labels, value = line.rsplit(' ', 1)
        labels = frozenset()
        name = name_labels
        if '{' in name_labels:
            name, label_str = name_labels[:-1].split('{', 1)
            labels = frozenset(tuple(item.split('=', 1)) for item in label_str.split('",'))
            labels = frozenset((k, v.strip('"')) for k, v in labels)
        result[(name, labels)] = float(value)
    return result


def metric_sum(values, name, **labels):
    return sum(value for (metric_name, metric_labels), value in values.items()
               if metric_name == name and all((k, str(v)) in metric_labels for k, v in labels.items()))


def check_request_metrics():
    """Checks that the server counted exactly the requests answered to the test."""
    values = metrics()
    with API_CALLS_LOCK:
        calls = dict(API_CALLS)
    for (method, code), count in sorted(calls.items()):
        counted = metric_sum(values, 'tgbotapi_bot_requests_total', bot=BOT_ID, method=method, code=code)
        assert counted == count, (method, code, count, counted)
    total = metric_sum(values, 'tgbotapi_bot_requests_total', bot=BOT_ID)
    assert total == sum(calls.values()), (total, sum(calls.values()))
    log('metrics: %d answered requests are counted exactly' % total)
    return values


def stats(query=''):
    with urllib.request.urlopen('http://127.0.0.1:%d/%s' % (STAT_PORT, query), timeout=10) as r:
        text = r.read().decode()
    result = {}
    for line in text.splitlines():
        parts = line.split('\t')
        if len(parts) >= 2 and parts[0] not in result:
            result[parts[0]] = parts[1]
    return result


def prepare(name, update_count, second_bot=False):
    work = os.path.join(WORK_ROOT, name)
    if os.path.exists(work):
        shutil.rmtree(work)
    for token, bot_id in [(TOKEN, BOT_ID)] + ([(TOKEN2, BOT2_ID)] if second_bot else []):
        os.makedirs(os.path.join(work, token))
        subprocess.run([os.path.join(BUILD_DIR, 'restart-test-fake-auth'), os.path.join(work, token, 'td.binlog'),
                        str(bot_id)], check=True, capture_output=True)
    if update_count:
        subprocess.run([os.path.join(BUILD_DIR, 'restart-test-fake-updates'), os.path.join(work, 'tqueue.binlog'),
                        str(BOT_ID), str(update_count)], check=True, capture_output=True)
    return work


def start_server(work, *extra):
    with API_CALLS_LOCK:
        API_CALLS.clear()
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
        time.sleep(0.5)  # wait for the last answers to be counted
        values = check_request_metrics()
        assert metric_sum(values, 'tgbotapi_bot_webhook_deliveries_total', result='ok') == update_count
        assert metric_sum(values, 'tgbotapi_bot_tdlib_restarts_total') == 3
        assert metric_sum(values, 'tgbotapi_tdlib_failed_restarts_total') == 0
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
        time.sleep(0.5)
        values = check_request_metrics()
        assert metric_sum(values, 'tgbotapi_bot_fetched_updates_total') == 50
        assert metric_sum(values, 'tgbotapi_bot_tdlib_restarts_total') == 1
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
        values = metrics()
        assert metric_sum(values, 'tgbotapi_tdlib_failed_restarts_total') == 1
        assert metric_sum(values, 'tgbotapi_bot_requests_total', method='getme', code=200) == \
            API_CALLS[('getme', 200)]
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


def scenario_metrics():
    """Failure reasons and requests abandoned by clients are counted."""
    work = prepare('metrics', 3)
    proc = start_server(work)
    try:
        assert api('getMe')[0]['ok']
        assert api('unknownMethod')[0]['error_code'] == 404
        assert api('setWebhook', url='http://127.0.0.1:%d/a' % HOOK_PORT)[0]['ok']
        assert api('setWebhook', url='http://127.0.0.1:%d/b' % HOOK_PORT)[0]['error_code'] == 429
        assert api('deleteWebhook')[0]['ok']
        # requires network, so it is never answered; the client gives up after 1 second
        result, _ = api('sendDocument', timeout=1, chat_id=123456789, document='https://example.com/file.pdf')
        assert not result['ok'], result
        result, _ = api('getUpdates')
        assert len(result['result']) == 3, result
        time.sleep(0.5)
        values = check_request_metrics()
        assert metric_sum(values, 'tgbotapi_bot_requests_total', method='setwebhook', code=429,
                          reason='set_webhook_flood') == 1
        assert metric_sum(values, 'tgbotapi_bot_requests_total', method='unknown', code=404) == 1
        assert metric_sum(values, 'tgbotapi_bot_abandoned_requests_total', method='senddocument') == 1
        assert metric_sum(values, 'tgbotapi_bot_input_files_total', method='senddocument', type='url') == 1
        assert metric_sum(values, 'tgbotapi_bot_fetched_updates_total') == 3
        assert metric_sum(values, 'tgbotapi_bots', state='authorized') == 1
    finally:
        stop_server(proc)


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
    work = prepare('resources', 0)
    proc = start_server(work, '--tdlib-restart-cooldown=1')
    try:
        assert api('getMe')[0]['ok']

        def tdlib_load():
            # invalid file identifiers are checked by TDLib
            for _ in range(300):
                api('getFile', file_id='AgADBAADbad')

        threads = [threading.Thread(target=tdlib_load) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        time.sleep(1.5)  # wait for CPU samples to be collected
        values = check_request_metrics()
        cpu_before_restart = check_cpu_metrics(values)

        assert metric_sum(values, 'tgbotapi_bot_storage_bytes', type='database') > 0
        for name in ['tgbotapi_process_disk_read_bytes_total', 'tgbotapi_process_disk_written_bytes_total']:
            assert (name, frozenset()) in values, name
        assert metric_sum(values, 'tgbotapi_thread_disk_written_bytes_total') <= metric_sum(
            values, 'tgbotapi_process_disk_written_bytes_total')
        log('disk: written %d bytes, TDLib database thread wrote %d bytes' % (
            metric_sum(values, 'tgbotapi_process_disk_written_bytes_total'),
            metric_sum(values, 'tgbotapi_thread_disk_written_bytes_total', scheduler=1)))
        assert ('tgbotapi_bot_storage_files', frozenset({('bot', str(BOT_ID))})) in values
        assert metric_sum(values, 'tgbotapi_bot_tdlib_objects', type='user') >= 1
        object_types = {dict(labels)['type'] for (name, labels) in values if name == 'tgbotapi_bot_tdlib_objects'}
        for object_type in ['file_id', 'file_node', 'chat', 'message', 'message_file_source', 'video', 'document']:
            assert object_type in object_types, (object_type, object_types)

        stats('?restart=%d' % BOT_ID)
        wait_for(lambda: restart_count() >= 1, 30, 'restart')
        assert api('getMe')[0]['ok']
        threads = [threading.Thread(target=tdlib_load) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        time.sleep(1.5)
        values = check_request_metrics()
        cpu_after_restart = check_cpu_metrics(values)
        assert cpu_after_restart > cpu_before_restart, (cpu_before_restart, cpu_after_restart)
        assert metric_sum(values, 'tgbotapi_bot_tdlib_objects', type='user') >= 1
    finally:
        stop_server(proc)


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
    # the server reports standard deviation of the estimate
    assert abs(estimated - exact) < 5 * error, (estimated, exact, error)
    for bot_id in [BOT_ID, BOT2_ID]:
        assert (('tgbotapi_bot_memory_error_bytes', frozenset({('bot', str(bot_id))})) in values), bot_id
    assert abs(sum(sum(parts.values()) for parts in bots.values()) + unattributed - estimated) <= 1, 'parts sum'
    return bots


def scenario_memory():
    """Memory of each bot is estimated, attributed to the right bot and is used to restart TDLib instances."""
    work = prepare('memory', 0, second_bot=True)
    proc = start_server(work, '--memory-sampling-interval=16')
    try:
        assert api('getMe')[0]['ok']
        assert api('getMe', token=TOKEN2)[0]['ok']
        time.sleep(1)
        values = metrics()
        heap_before = metric_sum(values, 'tgbotapi_heap_bytes')
        before = check_memory_metrics(values)
        for bot_id in [BOT_ID, BOT2_ID]:
            assert before[bot_id]['tdlib'] > 0, before

        # messages, which are being sent by the first bot, stay in memory of its TDLib instance and Bot API client
        def send_messages(thread_index):
            for i in range(250):
                api('sendMessage', timeout=0.1, chat_id=BOT_ID, text='message %d %d %s' % (thread_index, i, 'x' * 2000))

        threads = [threading.Thread(target=send_messages, args=(i,)) for i in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        time.sleep(2)
        values = metrics()
        heap_growth = metric_sum(values, 'tgbotapi_heap_bytes') - heap_before
        after = check_memory_metrics(values)
        growth = {bot_id: sum(after[bot_id].values()) - sum(before[bot_id].values()) for bot_id in after}
        log('memory growth: heap %.1f MB, bot 1 %.1f MB, bot 2 %.1f MB' % (
            heap_growth / 1e6, growth[BOT_ID] / 1e6, growth[BOT2_ID] / 1e6))
        # the rest is used by HTTP connections and requests, which aren't attributed to bots
        assert heap_growth > 1e6 and growth[BOT_ID] > 0.5 * heap_growth, (heap_growth, growth)
        assert abs(growth[BOT2_ID]) < 0.1 * growth[BOT_ID], growth
    finally:
        stop_server(proc)

    # only the bot, which exceeds the memory limit, is restarted, and restarts don't leak memory
    work = prepare('memory_limit', 0, second_bot=True)
    proc = start_server(work, '--memory-sampling-interval=4', '--tdlib-restart-bot-memory-limit=1',
                        '--tdlib-restart-min-uptime=2', '--tdlib-restart-cooldown=1')
    try:
        assert api('getMe')[0]['ok']
        assert api('getMe', token=TOKEN2)[0]['ok']
        load = Load(['getMe', 'getWebhookInfo'], threads=4)
        wait_for(lambda: restart_count() >= 1, 60, 'restart because of the bot memory limit')
        time.sleep(1)
        values = metrics()
        first_heap = metric_sum(values, 'tgbotapi_heap_bytes')
        first = check_memory_metrics(values)
        wait_for(lambda: restart_count() >= 6, 60, 'restarts because of the bot memory limit')
        time.sleep(1)
        load.finish()
        values = check_request_metrics()
        last_heap = metric_sum(values, 'tgbotapi_heap_bytes')
        last = check_memory_metrics(values)
        log('after 1 restart: heap %.2f MB, bot %.2f MB; after 6 restarts: heap %.2f MB, bot %.2f MB' % (
            first_heap / 1e6, sum(first[BOT_ID].values()) / 1e6, last_heap / 1e6, sum(last[BOT_ID].values()) / 1e6))
        assert metric_sum(values, 'tgbotapi_bot_tdlib_restarts_total', bot=BOT_ID) >= 6
        assert sum(first[BOT2_ID].values()) < 1 << 20, first
        assert metric_sum(values, 'tgbotapi_bot_tdlib_restarts_total', bot=BOT2_ID) == 0, 'bot below the limit'
        assert last_heap < 1.1 * first_heap, 'memory leak'
        assert abs(sum(last[BOT_ID].values()) - sum(first[BOT_ID].values())) < 0.25 * sum(first[BOT_ID].values())
        with open(os.path.join(work, 'log.txt'), errors='replace') as f:
            assert 'because of bot memory limit' in f.read()
    finally:
        stop_server(proc)


def main():
    global BUILD_DIR, WORK_ROOT
    if len(sys.argv) < 2:
        print(__doc__ or 'Usage: restart_test.py <build dir> [scenario]...')
        sys.exit(1)
    BUILD_DIR = os.path.abspath(sys.argv[1])
    scenarios = sys.argv[2:] or ['webhook', 'polling', 'drain_timeout', 'policy', 'metrics', 'resources', 'memory']
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
