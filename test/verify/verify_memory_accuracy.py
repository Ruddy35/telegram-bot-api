#!/usr/bin/env python3
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
"""Checks that per-bot memory estimates of a server have the error, which the server reports.

The server must run with the environment variable TELEGRAM_BOT_API_MEMORY_AUDIT=1. Then it exports the exact memory of
every bot (tgbotapi_bot_memory_exact_bytes) next to the estimate (tgbotapi_bot_memory_bytes) and its standard deviation
(tgbotapi_bot_memory_error_bytes). Sampling doesn't depend on the mode, so the estimates are the same as without it.

For every bot with at least --min-samples expected samples (exact memory / sampling interval; the selection doesn't
depend on the random sampling) the check computes z = (estimate - exact) / reported standard deviation. Estimates of
different bots are independent, so if the reported error is right, z are approximately standard normal:
 - the exact value is within 1, 2 and 3 standard deviations for 68.3%, 95.4% and 99.7% of bots; the observed share
   must be at least the expected one minus 3 standard deviations of a binomial distribution minus 1%;
 - mean z is within +-3 / sqrt(n): the estimates are unbiased;
 - standard deviation of z is at most 1 + 3 / sqrt(2 n) + 0.05: the error isn't understated;
 - z of the estimate of the whole heap is within +-4 in every scrape.
Consecutive scrapes of a bot are correlated, so in the watch mode n is the smallest number of bots in a scrape.

Exit code: 0 if all checks passed, 1 otherwise, 2 for invalid arguments.
"""
import argparse
import asyncio
import csv
import json
import math
import os
import random
import shutil
import sys
import tempfile
import time
import urllib.request

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import testlib  # noqa: E402

MAX_HEAP_Z = 4.0


def get_observations(values, min_samples):
    """Returns (observations of bots, number of skipped bots, observation of the whole heap or None)."""
    interval = testlib.metric_sum(values, 'tgbotapi_memory_sampling_interval_bytes')
    bots = {}
    for (name, labels), value in values.items():
        labels = dict(labels)
        if 'bot' in labels and name.startswith('tgbotapi_bot_memory_'):
            bot = bots.setdefault(labels['bot'], {'bot': labels['bot'], 'estimate': 0.0, 'exact': None,
                                                  'sigma': 0.0, 'samples': 0})
            if name == 'tgbotapi_bot_memory_bytes':
                bot['estimate'] += value
            elif name == 'tgbotapi_bot_memory_exact_bytes':
                bot['exact'] = (bot['exact'] or 0.0) + value
            elif name == 'tgbotapi_bot_memory_error_bytes':
                bot['sigma'] = value
            elif name == 'tgbotapi_bot_memory_samples':
                bot['samples'] = value
    if any(bot['exact'] is None for bot in bots.values()):
        raise SystemExit('the server doesn\'t export exact memory of bots; run it with TELEGRAM_BOT_API_MEMORY_AUDIT=1')
    observations = [bot for _, bot in sorted(bots.items())
                    if interval > 0 and bot['exact'] >= min_samples * interval and bot['sigma'] > 0]
    heap = None
    if ('tgbotapi_heap_audit_bytes', frozenset()) in values:
        heap = {'estimate': testlib.metric_sum(values, 'tgbotapi_heap_estimated_bytes'),
                'exact': testlib.metric_sum(values, 'tgbotapi_heap_audit_bytes'),
                'sigma': testlib.metric_sum(values, 'tgbotapi_heap_estimate_error_bytes')}
    return observations, len(bots) - len(observations), heap


def z_score(o):
    return (o['estimate'] - o['exact']) / o['sigma']


