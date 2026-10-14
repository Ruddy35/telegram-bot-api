#!/usr/bin/env python3
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
"""Self-tests of the verification tools and of the shared test helpers; they don't need a server.

Usage: python3 test/verify/test_tools.py
"""
import contextlib
import io
import itertools
import os
import random
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import latency_ab  # noqa: E402
import testlib  # noqa: E402
import verify_memory_accuracy  # noqa: E402


def quiet(function, *args, **kwargs):
    with contextlib.redirect_stdout(io.StringIO()):
        return function(*args, **kwargs)


class MetricsTest(unittest.TestCase):
    def test_parse(self):
        values = testlib.parse_metrics(
            '# HELP a help\n# TYPE a counter\na 1\nb{x="1",y="2"} 2.5\nc_bucket{le="+Inf"} 3\nd NaN\n'
            'e{v="a\\"b,c\\\\d\\n"} 4\n')
        self.assertEqual(values[('a', frozenset())], 1)
        self.assertEqual(values[('b', frozenset({('x', '1'), ('y', '2')}))], 2.5)
        self.assertEqual(values[('c_bucket', frozenset({('le', '+Inf')}))], 3)
        self.assertNotEqual(values[('d', frozenset())], values[('d', frozenset())])  # NaN
        self.assertEqual(values[('e', frozenset({('v', 'a"b,c\\d\n')}))], 4)
        self.assertRaises(ValueError, testlib.parse_metrics, 'a{x="1"}\n')

    def test_sum(self):
        values = testlib.parse_metrics('m{bot="1",part="a"} 1\nm{bot="1",part="b"} 2\nm{bot="2",part="a"} 4\nn 8\n')
        self.assertEqual(testlib.metric_sum(values, 'm'), 7)
        self.assertEqual(testlib.metric_sum(values, 'm', bot=1), 3)
        self.assertEqual(testlib.metric_sum(values, 'm', part='a'), 5)
        self.assertEqual(testlib.metric_sum(values, 'x'), 0)

    def test_percentile_and_median(self):
        self.assertEqual(testlib.percentile(list(range(100)), 0.99), 99)
        self.assertEqual(testlib.percentile(list(range(1000)), 0.5), 500)
        self.assertEqual(testlib.median([3, 1, 2]), 2)
        self.assertEqual(testlib.median([4, 1, 2, 3]), 2.5)


