#!/usr/bin/env python3
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
"""A/B test of latency, stalls, failures and CPU usage of server binaries or options, for example, of a server with
and without memory accounting.

In every round the servers of all configurations run at the same time with two offline test bots and get the same
load. A pause of the whole machine (for example, of a virtual machine by its host) delays all of them at the same time,
so paired comparison of rounds cancels it out. The ports of the configurations alternate between rounds.
 - Load: requests with a fixed total rate over many keep-alive connections; a mix of methods answered by Bot API
   (getMe, getWebhookInfo), by TDLib (getChatMember, getFile) and requests with 100 KB bodies.
 - Probes: requests sent by a fixed schedule; latency is measured from the scheduled time, so a stall of a server
   delays every probe scheduled during the stall. The "client" probe is getMe, answered by Bot API; the "tdlib" probe
   is getChatMember, answered by TDLib. A probe is "stalled" if it is delayed by more than 10 ms, and "own stalled" if
   at the same time no probe of another server was stalled, i.e. the stall wasn't caused by the machine.
A request fails if it isn't answered with the expected HTTP status code in 10 seconds.

For every statistic the report shows the median over the rounds; for every configuration except the first one it
shows the difference from the first configuration over paired rounds: the Hodges-Lehmann estimate and the 95%
confidence interval of the Wilcoxon signed-rank test, which a single disturbed round can't spoil. At least 6 rounds
are needed.
 PASS - the upper bound of the interval doesn't exceed the allowed increase, i.e. a bigger increase is rejected at
        the significance level of 2.5%;
 FAIL - the lower bound exceeds the allowed increase;
 INCONCLUSIVE - the noise is too big to decide; more rounds are needed.
Servers share the CPU, so extra CPU usage of one server slows down the others a little; on a machine with few cores
this makes differences of latency smaller. The CPU time per request is compared directly.

Exit code: 0 if no requests failed and all comparisons passed, 1 otherwise, 2 for invalid arguments.
"""
import argparse
import asyncio
import bisect
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import testlib  # noqa: E402

PROBE_STREAMS = {
    # name: (method, parameters, period in seconds, number of connections)
    'client': ('getMe', {}, 0.002, 8),
    'tdlib': ('getChatMember', {'chat_id': testlib.BOT_ID, 'user_id': testlib.BOT_ID}, 0.005, 8),
}
STALL_THRESHOLD = 0.01
COINCIDENCE_WINDOW = 0.2  # stalls of different servers scheduled at most this far apart are caused by the machine
REQUEST_TIMEOUT = 10.0
WARM_UP_DURATION = 3.0
SAMPLER_THRESHOLDS = [0.0001, 0.001]
LAG_THRESHOLDS = [0.01, 0.1]

# statistic: (allowed relative increase, allowed absolute increase); the bigger one is used
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
    """A keep-alive HTTP connection, which is reopened after an error."""

    def __init__(self, port):
        self.port = port
        self.writer = None

    async def request(self, data):
        try:
            if self.writer is None:
                self.reader, self.writer = await asyncio.wait_for(
                    asyncio.open_connection('127.0.0.1', self.port), REQUEST_TIMEOUT)
            self.writer.write(data)
            return await asyncio.wait_for(read_response(self.reader), REQUEST_TIMEOUT)
        except BaseException:
            if self.writer is not None:
                self.writer.close()
                self.writer = None
            raise


async def run_probes(port, duration):
    """Sends probes by the schedule; returns {stream: {'latencies', 'errors', 'stall_times'}}."""
    results = {name: {'latencies': [], 'errors': [], 'stall_times': []} for name in PROBE_STREAMS}
    loop = asyncio.get_running_loop()
    start = loop.time() + 0.1

    async def probe(name, index):
        method, params, period, connection_count = PROBE_STREAMS[name]
        connection = Connection(port)
        data = make_request(testlib.TOKEN, method, params)
        scheduled = start + period * index
        # probes scheduled during a stall at the end are sent too, so the stall is fully counted
        while scheduled < start + duration:
            delay = scheduled - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)
            try:
                status = await connection.request(data)
                if status != 200:
                    results[name]['errors'].append('%s: HTTP %d' % (method, status))
            except Exception as e:
                results[name]['errors'].append('%s: %r' % (method, e))
            latency = loop.time() - scheduled
            results[name]['latencies'].append(latency)
            if latency > STALL_THRESHOLD:
                # monotonic clock time, which is comparable between processes
                results[name]['stall_times'].append(scheduled)
            scheduled += period * connection_count

    await asyncio.gather(*[probe(name, i) for name, stream in PROBE_STREAMS.items() for i in range(stream[3])])
    return results


