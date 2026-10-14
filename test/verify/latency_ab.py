#!/usr/bin/env python3
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
"""A/B test of latency, stalls and failures of the server in different configurations, for example, with and without
memory accounting.

Servers of all configurations are started with two offline test bots and get the same load. Rounds are repeated, and
every round gives a paired comparison. By default the servers of a round run in turn, and the order alternates between
rounds. With --concurrent=1 they run at the same time on different ports (the ports alternate between rounds), so pauses
of the whole machine, for example, of a virtual machine by its host, delay probes of all servers at the same time; probes
delayed while probes of the other servers weren't are counted separately as delays caused by the server itself.
 - Background load: open loop requests with a fixed total rate over many keep-alive connections; a mix of methods
   answered by Bot API clients (getMe, getWebhookInfo), by TDLib (getChatMember, getFile) and requests with
   100 KB bodies, which are allocated as big memory blocks and are sampled often.
 - Probes: requests sent by a fixed schedule; latency is measured from the scheduled time, so a stall of the server
   delays all probes scheduled during the stall (no coordinated omission). The "client" probe is getMe, which is
   answered by a Bot API client; the "tdlib" probe is getChatMember, which is answered by TDLib.
A request fails if it isn't answered with the expected HTTP status code.

For every configuration and every statistic the report shows the median over the rounds and for every configuration
except the first one the difference from the first configuration over paired rounds: the Hodges-Lehmann estimate and
its 95% confidence interval by the Wilcoxon signed-rank test, which aren't spoiled by a single round disturbed by the
machine; at least 6 rounds are needed:
 PASS - the upper bound of the interval is below the allowed increase, i.e. a bigger increase is excluded;
 FAIL - the lower bound of the interval is above the allowed increase;
 INCONCLUSIVE - the noise is too big to decide, more rounds are needed.

Usage:
  latency_ab.py <build dir with restart-test-fake-auth> --config=NAME=BINARY[,ARGUMENT]... [--config=...]
                [--rounds=8] [--duration=20] [--load-rate=2000] [--load-connections=64] [--concurrent=1] [--json=<file>]
  latency_ab.py --analyze=<file saved with --json>
Example:
  latency_ab.py build --config=baseline=nohs/telegram-bot-api,--cpu-sampling-rate=0 --config=default=build/telegram-bot-api

Exit code is 0 if no requests failed and all comparisons passed.
"""
import asyncio
import bisect
import json
import math
import os
import random
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

PROBE_STREAMS = {
    # name: (method, parameters of bot 1, period in seconds, number of connections)
    'client': ('getMe', {}, 0.002, 8),
    'tdlib': ('getChatMember', {'chat_id': 1087968824, 'user_id': 1087968824}, 0.005, 8),
}

STALL_THRESHOLDS = [0.01, 0.05, 0.1]

REQUEST_TIMEOUT = 10.0  # a request without a response in this time fails

BASE_PORT = 18100  # servers use ports BASE_PORT + 10 * i and BASE_PORT + 10 * i + 1

# delayed probes of servers running at the same time are caused by the machine, if they were scheduled at most this far
# apart
COINCIDENCE_WINDOW = 0.2

# statistic: (allowed relative increase, allowed absolute increase in seconds); the bigger one is used
ALLOWED_INCREASE = {
    'p50': (0.10, 0.0002),
    'p99': (0.15, 0.0005),
    'p99.9': (0.25, 0.001),
    'stalled': (0.0, 0.001),
    'own_stalled': (0.0, 0.001),
    'cpu_per_request': (0.05, 0.0),
}


def make_request(token, method, params):
    body = json.dumps(params).encode()
    return (b'POST /bot%s/%s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %d\r\n'
            b'\r\n' % (token.encode(), method.encode(), len(body)) + body)


