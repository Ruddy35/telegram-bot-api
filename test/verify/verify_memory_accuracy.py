#!/usr/bin/env python3
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
"""Checks that per-bot memory estimates of the server have the error, which the server reports.

The server must be started with the environment variable TELEGRAM_BOT_API_MEMORY_AUDIT=1. In this mode every memory
allocation also gets a 16-byte header with its owner, and the server exports the exact memory of every bot
(tgbotapi_bot_memory_exact_bytes) next to the estimate (tgbotapi_bot_memory_bytes) and its estimated standard deviation
(tgbotapi_bot_memory_error_bytes). Sampling decisions and weights don't depend on the mode, so the estimates have the
same distribution as without it.

For every bot with enough sampled allocations the check computes z = (estimate - exact) / reported standard deviation.
Estimates of different bots are made from different allocations and are independent, so if the reported error is right,
z of the bots of one scrape are a sample of an approximately standard normal distribution:
 - the exact value is within 1, 2 and 3 reported standard deviations from the estimate for 68.3%, 95.4% and 99.7% of bots;
 - mean z is close to 0, i.e. the estimates are unbiased;
 - standard deviation of z is close to 1, i.e. the error isn't understated.
Consecutive scrapes of the same bot are correlated, so tolerances are computed for the smallest number of bots in a
scrape.

Usage:
  verify_memory_accuracy.py watch <metrics URL> [--duration=600] [--period=15] [--min-samples=30] [--csv=<file>]
      checks a running server, for example, a canary instance with real bots
  verify_memory_accuracy.py offline <build dir> [--runs=30] [--intervals=64,16] [--min-samples=30] [--csv=<file>]
      starts independent offline servers with two test bots, which get random amounts of memory, and checks them;
      every run is a new process with new random sampling, so all observations are independent

Exit code is 0 if all checks passed.
"""
import asyncio
import csv
import json
import math
import os
import random
import sys
import tempfile
import time
import urllib.request

PARTS = ['client', 'tdlib', 'updates']


def parse_metrics(text):
    """Returns metrics as {(name, frozenset of labels): value}."""
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


def fetch_metrics(url):
    with urllib.request.urlopen(url, timeout=30) as r:
        return parse_metrics(r.read().decode())


def get_observations(values, min_samples):
    """Returns observations of bots and of the whole heap from one scrape."""
    bots = {}
    for (name, labels), value in values.items():
        labels = dict(labels)
        if 'bot' not in labels:
            continue
        bot = bots.setdefault(labels['bot'], {'estimate': 0.0, 'exact': 0.0, 'sigma': None, 'samples': None,
                                              'parts': {}, 'has_exact': False})
        if name == 'tgbotapi_bot_memory_bytes':
            bot['estimate'] += value
            bot['parts'].setdefault(labels['part'], [0.0, 0.0])[0] = value
        elif name == 'tgbotapi_bot_memory_exact_bytes':
            bot['exact'] += value
            bot['has_exact'] = True
            bot['parts'].setdefault(labels['part'], [0.0, 0.0])[1] = value
        elif name == 'tgbotapi_bot_memory_error_bytes':
            bot['sigma'] = value
        elif name == 'tgbotapi_bot_memory_samples':
            bot['samples'] = value
    observations = []
    skipped = 0
    for bot_id, bot in sorted(bots.items()):
        if bot['sigma'] is None or bot['samples'] is None:
            continue
        if not bot['has_exact']:
            raise SystemExit('the server doesn\'t export exact memory of bots; start it with '
                             'TELEGRAM_BOT_API_MEMORY_AUDIT=1')
        if bot['samples'] < min_samples or bot['sigma'] <= 0:
            skipped += 1
            continue
        observations.append({'bot': bot_id, 'estimate': bot['estimate'], 'exact': bot['exact'], 'sigma': bot['sigma'],
                             'samples': bot['samples'], 'parts': bot['parts']})
    heap = None
    key = lambda name: (name, frozenset())
    if key('tgbotapi_heap_audit_bytes') in values:
        heap = {'estimate': values[key('tgbotapi_heap_estimated_bytes')], 'exact': values[key('tgbotapi_heap_audit_bytes')],
                'sigma': values[key('tgbotapi_heap_estimate_error_bytes')]}
    return observations, skipped, heap


def normal_coverage(k):
    return math.erf(k / math.sqrt(2))


def percentile(sorted_values, q):
    if not sorted_values:
        return float('nan')
    return sorted_values[min(len(sorted_values) - 1, int(q * len(sorted_values)))]