def load_requests():
    """Returns the mix of load requests as [(request, expected HTTP status code, weight)]."""
    result = []
    for token, bot_id in testlib.BOTS:
        result += [
            (make_request(token, 'getMe', {}), 200, 40),
            (make_request(token, 'getWebhookInfo', {}), 200, 30),
            (make_request(token, 'getChatMember', {'chat_id': bot_id, 'user_id': bot_id}), 200, 15),
            (make_request(token, 'getFile', {'file_id': 'AgADBAAD' + 'A' * 40}), 400, 10),
            (make_request(token, 'getWebhookInfo', {'padding': 'x' * 100000}), 200, 5),
        ]
    return result


async def run_load(port, duration, rate, connection_count, seed):
    """Sends requests with the given total rate; returns the number of requests and failures."""
    requests = load_requests()
    weights = [weight for _, _, weight in requests]
    rng = random.Random(seed)
    loop = asyncio.get_running_loop()
    start = loop.time() + 0.1
    count = 0
    errors = []

    async def worker(index):
        nonlocal count
        connection = Connection(port)
        scheduled = start + index / rate
        while scheduled < start + duration:
            delay = scheduled - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)
            data, expected_status, _ = rng.choices(requests, weights)[0]
            method = data.split(b' ', 2)[1].rsplit(b'/', 1)[1].decode()
            try:
                status = await connection.request(data)
                if status != expected_status:
                    errors.append('%s: HTTP %d' % (method, status))
            except Exception as e:
                errors.append('%s: %r' % (method, e))
            count += 1
            scheduled += connection_count / rate

    await asyncio.gather(*[worker(i) for i in range(connection_count)])
    return {'count': count, 'errors': errors}


def worker_main(args):
    """Entry point of load and probe processes; prints results as JSON."""
    kind, port, duration = args[0], int(args[1]), float(args[2])
    if kind == 'probe':
        result = asyncio.run(run_probes(port, duration))
    else:
        result = asyncio.run(run_load(port, duration, float(args[3]), int(args[4]), int(args[5])))
    json.dump(result, sys.stdout)