async def read_response(reader):
    status_line = await reader.readline()
    if not status_line:
        raise ConnectionError('connection closed')
    status = int(status_line.split()[1])
    length = 0
    while True:
        line = await reader.readline()
        if line in (b'\r\n', b'\n', b''):
            break
        name, _, value = line.partition(b':')
        if name.strip().lower() == b'content-length':
            length = int(value)
    await reader.readexactly(length)
    return status


class Connection:
    def __init__(self, port):
        self.port = port
        self.reader = None
        self.writer = None

    async def request(self, data):
        if self.writer is None:
            self.reader, self.writer = await asyncio.open_connection('127.0.0.1', self.port)
        try:
            self.writer.write(data)
            return await asyncio.wait_for(read_response(self.reader), REQUEST_TIMEOUT)
        except (OSError, ConnectionError, asyncio.IncompleteReadError, asyncio.TimeoutError, ValueError, IndexError):
            self.writer.close()
            self.writer = None
            raise


async def run_probes(port, tokens, duration):
    """Sends probes by the schedule and returns {stream: {'latencies': [...], 'errors': [...]}}."""
    results = {name: {'latencies': [], 'errors': [], 'stall_times': []} for name in PROBE_STREAMS}
    loop = asyncio.get_running_loop()
    start = loop.time() + 0.1

    async def probe(name, index):
        method, params, period, connection_count = PROBE_STREAMS[name]
        connection = Connection(port)
        data = make_request(tokens[0], method, params)
        connection_period = period * connection_count
        scheduled = start + period * index
        end = start + duration
        while scheduled < end and loop.time() < end:
            delay = scheduled - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)
            try:
                status = await connection.request(data)
                if status != 200:
                    results[name]['errors'].append('HTTP %d' % status)
            except Exception as e:
                results[name]['errors'].append(repr(e))
            latency = loop.time() - scheduled
            results[name]['latencies'].append(latency)
            if latency > STALL_THRESHOLDS[0]:
                # the time of the system-wide monotonic clock, which is comparable between processes
                results[name]['stall_times'].append(scheduled)
            scheduled += connection_period

    await asyncio.gather(*[probe(name, i) for name, stream in PROBE_STREAMS.items() for i in range(stream[3])])
    return results


def load_requests(tokens, bot_ids):
    """Returns the mix of background requests as [(request bytes, expected status, weight)]."""
    result = []
    for token, bot_id in zip(tokens, bot_ids):
        result += [
            (make_request(token, 'getMe', {}), 200, 40),
            (make_request(token, 'getWebhookInfo', {}), 200, 30),
            (make_request(token, 'getChatMember', {'chat_id': bot_id, 'user_id': bot_id}), 200, 15),
            (make_request(token, 'getFile', {'file_id': 'AgADBAAD' + 'A' * 40}), 400, 10),
            (make_request(token, 'getWebhookInfo', {'padding': 'x' * 100000}), 200, 5),
        ]
    return result


async def run_load(port, tokens, bot_ids, duration, rate, connection_count, seed):
    """Sends requests with the given total rate; returns the number of requests and errors."""
    requests = load_requests(tokens, bot_ids)
    weights = [weight for _, _, weight in requests]
    rng = random.Random(seed)
    loop = asyncio.get_running_loop()
    start = loop.time() + 0.1
    count = 0
    errors = []
    late = 0

    async def worker(index):
        nonlocal count, late
        connection = Connection(port)
        period = connection_count / rate
        scheduled = start + index / rate
        end = start + duration
        while scheduled < end and loop.time() < end:
            delay = scheduled - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)
            elif delay < -1:
                late += 1
            data, expected_status, _ = rng.choices(requests, weights)[0]
            method = data.split(b' ', 2)[1].rsplit(b'/', 1)[1].decode()
            try:
                status = await connection.request(data)
                if status != expected_status:
                    errors.append('%s: HTTP %d' % (method, status))
            except Exception as e:
                errors.append('%s: %r' % (method, e))
            count += 1
            scheduled += period

    await asyncio.gather(*[worker(i) for i in range(connection_count)])
    return {'count': count, 'errors': errors, 'late': late}