class PairedDifferenceTest(unittest.TestCase):
    def test_critical_value_by_enumeration(self):
        for n in range(1, 13):
            # the signed-rank statistic for every assignment of signs to the ranks 1..n
            statistics = [sum(rank for rank, negative in zip(range(1, n + 1), signs) if negative)
                          for signs in itertools.product([False, True], repeat=n)]
            expected = -1
            for t in range(n * (n + 1) // 2 + 1):
                if sum(1 for w in statistics if w <= t) <= 0.025 * len(statistics):
                    expected = t
            self.assertEqual(latency_ab.wilcoxon_critical_value(n), expected, n)

    def test_known_values(self):
        table = {6: 0, 8: 3, 10: 8, 15: 25, 20: 52, 25: 89}
        for n, value in table.items():
            self.assertEqual(latency_ab.wilcoxon_critical_value(n), value)
        self.assertEqual(latency_ab.paired_difference(list(range(1, 11))), (5.5, 3.0, 8.0))
        estimate, low, high = latency_ab.paired_difference([1, 2, 3])
        self.assertEqual((low, high), (float('-inf'), float('inf')))

    def test_coverage(self):
        rng = random.Random(1)
        runs = 2000
        covered = 0
        for _ in range(runs):
            _, low, high = latency_ab.paired_difference([rng.gauss(0, 1) for _ in range(10)])
            covered += low <= 0 <= high
        self.assertTrue(0.93 <= covered / runs <= 0.98, covered / runs)


def make_result(name, round_index, latency, cpu, errors=()):
    stream = {'count': 1000, 'p50': latency, 'p99': latency * 2, 'p99.9': latency * 3, 'max': latency * 4,
              'stall_times': [], 'stalled': 0.0, 'own_stalled': 0.0}
    return {'config': name, 'round': round_index, 'requests': 1000, 'errors': list(errors), 'cpu_per_request': cpu,
            'peak_rss': 1e7, 'sampler_operations': 0, 'slow_sampler_operations': [0, 0], 'sampler_max_lock_wait': 0,
            'scheduler_lags': [0, 0], 'streams': {name: dict(stream) for name in latency_ab.PROBE_STREAMS}}


class LatencyAbTest(unittest.TestCase):
    def results(self, b_latency_factor=1.0, b_cpu_factor=1.0, b_errors=()):
        rng = random.Random(2)
        results = []
        for r in range(12):
            latency = 0.001 * (1 + rng.random())
            cpu = 1e-4 * (1 + 0.01 * rng.random())
            results.append(make_result('a', r, latency, cpu))
            results.append(make_result('b', r, latency * b_latency_factor, cpu * b_cpu_factor, b_errors))
        return results

    def test_compare(self):
        self.assertTrue(quiet(latency_ab.compare, self.results()))
        self.assertFalse(quiet(latency_ab.compare, self.results(b_cpu_factor=1.5)))
        self.assertFalse(quiet(latency_ab.compare, self.results(b_latency_factor=2)))
        self.assertFalse(quiet(latency_ab.compare, self.results(b_errors=['getMe: HTTP 500'])))

    def test_histogram_excess(self):
        before = testlib.parse_metrics('h_bucket{s="1",le="0.01"} 5\nh_bucket{s="1",le="0.1"} 6\n'
                                       'h_bucket{s="1",le="+Inf"} 7\n')
        after = testlib.parse_metrics('h_bucket{s="1",le="0.01"} 15\nh_bucket{s="1",le="0.1"} 18\n'
                                      'h_bucket{s="1",le="+Inf"} 20\nh_bucket{s="2",le="0.01"} 1\n'
                                      'h_bucket{s="2",le="0.1"} 1\nh_bucket{s="2",le="+Inf"} 2\n')
        # new observations: s=1: 10 <= 0.01, 2 in (0.01, 0.1], 1 > 0.1; s=2: 1 <= 0.01, 1 > 0.1
        self.assertEqual(latency_ab.histogram_excess(before, after, 'h', [0.01, 0.1]), [4, 2])
        self.assertEqual(latency_ab.histogram_excess(before, after, 'missing', [0.01]), [0])

    def test_own_stalls(self):
        results = [make_result('a', 0, 0.001, 1e-4), make_result('b', 0, 0.001, 1e-4)]
        results[0]['streams']['client']['stall_times'] = [10.0, 20.0]
        results[1]['streams']['tdlib']['stall_times'] = [10.1]
        latency_ab.mark_own_stalls(results)
        self.assertEqual(results[0]['streams']['client']['own_stalled'], 1 / 1000)  # only the stall at 20 is own
        self.assertEqual(results[1]['streams']['tdlib']['own_stalled'], 0)


def memory_metrics(bots, interval=65536, audit=True):
    text = 'tgbotapi_memory_sampling_interval_bytes %d\n' % interval
    for bot_id, (estimate, exact, sigma) in bots.items():
        text += 'tgbotapi_bot_memory_bytes{bot="%s",part="tdlib"} %d\n' % (bot_id, estimate)
        text += 'tgbotapi_bot_memory_error_bytes{bot="%s"} %d\n' % (bot_id, sigma)
        text += 'tgbotapi_bot_memory_samples{bot="%s"} 10\n' % bot_id
        if audit:
            text += 'tgbotapi_bot_memory_exact_bytes{bot="%s",part="tdlib"} %d\n' % (bot_id, exact)
    return testlib.parse_metrics(text)


class MemoryAccuracyTest(unittest.TestCase):
    def test_observations(self):
        values = memory_metrics({1: (3e6, 2e6, 1e5), 2: (1e5, 1e5, 1e4)})
        observations, skipped, heap = verify_memory_accuracy.get_observations(values, 30)
        # bot 2 has 100 KB, i.e. less than 30 expected samples of 64 KB
        self.assertEqual([o['bot'] for o in observations], ['1'])
        self.assertEqual(skipped, 1)
        self.assertIsNone(heap)
        self.assertRaises(SystemExit, verify_memory_accuracy.get_observations, memory_metrics({1: (1, 1, 1)}, audit=False),
                          30)

    def check(self, mean, scale, heap_z=0.0):
        rng = random.Random(3)
        report = verify_memory_accuracy.Report('test')
        for _ in range(300):
            report.add_group([{'bot': '1', 'samples': 100, 'estimate': 1e7 + 1e5 * (mean + scale * rng.gauss(0, 1)),
                               'exact': 1e7, 'sigma': 1e5}],
                             {'estimate': 1e8 + heap_z * 1e6, 'exact': 1e8, 'sigma': 1e6})
        return quiet(report.check, True)

    def test_check(self):
        self.assertTrue(self.check(0, 1))
        self.assertFalse(self.check(0, 1.5))  # understated error
        self.assertFalse(self.check(0.5, 1))  # biased estimates
        self.assertFalse(self.check(0, 1, heap_z=5))
        self.assertFalse(quiet(verify_memory_accuracy.Report('empty').check, True))


if __name__ == '__main__':
    unittest.main()