def wilcoxon_critical_value(n, alpha=0.05):
    """Returns the largest t, such that P(W <= t) <= alpha / 2 for the Wilcoxon signed-rank statistic W of n pairs,
    or -1 if there is no such t. The null distribution is counted exactly: every subset of ranks is equally likely."""
    counts = [1] + [0] * (n * (n + 1) // 2)  # counts[s] - number of subsets of the ranks 1..n with the sum s
    for rank in range(1, n + 1):
        for s in range(len(counts) - 1, rank - 1, -1):
            counts[s] += counts[s - rank]
    t = -1
    cumulative = 0
    for s, count in enumerate(counts):
        cumulative += count
        if cumulative > alpha / 2 * 2 ** n:
            break
        t = s
    return t


def paired_difference(differences):
    """Returns the Hodges-Lehmann estimate of the median difference and its 95% confidence interval, which is formed by
    the order statistics of the Walsh averages, as in the Wilcoxon signed-rank test."""
    n = len(differences)
    walsh = sorted((differences[i] + differences[j]) / 2 for i in range(n) for j in range(i, n))
    t = wilcoxon_critical_value(n)
    if t < 0:
        return testlib.median(walsh), float('-inf'), float('inf')
    return testlib.median(walsh), walsh[t], walsh[len(walsh) - 1 - t]


def histogram_excess(before, after, name, thresholds):
    """Returns the number of observations of a histogram above each threshold between two scrapes."""
    def bucket_counts(values):
        result = {}
        for (metric_name, labels), value in values.items():
            if metric_name == name + '_bucket':
                labels = dict(labels)
                le = float(labels.pop('le'))
                key = tuple(sorted(labels.items()))
                result[key, le] = result.get((key, le), 0) + value
        return result

    old = bucket_counts(before)
    new = bucket_counts(after)
    keys = {key for key, _ in new}
    return [sum((new[key, float('inf')] - old.get((key, float('inf')), 0)) -
                (new[key, threshold] - old.get((key, threshold), 0)) for key in keys) for threshold in thresholds]


class ServerRun:
    """A server of one configuration in one round."""

    def __init__(self, name, binary, args, work, round_index, options):
        self.name = name
        self.server = testlib.Server(binary, work, args)
        self.round_index = round_index
        self.options = options
        self.workers = []

    def spawn(self, *args):
        process = subprocess.Popen([sys.executable, os.path.abspath(__file__), '--worker'] + [str(a) for a in args],
                                   stdout=subprocess.PIPE)
        self.workers.append(process)
        return process

    def start_load(self, duration, seed):
        processes = self.options.load_processes
        return [self.spawn('load', self.server.api_port, duration, self.options.load_rate / processes,
                           self.options.load_connections // processes, seed + i) for i in range(processes)]

    def begin(self):
        self.metrics_before = self.server.metrics()
        self.cpu_before = self.server.cpu_seconds()
        duration = self.options.duration
        return self.start_load(duration, self.round_index * 100) + [self.spawn('probe', self.server.api_port, duration)]

    def finish(self, outputs):
        cpu = self.server.cpu_seconds() - self.cpu_before
        metrics = self.server.metrics()
        before = self.metrics_before
        probes = outputs[-1]
        loads = outputs[:-1]
        request_count = sum(load['count'] for load in loads) + sum(len(p['latencies']) for p in probes.values())
        result = {
            'config': self.name,
            'round': self.round_index,
            'requests': request_count,
            'errors': [e for load in loads for e in load['errors']] + [e for p in probes.values() for e in p['errors']],
            'cpu_per_request': cpu / request_count,
            'peak_rss': self.server.peak_rss(),
            'sampler_operations': testlib.metric_sum(metrics, 'tgbotapi_memory_sampler_operation_seconds_count') -
                                  testlib.metric_sum(before, 'tgbotapi_memory_sampler_operation_seconds_count'),
            'slow_sampler_operations': histogram_excess(before, metrics, 'tgbotapi_memory_sampler_operation_seconds',
                                                        SAMPLER_THRESHOLDS),
            'sampler_max_lock_wait': testlib.metric_sum(metrics, 'tgbotapi_memory_sampler_max_lock_wait_seconds'),
            'scheduler_lags': histogram_excess(before, metrics, 'tgbotapi_scheduler_lag_seconds', LAG_THRESHOLDS),
            'streams': {},
        }
        for name, probe in probes.items():
            latencies = sorted(probe['latencies'])
            result['streams'][name] = {
                'count': len(latencies),
                'p50': testlib.percentile(latencies, 0.5),
                'p99': testlib.percentile(latencies, 0.99),
                'p99.9': testlib.percentile(latencies, 0.999),
                'max': latencies[-1] if latencies else float('nan'),
                'stall_times': probe['stall_times'],
                'stalled': len(probe['stall_times']) / max(len(latencies), 1),
            }
        return result


def collect(processes, timeout):
    return [json.loads(process.communicate(timeout=timeout)[0]) for process in processes]


def run_round(configs, round_index, options, work_root):
    """Runs the servers of all configurations at the same time; returns their results."""
    shift = round_index % len(configs)
    runs = [ServerRun(name, binary, args, os.path.join(work_root, 'server%d' % i), round_index, options)
            for i, (name, binary, args) in enumerate(configs[shift:] + configs[:shift])]
    try:
        for run in runs:
            testlib.prepare_work_dir(options.build, run.server.work)
            run.server.start()
            for token, _ in testlib.BOTS:
                result, _ = run.server.api('getMe', token=token)
                assert result.get('ok'), result
        collect([process for run in runs for process in run.start_load(WARM_UP_DURATION, 0)], WARM_UP_DURATION + 60)
        processes = [run.begin() for run in runs]
        outputs = [collect(run_processes, options.duration + 60) for run_processes in processes]
        results = [run.finish(output) for run, output in zip(runs, outputs)]
    except BaseException:
        for run in runs:
            for process in run.workers:
                if process.poll() is None:
                    process.kill()
            run.server.__exit__(Exception, None, None)  # kills the server
        raise
    for run in runs:
        run.server.stop()
    mark_own_stalls(results)
    return results


def mark_own_stalls(results):
    """Counts stalled probes of each server, when no probe of the other servers of the round was stalled."""
    for result in results:
        others = sorted(t for other in results if other is not result for stream in other['streams'].values()
                        for t in stream['stall_times'])
        for stream in result['streams'].values():
            own = 0
            for t in stream['stall_times']:
                position = bisect.bisect_left(others, t - COINCIDENCE_WINDOW)
                if position == len(others) or others[position] > t + COINCIDENCE_WINDOW:
                    own += 1
            stream['own_stalled'] = own / max(stream['count'], 1)


def compare(results):
    """Prints the report; returns True if no requests failed and all comparisons passed."""
    names = [r['config'] for r in results if r['round'] == 0]  # in the order of the command line
    base_name = names[0]
    by_key = {(r['config'], r['round']): r for r in results}
    rounds = sorted({r['round'] for r in results})
    ok = True

    print('\n== failures')
    for name in names:
        runs = [r for r in results if r['config'] == name]
        errors = [e for r in runs for e in r['errors']]
        print('  [%s] %-16s %d failed of %d requests%s' % ('PASS' if not errors else 'FAIL', name, len(errors),
                                                          sum(r['requests'] for r in runs),
                                                          ', for example %s' % errors[:3] if errors else ''))
        ok = ok and not errors

    def value(run, stream, key):
        return run[key] if stream is None else run['streams'][stream][key]

    def report(title, stream, key, unit, scale):
        nonlocal ok
        print('  %s' % title)
        for name in names:
            values = [value(by_key[name, r], stream, key) for r in rounds if (name, r) in by_key]
            line = '    %-16s median %9.3f %s' % (name, testlib.median(values) * scale, unit)
            if name != base_name and key in ALLOWED_INCREASE:
                paired = [r for r in rounds if (name, r) in by_key and (base_name, r) in by_key]
                estimate, low, high = paired_difference(
                    [value(by_key[name, r], stream, key) - value(by_key[base_name, r], stream, key) for r in paired])
                base = testlib.median([value(by_key[base_name, r], stream, key) for r in paired])
                relative, absolute = ALLOWED_INCREASE[key]
                allowed = max(relative * base, absolute)
                verdict = 'PASS' if high <= allowed else 'FAIL' if low > allowed else 'INCONCLUSIVE'
                ok = ok and verdict == 'PASS'
                line += ', difference %+.3f %s, 95%% CI [%+.3f, %+.3f], allowed +%.3f: %s' % (
                    estimate * scale, unit, low * scale, high * scale, allowed * scale, verdict)
            print(line)

    for stream, (method, _, period, _) in PROBE_STREAMS.items():
        print('\n== %s probes: %s every %g ms, latency from the scheduled time' % (stream, method, period * 1000))
        for key in ['p50', 'p99', 'p99.9', 'max']:
            report(key, stream, key, 'ms', 1000)
        report('share of probes delayed by more than %g ms' % (STALL_THRESHOLD * 1000), stream, 'stalled', '%', 100)
        report('the same, when no probe of another server was delayed', stream, 'own_stalled', '%', 100)

    print('\n== server')
    report('CPU time per request', None, 'cpu_per_request', 'us', 1e6)
    report('peak RSS', None, 'peak_rss', 'MB', 1e-6)
    for name in names:
        runs = [r for r in results if r['config'] == name]
        print('  %-16s sampled allocation operations %d, longer than %s ms: %s; max lock wait %.0f us; '
              'scheduler timer delays longer than %s ms: %s' % (
                  name, sum(r['sampler_operations'] for r in runs),
                  '/'.join('%g' % (x * 1000) for x in SAMPLER_THRESHOLDS),
                  '/'.join('%d' % sum(r['slow_sampler_operations'][i] for r in runs)
                           for i in range(len(SAMPLER_THRESHOLDS))),
                  max(r['sampler_max_lock_wait'] for r in runs) * 1e6,
                  '/'.join('%g' % (x * 1000) for x in LAG_THRESHOLDS),
                  '/'.join('%d' % sum(r['scheduler_lags'][i] for r in runs) for i in range(len(LAG_THRESHOLDS)))))
    return ok


def parse_config(text):
    name, _, value = text.partition('=')
    parts = value.split(',')
    if not name or not parts[0]:
        raise argparse.ArgumentTypeError('expected NAME=BINARY[,ARGUMENT]...')
    return name, os.path.abspath(parts[0]), parts[1:]


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--worker':
        return worker_main(sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', help='build directory with restart-test-fake-auth')
    parser.add_argument('--config', type=parse_config, action='append', default=[],
                        help='NAME=BINARY[,ARGUMENT]...; the first configuration is the baseline')
    parser.add_argument('--rounds', type=int, default=40)
    parser.add_argument('--duration', type=float, default=10.0, help='seconds of measurement in each round')
    parser.add_argument('--load-rate', type=float, default=2000.0, help='requests per second to each server')
    parser.add_argument('--load-connections', type=int, default=64)
    parser.add_argument('--load-processes', type=int, default=2)
    parser.add_argument('--json', help='file to save raw results to')
    parser.add_argument('--analyze', help='file with raw results to report instead of running servers')
    options = parser.parse_args()
    if options.analyze:
        with open(options.analyze) as f:
            results = json.load(f)
    else:
        if not options.build or len(options.config) < 2:
            parser.error('--build and at least two --config are needed')
        work_root = tempfile.mkdtemp(prefix='telegram-bot-api-latency-ab-')
        results = []
        try:
            for round_index in range(options.rounds):
                for result in run_round(options.config, round_index, options, work_root):
                    results.append(result)
                    print('round %d %-16s requests %6d, failures %d, cpu %.1f us/request, %s' % (
                        round_index + 1, result['config'], result['requests'], len(result['errors']),
                        result['cpu_per_request'] * 1e6, ', '.join(
                            '%s p50 %.2f p99 %.2f max %.1f ms' % (stream, s['p50'] * 1e3, s['p99'] * 1e3, s['max'] * 1e3)
                            for stream, s in result['streams'].items())), flush=True)
        finally:
            shutil.rmtree(work_root, ignore_errors=True)
        if options.json:
            with open(options.json, 'w') as f:
                json.dump(results, f)
    ok = compare(results)
    print('\nALL CHECKS PASSED' if ok else '\nSOME CHECKS DID NOT PASS')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