class Report:
    """Accumulates groups of observations; observations of a group are independent."""

    def __init__(self, title):
        self.title = title
        self.observations = []
        self.min_group_size = None
        self.heap_z = []

    def add_group(self, observations, heap):
        self.observations.extend(observations)
        if observations and (self.min_group_size is None or len(observations) < self.min_group_size):
            self.min_group_size = len(observations)
        if heap is not None and heap['sigma'] > 0:
            self.heap_z.append(z_score(heap))

    def check(self, independent_groups):
        """Prints the report; returns True if all checks passed. Observations of different groups are independent if
        independent_groups is True."""
        print('\n== %s' % self.title)
        n = len(self.observations)
        if n == 0:
            print('  [FAIL] no bots with enough memory')
            return False
        n_eff = n if independent_groups else self.min_group_size
        z = [z_score(o) for o in self.observations]
        mean = sum(z) / n
        stddev = math.sqrt(sum((x - mean) ** 2 for x in z) / max(n - 1, 1))
        print('  observations: %d, independent: %d' % (n, n_eff))
        results = []
        for k in [1, 2, 3]:
            covered = sum(1 for x in z if abs(x) <= k) / n
            p = math.erf(k / math.sqrt(2))
            minimum = p - 3 * math.sqrt(p * (1 - p) / n_eff) - 0.01
            results.append((covered >= minimum, 'exact value within %d sigma: %.1f%% (normal distribution %.1f%%, '
                                                'minimum %.1f%%)' % (k, covered * 100, p * 100, minimum * 100)))
        bound = 3 / math.sqrt(n_eff)
        results.append((abs(mean) <= bound, 'mean z %+.3f, allowed +-%.3f' % (mean, bound)))
        max_stddev = 1 + 3 / math.sqrt(2 * n_eff) + 0.05
        results.append((stddev <= max_stddev, 'standard deviation of z %.3f, allowed at most %.3f' % (stddev, max_stddev)))
        if self.heap_z:
            max_heap_z = max(abs(x) for x in self.heap_z)
            results.append((max_heap_z <= MAX_HEAP_Z, 'whole heap: max |z| %.2f in %d scrapes, allowed at most %g' % (
                max_heap_z, len(self.heap_z), MAX_HEAP_Z)))
        for passed, text in results:
            print('  [%s] %s' % ('PASS' if passed else 'FAIL', text))
        if stddev < 0.7:
            print('  [WARN] standard deviation of z is less than 0.7: the reported error is overstated')
        errors = sorted(abs(o['estimate'] - o['exact']) / o['exact'] for o in self.observations)
        print('  max |z| %.2f; relative error: median %.1f%%, p95 %.1f%%, max %.1f%%; total error %+.2f%% of %.1f MB' % (
            max(abs(x) for x in z), testlib.percentile(errors, 0.5) * 100, testlib.percentile(errors, 0.95) * 100,
            errors[-1] * 100, sum(o['estimate'] - o['exact'] for o in self.observations) /
            sum(o['exact'] for o in self.observations) * 100, sum(o['exact'] for o in self.observations) / 1e6))
        return all(passed for passed, _ in results)


def write_csv(path, rows):
    if path:
        with open(path, 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['group', 'bot', 'samples', 'estimate', 'exact', 'sigma', 'z'])
            for group, o in rows:
                writer.writerow([group, o['bot'], int(o['samples']), int(o['estimate']), int(o['exact']),
                                 round(o['sigma']), '%.4f' % z_score(o)])


def watch(options):
    report = Report('server %s, %g s' % (options.url, options.duration))
    rows = []
    deadline = time.time() + options.duration
    scrape = 0
    while True:
        with urllib.request.urlopen(options.url, timeout=30) as r:
            observations, skipped, heap = get_observations(testlib.parse_metrics(r.read().decode()),
                                                           options.min_samples)
        scrape += 1
        report.add_group(observations, heap)
        rows.extend((scrape, o) for o in observations)
        z = [z_score(o) for o in observations]
        print('scrape %d: %d bots (%d skipped), within 1/2/3 sigma %s, max |z| %.2f' % (
            scrape, len(z), skipped, '/'.join('%.0f%%' % (sum(1 for x in z if abs(x) <= k) / max(len(z), 1) * 100)
                                              for k in [1, 2, 3]), max([abs(x) for x in z] or [0])), flush=True)
        if time.time() + options.period > deadline:
            break
        time.sleep(options.period)
    write_csv(options.csv, rows)
    return report.check(independent_groups=False)


async def send_without_waiting(port, requests):
    """Sends requests without waiting for responses; requests, which need network access, stay in memory."""
    semaphore = asyncio.Semaphore(100)

    async def send(token, method, params):
        async with semaphore:
            _, writer = await asyncio.wait_for(asyncio.open_connection('127.0.0.1', port), 10)
            body = json.dumps(params).encode()
            writer.write(b'POST /bot%s/%s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n'
                         b'Content-Length: %d\r\n\r\n' % (token.encode(), method.encode(), len(body)) + body)
            await writer.drain()
            writer.close()

    await asyncio.gather(*[send(*request) for request in requests])