class Report:
    def __init__(self, title):
        self.title = title
        self.observations = []
        self.min_group_size = None  # the smallest number of independent observations in a group of correlated ones
        self.group_count = 0
        self.heap_z = []

    def add_group(self, observations, heap=None):
        self.observations.extend(observations)
        if observations:
            self.group_count += 1
            if self.min_group_size is None or len(observations) < self.min_group_size:
                self.min_group_size = len(observations)
        if heap is not None and heap['sigma'] > 0:
            self.heap_z.append((heap['estimate'] - heap['exact']) / heap['sigma'])

    def check(self, independent_groups):
        """Prints the report and returns True if all checks passed. If groups are independent, all observations are
        independent; otherwise, observations of different groups can be correlated."""
        print('\n== %s' % self.title)
        n = len(self.observations)
        if n == 0:
            print('no observations with enough samples')
            return False
        n_eff = n if independent_groups else self.min_group_size
        z = [(o['estimate'] - o['exact']) / o['sigma'] for o in self.observations]
        mean = sum(z) / n
        stddev = math.sqrt(sum((x - mean) ** 2 for x in z) / max(n - 1, 1))
        print('observations: %d (%s), independent: %d' % (
            n, '%d groups' % self.group_count, n_eff))
        ok = True

        def verdict(passed, text):
            nonlocal ok
            ok = ok and passed
            print('  [%s] %s' % ('PASS' if passed else 'FAIL', text))

        for k in [1, 2, 3]:
            covered = sum(1 for x in z if abs(x) <= k) / n
            p = normal_coverage(k)
            # 3 standard deviations of the binomial distribution and 1% for the error of the normal approximation
            tolerance = 3 * math.sqrt(p * (1 - p) / n_eff) + 0.01
            verdict(covered >= p - tolerance,
                    'exact value within %d reported sigma: %.1f%% (normal distribution: %.1f%%, minimum %.1f%%)' % (
                        k, covered * 100, p * 100, (p - tolerance) * 100))
        bound = 3 / math.sqrt(n_eff)
        verdict(abs(mean) <= bound, 'mean z %+.3f, must be within +-%.3f (no bias)' % (mean, bound))
        max_stddev = 1 + 3 / math.sqrt(2 * n_eff) + 0.05
        verdict(stddev <= max_stddev, 'standard deviation of z %.3f, must be at most %.3f (error isn\'t understated)' % (
            stddev, max_stddev))
        if stddev < 0.7:
            print('  [WARN] standard deviation of z is less than 0.7: the reported error is overstated')
        print('  max |z| %.2f' % max(abs(x) for x in z))
        total_exact = sum(o['exact'] for o in self.observations)
        total_difference = sum(o['estimate'] - o['exact'] for o in self.observations)
        print('  total error over all observations: %+.3f%% of %.1f MB' % (
            total_difference / total_exact * 100, total_exact / 1e6))

        print('  relative error by bot memory size:')
        print('    %-14s %6s %10s %10s %10s %16s' % ('memory', 'count', 'median', 'p95', 'max', 'median 3 sigma'))
        bounds = [0, 10e6, 100e6, 1e9, float('inf')]
        names = ['< 10 MB', '10 - 100 MB', '100 MB - 1 GB', '> 1 GB']
        for name, low, high in zip(names, bounds, bounds[1:]):
            group = [o for o in self.observations if low <= o['exact'] < high]
            if not group:
                continue
            errors = sorted(abs(o['estimate'] - o['exact']) / o['exact'] for o in group)
            stated = sorted(3 * o['sigma'] / o['exact'] for o in group)
            print('    %-14s %6d %9.1f%% %9.1f%% %9.1f%% %15.1f%%' % (
                name, len(group), percentile(errors, 0.5) * 100, percentile(errors, 0.95) * 100, errors[-1] * 100,
                percentile(stated, 0.5) * 100))
        if self.heap_z:
            print('  whole heap: %d scrapes, mean z %+.2f, max |z| %.2f' % (
                len(self.heap_z), sum(self.heap_z) / len(self.heap_z), max(abs(x) for x in self.heap_z)))
        return ok


def write_csv(path, rows):
    if not path:
        return
    with open(path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['group', 'bot', 'samples', 'estimate', 'exact', 'sigma', 'z'] +
                        ['estimate_' + p for p in PARTS] + ['exact_' + p for p in PARTS])
        for group, o in rows:
            writer.writerow([group, o['bot'], int(o['samples']), int(o['estimate']), int(o['exact']), round(o['sigma']),
                             '%.4f' % ((o['estimate'] - o['exact']) / o['sigma'])] +
                            [int(o['parts'].get(p, [0, 0])[0]) for p in PARTS] +
                            [int(o['parts'].get(p, [0, 0])[1]) for p in PARTS])
    print('observations are saved to %s' % path)


def parse_options(args, defaults):
    options = dict(defaults)
    positional = []
    for arg in args:
        if arg.startswith('--') and '=' in arg:
            name, value = arg[2:].split('=', 1)
            if name not in options:
                raise SystemExit('unknown option --%s' % name)
            options[name] = value
        else:
            positional.append(arg)
    return options, positional


