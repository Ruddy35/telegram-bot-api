#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
"""Helpers shared by the tests and the verification tools: offline test bots, server processes, HTTP requests and
metrics in the Prometheus text format."""
import json
import os
import re
import signal
import socket
import subprocess
import time
import urllib.error
import urllib.request

# Bots, which are known to TDLib without network access, so getMe works offline. Their databases are marked as
# authorized by restart-test-fake-auth.
BOT_ID = 1087968824  # @GroupAnonymousBot
TOKEN = '%d:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw' % BOT_ID
BOT2_ID = 136817688  # @Channel_Bot
TOKEN2 = '%d:AAGq7rGkXmpVZ3bNzE9tP1yLcW4sDfHjKlQ' % BOT2_ID
BOTS = [(TOKEN, BOT_ID), (TOKEN2, BOT2_ID)]

_METRIC_LINE = re.compile(r'^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{(.*)\})?\s+(\S+)$')
_LABEL = re.compile(r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:[^"\\]|\\.)*)"')


def parse_metrics(text):
    """Parses the Prometheus text format into {(name, frozenset of (label, value)): value}."""
    result = {}
    for line in text.splitlines():
        if not line or line.startswith('#'):
            continue
        match = _METRIC_LINE.match(line)
        if match is None:
            raise ValueError('invalid metric line: %r' % line)
        name, label_text, value = match.groups()
        labels = frozenset((label, re.sub(r'\\(.)', lambda m: '\n' if m.group(1) == 'n' else m.group(1), raw))
                           for label, raw in _LABEL.findall(label_text or ''))
        result[(name, labels)] = float(value)
    return result


def metric_sum(values, name, **labels):
    """Returns the sum of values of the metric over all series, which have the given labels."""
    return sum(value for (metric_name, metric_labels), value in values.items()
               if metric_name == name and all((k, str(v)) in metric_labels for k, v in labels.items()))


def percentile(sorted_values, q):
    if not sorted_values:
        return float('nan')
    return sorted_values[min(len(sorted_values) - 1, int(q * len(sorted_values)))]


def median(values):
    values = sorted(values)
    if not values:
        return float('nan')
    middle = len(values) // 2
    return values[middle] if len(values) % 2 else (values[middle - 1] + values[middle]) / 2


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def prepare_work_dir(build_dir, work, bots=BOTS, update_count=0):
    """Creates a working directory with authorized databases of the bots and optionally update_count pending updates
    of the first bot."""
    os.makedirs(work, exist_ok=True)
    for token, bot_id in bots:
        os.makedirs(os.path.join(work, token), exist_ok=True)
        subprocess.run([os.path.join(build_dir, 'restart-test-fake-auth'), os.path.join(work, token, 'td.binlog'),
                        str(bot_id)], check=True, capture_output=True)
    if update_count:
        subprocess.run([os.path.join(build_dir, 'restart-test-fake-updates'), os.path.join(work, 'tqueue.binlog'),
                        str(bots[0][1]), str(update_count)], check=True, capture_output=True)


def http_request(port, token, method, params, timeout=60):
    """Calls an API method; returns (decoded response, latency in seconds). Network errors are returned as responses
    with ok == False."""
    request = urllib.request.Request('http://127.0.0.1:%d/bot%s/%s' % (port, token, method),
                                     data=json.dumps(params).encode(), headers={'Content-Type': 'application/json'})
    start = time.time()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as r:
            result = json.loads(r.read())
    except urllib.error.HTTPError as e:
        result = json.loads(e.read())
    except OSError as e:
        result = {'ok': False, 'description': str(e)}
    return result, time.time() - start


class Server:
    """A server process with offline test bots; use as a context manager to stop the server in any case."""

    def __init__(self, binary, work, args=(), env=None, api_port=None, stat_port=None):
        self.binary = binary
        self.work = work
        self.args = list(args)
        self.env = env
        self.api_port = api_port or free_port()
        self.stat_port = stat_port or free_port()
        self.proc = None

    def start(self):
        env = dict(os.environ)
        env.update(self.env or {})
        self.proc = subprocess.Popen(
            [self.binary, '--api-id=1', '--api-hash=test', '--local', '-p', str(self.api_port), '-s', str(self.stat_port),
             '-d', self.work, '-l', os.path.join(self.work, 'log.txt'), '-v', '2'] + self.args,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        deadline = time.time() + 30
        while True:
            if self.proc.poll() is not None:
                raise RuntimeError('server exited with code %d; see %s' % (self.proc.returncode,
                                                                           os.path.join(self.work, 'log.txt')))
            try:
                self.stats()
                return self
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)

    def stop(self):
        """Stops the server gracefully, kills it if it doesn't exit in 60 seconds; returns the exit code."""
        if self.proc is None or self.proc.poll() is not None:
            return None if self.proc is None else self.proc.returncode
        self.proc.send_signal(signal.SIGTERM)
        try:
            return self.proc.wait(60)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            raise

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        if exc[0] is None:
            self.stop()
        elif self.proc is not None and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()

    def api(self, method, token=TOKEN, timeout=60, **params):
        return http_request(self.api_port, token, method, params, timeout)

    def metrics(self):
        with urllib.request.urlopen('http://127.0.0.1:%d/metrics' % self.stat_port, timeout=30) as r:
            return parse_metrics(r.read().decode())

    def stats(self, query=''):
        """Returns the first value of each key of the text statistics page."""
        with urllib.request.urlopen('http://127.0.0.1:%d/%s' % (self.stat_port, query), timeout=10) as r:
            text = r.read().decode()
        result = {}
        for line in text.splitlines():
            parts = line.split('\t')
            if len(parts) >= 2 and parts[0] not in result:
                result[parts[0]] = parts[1]
        return result

    def cpu_seconds(self):
        """CPU time of the process from /proc."""
        with open('/proc/%d/stat' % self.proc.pid) as f:
            fields = f.read().rsplit(')', 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')

    def peak_rss(self):
        with open('/proc/%d/status' % self.proc.pid) as f:
            for line in f:
                if line.startswith('VmHWM:'):
                    return int(line.split()[1]) * 1024
        return 0


def wait_for(predicate, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError('timeout waiting for ' + what)