def run_offline_server(options, interval, run, rng, work):
    """Runs one offline server, gives its bots random amounts of memory and returns its observations."""
    testlib.prepare_work_dir(options.build, work)
    server = testlib.Server(os.path.join(options.build, 'telegram-bot-api'), work,
                            ['--memory-sampling-interval=%d' % interval], env={'TELEGRAM_BOT_API_MEMORY_AUDIT': '1'})
    with server:
        for token, _ in testlib.BOTS:
            assert server.api('getMe', token=token)[0]['ok']
        if run % 2 == 1:
            # memory of the first TDLib instance becomes unattributed; the new instance is accounted from scratch
            server.stats('?restart=%d' % testlib.BOT_ID)
            testlib.wait_for(lambda: int(server.stats().get('tdlib_restart_count', 0)) >= 1, 60, 'restart')
        # messages can't be sent without network access, so they stay in memory of the bots;
        # from 10 to 20000 messages of random size make from 1 to tens of megabytes
        requests = [(token, 'sendMessage', {'chat_id': bot_id, 'text': 'x' * rng.randrange(1, 4000)})
                    for token, bot_id in testlib.BOTS for _ in range(int(10 ** rng.uniform(1, 4.3)))]
        rng.shuffle(requests)
        asyncio.run(send_without_waiting(server.api_port, requests))

        def exact_memory():
            return [testlib.metric_sum(server.metrics(), 'tgbotapi_bot_memory_exact_bytes', bot=bot_id)
                    for _, bot_id in testlib.BOTS]

        last = exact_memory()
        for _ in range(120):
            time.sleep(0.5)
            current = exact_memory()
            if current == last:
                break
            last = current
        else:
            print('warning: memory of the bots still changes after 60 seconds')
        return get_observations(server.metrics(), options.min_samples)


def offline(options):
    rng = random.Random(options.seed)
    work_root = tempfile.mkdtemp(prefix='telegram-bot-api-verify-memory-')
    ok = True
    rows = []
    try:
        for interval in options.intervals:
            report = Report('offline servers, sampling interval %d KB, %d runs' % (interval, options.runs))
            for run in range(options.runs):
                work = os.path.join(work_root, '%d-%d' % (interval, run))
                observations, _, heap = run_offline_server(options, interval, run, rng, work)
                shutil.rmtree(work, ignore_errors=True)
                report.add_group(observations, heap)
                rows.extend(('%dKB-%d' % (interval, run), o) for o in observations)
                print('run %d: %s' % (run + 1, ', '.join('bot %s %.1f MB z %+.2f' % (
                    o['bot'], o['exact'] / 1e6, z_score(o)) for o in observations)), flush=True)
            ok = report.check(independent_groups=True) and ok
    finally:
        shutil.rmtree(work_root, ignore_errors=True)
    write_csv(options.csv, rows)
    return ok


def main():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--min-samples', type=float, default=30, help='minimum expected number of samples of a bot')
    common.add_argument('--csv', help='file to save observations to')
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    modes = parser.add_subparsers(dest='mode', required=True)
    watch_parser = modes.add_parser('watch', parents=[common],
                                    help='check a running server, for example, a canary with real bots')
    watch_parser.add_argument('url', help='metrics URL, for example, http://127.0.0.1:8082/metrics')
    watch_parser.add_argument('--duration', type=float, default=600.0)
    watch_parser.add_argument('--period', type=float, default=15.0)
    offline_parser = modes.add_parser('offline', parents=[common], help='check independent offline servers with test bots')
    offline_parser.add_argument('build', help='build directory with telegram-bot-api and restart-test-fake-auth')
    offline_parser.add_argument('--runs', type=int, default=30, help='servers for each sampling interval')
    offline_parser.add_argument('--intervals', type=lambda s: [int(x) for x in s.split(',')], default=[64, 16],
                                help='sampling intervals in kilobytes')
    offline_parser.add_argument('--seed', type=int, default=1)
    options = parser.parse_args()
    ok = watch(options) if options.mode == 'watch' else offline(options)
    print('\nALL CHECKS PASSED' if ok else '\nSOME CHECKS FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