def watch(url, options):
    duration = float(options['duration'])
    period = float(options['period'])
    min_samples = float(options['min-samples'])
    report = Report('server %s, %d s' % (url, duration))
    rows = []
    deadline = time.time() + duration
    scrape = 0
    while True:
        observations, skipped, heap = get_observations(fetch_metrics(url), min_samples)
        scrape += 1
        report.add_group(observations, heap)
        rows.extend((scrape, o) for o in observations)
        z = [(o['estimate'] - o['exact']) / o['sigma'] for o in observations]
        if z:
            print('scrape %d: %d bots (%d with less than %d samples), within 1/2/3 sigma %.0f%%/%.0f%%/%.0f%%, '
                  'mean z %+.2f, max |z| %.2f' % (
                      scrape, len(z), skipped, min_samples, *[sum(1 for x in z if abs(x) <= k) / len(z) * 100
                                                               for k in [1, 2, 3]],
                      sum(z) / len(z), max(abs(x) for x in z)), flush=True)
        else:
            print('scrape %d: no bots with at least %d samples' % (scrape, min_samples), flush=True)
        if time.time() + period > deadline:
            break
        time.sleep(period)
    write_csv(options['csv'], rows)
    return report.check(independent_groups=False)


async def fire_requests(port, requests, concurrency=100):
    """Sends requests without waiting for responses; requests, which need network access, stay in memory."""
    semaphore = asyncio.Semaphore(concurrency)

    async def send(token, method, params):
        async with semaphore:
            reader, writer = await asyncio.open_connection('127.0.0.1', port)
            body = json.dumps(params).encode()
            writer.write(b'POST /bot%s/%s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n'
                         b'Content-Length: %d\r\n\r\n' % (token.encode(), method.encode(), len(body)) + body)
            await writer.drain()
            writer.close()

    await asyncio.gather(*[send(*request) for request in requests])


def offline(build_dir, options):
    sys.dont_write_bytecode = True
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'restart'))
    import restart_test as t
    t.BUILD_DIR = os.path.abspath(build_dir)
    t.WORK_ROOT = tempfile.mkdtemp(prefix='telegram-bot-api-verify-memory-')
    runs = int(options['runs'])
    min_samples = float(options['min-samples'])
    bots = [(t.TOKEN, t.BOT_ID), (t.TOKEN2, t.BOT2_ID)]
    url = 'http://127.0.0.1:%d/metrics' % t.STAT_PORT
    ok = True
    rows = []
    rng = random.Random(int(options['seed']))
    for interval in [int(value) for value in options['intervals'].split(',')]:
        report = Report('offline servers, sampling interval %d KB, %d runs' % (interval, runs))
        for run in range(runs):
            work = t.prepare('verify', 0, second_bot=True)
            os.environ['TELEGRAM_BOT_API_MEMORY_AUDIT'] = '1'
            try:
                proc = t.start_server(work, '--memory-sampling-interval=%d' % interval)
            finally:
                del os.environ['TELEGRAM_BOT_API_MEMORY_AUDIT']
            try:
                for token, bot_id in bots:
                    assert t.api('getMe', token=token)[0]['ok']
                if run % 2 == 1:
                    # memory of the previous TDLib instance becomes unattributed, the new instance is a new owner
                    t.stats('?restart=%d' % t.BOT_ID)
                    t.wait_for(lambda: t.restart_count() >= 1, 60, 'restart')
                # messages, which can't be sent without network access, stay in memory of the bot
                requests = []
                for token, bot_id in bots:
                    # from 10 to 20000 messages, so memory of the bots changes from megabytes to tens of megabytes
                    for i in range(int(10 ** rng.uniform(1, 4.3))):
                        requests.append((token, 'sendMessage', {'chat_id': bot_id, 'text': 'x' * rng.randrange(1, 4000)}))
                rng.shuffle(requests)
                asyncio.run(fire_requests(t.API_PORT, requests))

                # wait until the server processes all requests
                def exact_memory():
                    values = fetch_metrics(url)
                    return [t.metric_sum(values, 'tgbotapi_bot_memory_exact_bytes', bot=bot_id) for _, bot_id in bots]

                last = exact_memory()
                for _ in range(100):
                    time.sleep(0.5)
                    current = exact_memory()
                    if current == last:
                        break
                    last = current
                observations, skipped, heap = get_observations(fetch_metrics(url), min_samples)
            finally:
                t.stop_server(proc)
            report.add_group(observations, heap)
            rows.extend(('%dKB-%d' % (interval, run), o) for o in observations)
            print('run %d: %s' % (run + 1, ', '.join('bot %s %.1f MB z %+.2f' % (
                o['bot'], o['exact'] / 1e6, (o['estimate'] - o['exact']) / o['sigma']) for o in observations)),
                  flush=True)
        ok = report.check(independent_groups=True) and ok
    write_csv(options['csv'], rows)
    return ok


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('watch', 'offline'):
        print(__doc__)
        sys.exit(2)
    if sys.argv[1] == 'watch':
        options, positional = parse_options(sys.argv[3:], {'duration': '600', 'period': '15', 'min-samples': '30',
                                                           'csv': ''})
        ok = watch(sys.argv[2], options)
    else:
        options, positional = parse_options(sys.argv[3:], {'runs': '30', 'intervals': '64,16', 'min-samples': '30',
                                                           'csv': '', 'seed': '1'})
        ok = offline(sys.argv[2], options)
    print('\nALL CHECKS PASSED' if ok else '\nSOME CHECKS FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