def worker_main(args):
    kind, port, duration = args[0], int(args[1]), float(args[2])
    tokens = args[3].split(',')
    bot_ids = [int(bot_id) for bot_id in args[4].split(',')]
    if kind == 'probe':
        result = asyncio.run(run_probes(port, tokens, duration))
    else:
        result = asyncio.run(run_load(port, tokens, bot_ids, duration, float(args[5]), int(args[6]), int(args[7])))
    json.dump(result, sys.stdout)


def process_cpu_seconds(pid):
    with open('/proc/%d/stat' % pid) as f:
        fields = f.read().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def process_peak_rss(pid):
    with open('/proc/%d/status' % pid) as f:
        for line in f:
            if line.startswith('VmHWM:'):
                return int(line.split()[1]) * 1024
    return 0


def percentile(sorted_values, q):
    if not sorted_values:
        return float('nan')
    return sorted_values[min(len(sorted_values) - 1, int(q * len(sorted_values)))]


# critical values of the Wilcoxon signed-rank statistic for the two-sided significance level 0.05
WILCOXON_CRITICAL = {6: 0, 7: 2, 8: 3, 9: 5, 10: 8, 11: 10, 12: 13, 13: 17, 14: 21, 15: 25, 16: 29, 17: 34, 18: 40,
                     19: 46, 20: 52, 21: 58, 22: 65, 23: 73, 24: 81, 25: 89}


def paired_difference(differences):
    """Returns the Hodges-Lehmann estimate of the difference and its 95% confidence interval by the Wilcoxon signed-rank
    test. Unlike the mean and the t-interval, they aren't spoiled by a single round disturbed by the machine."""
    n = len(differences)
    walsh = sorted((differences[i] + differences[j]) / 2 for i in range(n) for j in range(i, n))
    estimate = median(walsh)
    if n < 6:
        return estimate, float('-inf'), float('inf')
    if n in WILCOXON_CRITICAL:
        k = WILCOXON_CRITICAL[n]
    else:
        k = int(n * (n + 1) / 4 - 1.96 * math.sqrt(n * (n + 1) * (2 * n + 1) / 24))
    return estimate, walsh[k], walsh[len(walsh) - 1 - k]


def fetch_metrics(port):
    with urllib.request.urlopen('http://127.0.0.1:%d/metrics' % port, timeout=30) as r:
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
            labels = frozenset((k, v.strip('"')) for k, v in (item.split('=', 1) for item in label_str.split('",')))
        result[(name, labels)] = float(value)
    return result


def metric_sum(values, name):
    return sum(value for (metric_name, _), value in values.items() if metric_name == name)


class ServerRun:
    """A server of one configuration in one round."""

    def __init__(self, t, name, binary, args, index, round_index, options):
        self.t = t
        self.name = name
        self.binary = binary
        self.args = args
        self.api_port = BASE_PORT + 10 * index
        self.stat_port = self.api_port + 1
        self.index = index
        self.round_index = round_index
        self.options = options
        self.tokens = [t.TOKEN, t.TOKEN2]
        self.bot_ids = [t.BOT_ID, t.BOT2_ID]
        self.proc = None

    def start(self):
        work = self.t.prepare('ab%d' % self.index, 0, second_bot=True)
        self.proc = subprocess.Popen(
            [self.binary, '--api-id=1', '--api-hash=test', '--local', '-p', str(self.api_port), '-s', str(self.stat_port),
             '-d', work, '-l', os.path.join(work, 'log.txt'), '-v', '2'] + self.args,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for token in self.tokens:
            deadline = time.time() + 30
            while True:
                try:
                    request = urllib.request.Request('http://127.0.0.1:%d/bot%s/getMe' % (self.api_port, token),
                                                     data=b'{}', headers={'Content-Type': 'application/json'})
                    with urllib.request.urlopen(request, timeout=10) as r:
                        assert json.loads(r.read())['ok']
                    break
                except OSError:
                    if time.time() > deadline:
                        raise
                    time.sleep(0.1)

    def common_arguments(self, duration):
        return [str(self.api_port), str(duration), ','.join(self.tokens), ','.join(str(bot_id) for bot_id in self.bot_ids)]

    def warm_up(self):
        return [subprocess.Popen([sys.executable, __file__, '--worker', 'load'] + self.common_arguments(3) +
                                 [self.options['load-rate'], self.options['load-connections'], '0'],
                                 stdout=subprocess.PIPE)]

    def begin(self):
        self.metrics_before = fetch_metrics(self.stat_port)
        self.cpu_before = process_cpu_seconds(self.proc.pid)
        common = self.common_arguments(float(self.options['duration']))
        load_processes = int(self.options['load-processes'])
        workers = []
        for i in range(load_processes):
            workers.append(subprocess.Popen(
                [sys.executable, __file__, '--worker', 'load'] + common +
                [str(float(self.options['load-rate']) / load_processes),
                 str(int(self.options['load-connections']) // load_processes), str(self.round_index * 100 + i)],
                stdout=subprocess.PIPE))
        workers.append(subprocess.Popen([sys.executable, __file__, '--worker', 'probe'] + common, stdout=subprocess.PIPE))
        return workers

    def finish(self, outputs):
        try:
            cpu = process_cpu_seconds(self.proc.pid) - self.cpu_before
            peak_rss = process_peak_rss(self.proc.pid)
            metrics = fetch_metrics(self.stat_port)
        finally:
            self.proc.send_signal(signal.SIGTERM)
            self.proc.wait(60)
        metrics_before = self.metrics_before
        probes = outputs[-1]
        loads = outputs[:-1]
        request_count = sum(load['count'] for load in loads) + sum(len(p['latencies']) for p in probes.values())
        result = {
            'config': self.name,
            'round': self.round_index,
            'requests': request_count,
            'load_errors': [error for load in loads for error in load['errors']],
            'late_load_requests': sum(load['late'] for load in loads),
            'cpu_per_request': cpu / request_count,
            'peak_rss': peak_rss,
            'streams': {},
            # statistics since the start of the server, including the start
            'server': {
                'sampler_max_operation': metric_sum(metrics, 'tgbotapi_memory_sampler_max_operation_seconds'),
                'sampler_max_lock_wait': metric_sum(metrics, 'tgbotapi_memory_sampler_max_lock_wait_seconds'),
            },
            # statistics of the measurement window
            'sampler_operations': metric_sum(metrics, 'tgbotapi_memory_sampler_operation_seconds_count') - metric_sum(
                metrics_before, 'tgbotapi_memory_sampler_operation_seconds_count'),
            'slow_sampler_operations': [sum(counts) for counts in zip(*histogram_excess(
                metrics_before, metrics, 'tgbotapi_memory_sampler_operation_seconds', SAMPLER_THRESHOLDS).values())] or
                [0] * len(SAMPLER_THRESHOLDS),
            'scheduler_lags': {dict(key).get('scheduler'): counts for key, counts in histogram_excess(
                metrics_before, metrics, 'tgbotapi_scheduler_lag_seconds', LAG_THRESHOLDS).items() if any(counts)},
            'has_scheduler_lag': any(name == 'tgbotapi_scheduler_lag_seconds_count' for name, _ in metrics),
        }
        for name, probe in probes.items():
            latencies = sorted(probe['latencies'])
            result['streams'][name] = {
                'count': len(latencies),
                'errors': probe['errors'],
                'p50': percentile(latencies, 0.5),
                'p99': percentile(latencies, 0.99),
                'p99.9': percentile(latencies, 0.999),
                'max': latencies[-1] if latencies else float('nan'),
                'stalls': [sum(1 for x in latencies if x > threshold) for threshold in STALL_THRESHOLDS],
                'stalled': sum(1 for x in latencies if x > STALL_THRESHOLDS[0]) / max(len(latencies), 1),
                'stall_times': probe['stall_times'],
                'latencies': probe['latencies'],
            }
        return result


def run_together(servers):
    """Runs the servers at the same time and returns their results."""
    try:
        for server in servers:
            server.start()
        for worker in [worker for server in servers for worker in server.warm_up()]:
            worker.communicate()
        workers = [server.begin() for server in servers]
        outputs = [[json.loads(worker.communicate()[0]) for worker in server_workers] for server_workers in workers]
    except BaseException:
        for server in servers:
            if server.proc is not None and server.proc.poll() is None:
                server.proc.kill()
        raise
    return [server.finish(output) for server, output in zip(servers, outputs)]


def mark_own_stalls(results):
    """Marks probes, which were delayed when probes of other servers of the same round weren't."""
    for result in results:
        others = sorted(time_ for other in results if other is not result for stream in other['streams'].values()
                        for time_ in stream['stall_times'])
        for stream in result['streams'].values():
            own = 0
            for time_ in stream['stall_times']:
                position = bisect.bisect_left(others, time_ - COINCIDENCE_WINDOW)
                if position == len(others) or others[position] > time_ + COINCIDENCE_WINDOW:
                    own += 1
            stream['own_stalled'] = own / max(stream['count'], 1)


def histogram_excess(before, after, name, thresholds):
    """Returns numbers of observations of a histogram metric above the thresholds between two scrapes as
    {labels without le: [count above each threshold]}."""
    def buckets(values):
        result = {}
        for (metric_name, labels), value in values.items():
            if metric_name == name + '_bucket':
                labels = dict(labels)
                le = float(labels.pop('le'))
                result.setdefault(tuple(sorted(labels.items())), {})[le] = value
        return result

    before = buckets(before)
    result = {}
    for key, counts in buckets(after).items():
        old = before.get(key, {})
        total = counts[float('inf')] - old.get(float('inf'), 0)
        result[key] = [total - (counts[threshold] - old.get(threshold, 0)) for threshold in thresholds]
    return result


SAMPLER_THRESHOLDS = [0.0001, 0.001, 0.01]
LAG_THRESHOLDS = [0.01, 0.05, 0.1]


def median(values):
    values = sorted(values)
    if not values:
        return float('nan')
    middle = len(values) // 2
    return values[middle] if len(values) % 2 else (values[middle - 1] + values[middle]) / 2


def compare(results, configs):
    """Prints the report and returns True if everything passed."""
    ok = True
    names = [name for name, _, _ in configs]
    rounds = sorted({r['round'] for r in results})
    by_key = {(r['config'], r['round']): r for r in results}

    print('\n== failures')
    for name in names:
        runs = [r for r in results if r['config'] == name]
        errors = sum(len(r['load_errors']) for r in runs) + sum(
            len(s['errors']) for r in runs for s in r['streams'].values())
        requests = sum(r['requests'] for r in runs)
        late = sum(r['late_load_requests'] for r in runs)
        examples = [e for r in runs for e in r['load_errors']][:3] + [
            e for r in runs for s in r['streams'].values() for e in s['errors']][:3]
        print('  [%s] %-24s %d failed of %d requests%s%s' % (
            'PASS' if errors == 0 else 'FAIL', name, errors, requests, ', for example %s' % examples if examples else '',
            ', %d load requests were sent more than 1 s late' % late if late else ''))
        ok = ok and errors == 0

    def statistic(run, stream, key):
        if stream is None:
            return run[key]
        values = run['streams'][stream]
        if key == 'stalled' and key not in values:  # results saved by an older version
            return values['stalls'][0] / max(values['count'], 1)
        return values[key]

    def report(title, stream, key, unit, scale):
        nonlocal ok
        print('  %s' % title)
        base_name = names[0]
        for name in names:
            values = [statistic(by_key[(name, r)], stream, key) for r in rounds if (name, r) in by_key]
            line = '    %-24s median %9.3f %s' % (name, median(values) * scale, unit)
            if name != base_name and key in ALLOWED_INCREASE:
                differences = [statistic(by_key[(name, r)], stream, key) - statistic(by_key[(base_name, r)], stream, key)
                               for r in rounds if (name, r) in by_key and (base_name, r) in by_key]
                estimate, low, high = paired_difference(differences)
                base = median([statistic(by_key[(base_name, r)], stream, key) for r in rounds])
                relative, absolute = ALLOWED_INCREASE[key]
                allowed = max(relative * base, absolute)
                if high <= allowed:
                    verdict = 'PASS'
                elif low > allowed:
                    verdict = 'FAIL'
                    ok = False
                else:
                    verdict = 'INCONCLUSIVE'
                    ok = False
                line += ', difference %+.3f %s, 95%% CI [%+.3f, %+.3f], allowed +%.3f: %s' % (
                    estimate * scale, unit, low * scale, high * scale, allowed * scale, verdict)
            print(line)

    for stream in PROBE_STREAMS:
        method, _, period, _ = PROBE_STREAMS[stream]
        print('\n== %s probes: %s every %.0f ms, latency from the scheduled time' % (stream, method, period * 1000))
        for key in ['p50', 'p99', 'p99.9', 'max']:
            report(key, stream, key, 'ms', 1000)
        report('share of probes delayed by more than %g ms' % (STALL_THRESHOLDS[0] * 1000), stream, 'stalled', '%', 100)
        if all('own_stalled' in r['streams'][stream] for r in results):
            report('share of probes delayed by more than %g ms, while probes of the other servers weren\'t delayed' % (
                STALL_THRESHOLDS[0] * 1000), stream, 'own_stalled', '%', 100)
        print('  probes delayed by more than %s ms (all rounds)' % '/'.join('%g' % (x * 1000) for x in STALL_THRESHOLDS))
        for name in names:
            runs = [r for r in results if r['config'] == name]
            stalls = [sum(r['streams'][stream]['stalls'][i] for r in runs) for i in range(len(STALL_THRESHOLDS))]
            count = sum(r['streams'][stream]['count'] for r in runs)
            all_latencies = sorted(x for r in runs for x in r['streams'][stream]['latencies'])
            print('    %-24s %s of %d; pooled p99 %.3f ms, p99.9 %.3f ms, p99.99 %.3f ms, max %.3f ms' % (
                name, '/'.join(str(x) for x in stalls), count, percentile(all_latencies, 0.99) * 1000,
                percentile(all_latencies, 0.999) * 1000, percentile(all_latencies, 0.9999) * 1000,
                all_latencies[-1] * 1000))

    print('\n== server')
    report('CPU time per request', None, 'cpu_per_request', 'us', 1e6)
    report('peak RSS', None, 'peak_rss', 'MB', 1e-6)
    print('  operations with sampled allocations during measurements: total, longer than %s ms; max duration and '
          'max wait for the lock since the start' % '/'.join('%g' % (x * 1000) for x in SAMPLER_THRESHOLDS))
    for name in names:
        runs = [r for r in results if r['config'] == name]
        print('    %-24s %d, %s; max %.0f us, max wait %.0f us' % (
            name, sum(r['sampler_operations'] for r in runs),
            '/'.join(str(int(sum(r['slow_sampler_operations'][i] for r in runs))) for i in range(len(SAMPLER_THRESHOLDS))),
            max(r['server']['sampler_max_operation'] for r in runs) * 1e6,
            max(r['server']['sampler_max_lock_wait'] for r in runs) * 1e6))
    print('  delays of scheduler timer events during measurements longer than %s ms' % '/'.join(
        '%g' % (x * 1000) for x in LAG_THRESHOLDS))
    for name in names:
        runs = [r for r in results if r['config'] == name]
        if not all(r['has_scheduler_lag'] for r in runs):
            print('    %-24s not measured' % name)
            continue
        by_scheduler = {}
        for r in runs:
            for scheduler, counts in r['scheduler_lags'].items():
                by_scheduler[scheduler] = [a + b for a, b in zip(by_scheduler.get(scheduler, [0] * len(counts)), counts)]
        total = [sum(counts[i] for counts in by_scheduler.values()) for i in range(len(LAG_THRESHOLDS))]
        print('    %-24s %s%s' % (name, '/'.join(str(int(x)) for x in total), ' (by scheduler: %s)' % ', '.join(
            '%s: %s' % (scheduler, '/'.join(str(int(x)) for x in counts))
            for scheduler, counts in sorted(by_scheduler.items(), key=lambda item: int(item[0]))) if by_scheduler else ''))
    return ok


def parse_options(args):
    options = {'rounds': '8', 'duration': '20', 'load-rate': '2000', 'load-connections': '64', 'load-processes': '2',
               'json': '', 'analyze': '', 'concurrent': '0'}
    configs = []
    positional = []
    for arg in args:
        if arg.startswith('--config='):
            name, _, value = arg[len('--config='):].partition('=')
            parts = value.split(',')
            configs.append((name, os.path.abspath(parts[0]), parts[1:]))
        elif arg.startswith('--') and '=' in arg:
            name, value = arg[2:].split('=', 1)
            if name not in options:
                raise SystemExit('unknown option --%s' % name)
            options[name] = value
        else:
            positional.append(arg)
    return options, configs, positional


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--worker':
        worker_main(sys.argv[2:])
        return
    options, configs, positional = parse_options(sys.argv[1:])
    if options['analyze']:
        with open(options['analyze']) as f:
            results = json.load(f)
        names = [r['config'] for r in results if r['round'] == 0]
        ok = compare(results, [(name, None, None) for name in names])
        print('\nALL CHECKS PASSED' if ok else '\nSOME CHECKS DID NOT PASS')
        sys.exit(0 if ok else 1)
    if len(positional) != 1 or len(configs) < 2:
        print(__doc__)
        sys.exit(2)
    sys.dont_write_bytecode = True
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'restart'))
    import restart_test as t
    t.BUILD_DIR = os.path.abspath(positional[0])
    t.WORK_ROOT = tempfile.mkdtemp(prefix='telegram-bot-api-latency-ab-')
    signal.signal(signal.SIGINT, signal.default_int_handler)
    results = []
    for round_index in range(int(options['rounds'])):
        if options['concurrent'] == '1':
            # configurations use the ports in turn
            shift = round_index % len(configs)
            order = configs[shift:] + configs[:shift]
            round_results = run_together([ServerRun(t, name, binary, args, i, round_index, options)
                                          for i, (name, binary, args) in enumerate(order)])
            mark_own_stalls(round_results)
        else:
            order = configs if round_index % 2 == 0 else configs[::-1]
            round_results = [run_together([ServerRun(t, name, binary, args, 0, round_index, options)])[0]
                             for name, binary, args in order]
        for result in round_results:
            name = result['config']
            results.append(result)
            print('round %d %-24s requests %6d, cpu %.1f us/request, %s, failures %d' % (
                round_index + 1, name, result['requests'], result['cpu_per_request'] * 1e6, ', '.join(
                    '%s p50 %.2f p99 %.2f p99.9 %.2f max %.1f ms' % (
                        stream, s['p50'] * 1000, s['p99'] * 1000, s['p99.9'] * 1000, s['max'] * 1000)
                    for stream, s in result['streams'].items()),
                len(result['load_errors']) + sum(len(s['errors']) for s in result['streams'].values())), flush=True)
    if options['json']:
        with open(options['json'], 'w') as f:
            json.dump(results, f)
        print('raw results are saved to %s' % options['json'])
    ok = compare(results, configs)
    print('\nALL CHECKS PASSED' if ok else '\nSOME CHECKS DID NOT PASS')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
