#!/usr/bin/env python3
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
"""Tests of the Docker image: docker/entrypoint.sh builds options from environment variables, and the server works
with them.

 - entrypoint: the options built from the environment by the entrypoint with a fake server binary;
 - server: the entrypoint starts the real server with options from the environment and an invalid option stops it;
 - image (only with --image): the same in a container of the image, plus an API request of an offline test bot.

Usage:
  cmake --build <build dir> --target telegram-bot-api restart-test-fake-auth
  python3 test/test_docker.py <build dir> [--image <image name>]
"""
import argparse
import os
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testlib  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENTRYPOINT = os.path.join(ROOT, 'docker', 'entrypoint.sh')


def log(*args):
    print('[%.3f]' % (time.time() % 1000), *args, flush=True)


def run_entrypoint(binary_dir, env, args=(), **kwargs):
    """Runs the entrypoint with only the given environment and with telegram-bot-api from binary_dir."""
    env = dict(env, PATH=binary_dir + os.pathsep + os.environ['PATH'])
    return subprocess.Popen([ENTRYPOINT] + list(args), env=env, **kwargs)


def test_entrypoint(work):
    """Every variable is converted to the option by the documented rules."""
    fake_dir = os.path.join(work, 'fake')
    os.makedirs(fake_dir)
    output = os.path.join(work, 'arguments')
    fake = os.path.join(fake_dir, 'telegram-bot-api')
    with open(fake, 'w') as f:
        f.write('#!/bin/sh\nprintf "%%s\\0" "$@" > %s\nprintf "%%s" "$TELEGRAM_API_HASH" > %s.hash\n' %
                (output, output))
    os.chmod(fake, os.stat(fake).st_mode | stat.S_IEXEC)

    env = {
        'TELEGRAM_HTTP_PORT': '8081',
        'TELEGRAM_TDLIB_RESTART_BOT_MEMORY_LIMIT_MIB': '1500',
        'TELEGRAM_LOCAL': 'true',
        'TELEGRAM_MALLOC_TRIM': 'false',
        'TELEGRAM_PROXY': '',
        'TELEGRAM_FILTER': 'a b=c',
        'TELEGRAM_API_ID': '12345',
        'TELEGRAM_API_HASH': 'secret-hash',
        'TELEGRAM_BOT_API_MEMORY_AUDIT': '1',
        'OTHER_VARIABLE': 'x',
    }
    process = run_entrypoint(fake_dir, env, ['--verbosity=3', 'an argument'])
    assert process.wait(30) == 0
    with open(output) as f:
        arguments = f.read().split('\0')[:-1]
    expected_options = {'--http-port=8081', '--tdlib-restart-bot-memory-limit-mib=1500', '--local', '--filter=a b=c'}
    assert set(arguments[:-2]) == expected_options and len(arguments) == len(expected_options) + 2, arguments
    assert arguments[-2:] == ['--verbosity=3', 'an argument'], arguments
    with open(output + '.hash') as f:
        assert f.read() == 'secret-hash', 'the API hash must be passed in the environment'
    log('entrypoint: options', sorted(arguments[:-2]), 'arguments', arguments[-2:])


def check_running_server(api_port, stat_port, token=None):
    """Checks that the server works with the options from the environment."""
    values = testlib.wait_for(lambda: try_metrics(stat_port), 30, 'the server')
    names = {name for name, _ in values}
    assert 'tgbotapi_heap_bytes' in names, 'memory accounting must work'
    assert not [name for name in names if name.startswith('tgbotapi_cpu_')], 'TELEGRAM_CPU_SAMPLING_RATE_HZ=0 is ignored'
    if token is not None:
        result, _ = testlib.http_request(api_port, token, 'getMe', {})
        assert result.get('ok'), result
    return values


def try_metrics(stat_port):
    server = testlib.Server(None, None, stat_port=stat_port)
    try:
        return server.metrics()
    except OSError:
        return None


def server_env(work, api_port, stat_port):
    return {
        'TELEGRAM_API_ID': '1',
        'TELEGRAM_API_HASH': 'test',
        'TELEGRAM_LOCAL': 'true',
        'TELEGRAM_DIR': work,
        'TELEGRAM_HTTP_PORT': str(api_port),
        'TELEGRAM_HTTP_STAT_PORT': str(stat_port),
        'TELEGRAM_CPU_SAMPLING_RATE_HZ': '0',
        'TELEGRAM_TDLIB_RESTART_MEMORY_LIMIT_MIB': '100000',
    }


def test_server(build_dir, work):
    """The entrypoint starts the real server, which exits with the code 0 on SIGTERM; an invalid option stops it."""
    binary_dir = os.path.join(work, 'bin')
    os.makedirs(binary_dir)
    os.symlink(os.path.join(build_dir, 'telegram-bot-api'), os.path.join(binary_dir, 'telegram-bot-api'))
    data = os.path.join(work, 'data')
    os.makedirs(data)
    api_port, stat_port = testlib.free_port(), testlib.free_port()
    process = run_entrypoint(binary_dir, server_env(data, api_port, stat_port), stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        check_running_server(api_port, stat_port)
        process.send_signal(signal.SIGTERM)
        assert process.wait(60) == 0, 'exit code %d' % process.returncode
    finally:
        if process.poll() is None:
            process.kill()
    env = dict(server_env(data, api_port, stat_port), TELEGRAM_TDLIB_RESTART_DRAIN_TIMEOUT_SECONDS='0')
    process = run_entrypoint(binary_dir, env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    _, error = process.communicate(timeout=30)
    assert process.returncode != 0 and b'drain timeout' in error, (process.returncode, error)
    log('server: started with options from the environment, stopped with the code 0, rejected an invalid option')


def docker(*args, check=True):
    return subprocess.run(['docker'] + list(args), check=check, capture_output=True, text=True).stdout.strip()


def test_image(build_dir, work, image):
    """A container of the image works with options from the environment, keeps data in the volume and stops cleanly."""
    data = os.path.join(work, 'volume')
    testlib.prepare_work_dir(build_dir, data, testlib.BOTS[:1])
    # the server in the image doesn't run as root
    subprocess.run(['chmod', '-R', 'a+rwX', data], check=True)
    api_port, stat_port = testlib.free_port(), testlib.free_port()
    env = dict(server_env('/var/lib/telegram-bot-api', 8081, 8082), TELEGRAM_VERBOSITY='2')
    arguments = ['run', '-d', '-v', '%s:/var/lib/telegram-bot-api' % data, '-p', '127.0.0.1:%d:8081' % api_port,
                 '-p', '127.0.0.1:%d:8082' % stat_port]
    for name, value in env.items():
        arguments += ['-e', '%s=%s' % (name, value)]
    container = docker(*arguments, image)
    try:
        check_running_server(api_port, stat_port, testlib.TOKEN)
        user = docker('exec', container, 'id', '-un')
        assert user == 'telegram-bot-api', user
        docker('stop', '-t', '60', container)
        exit_code = docker('inspect', '-f', '{{.State.ExitCode}}', container)
        assert exit_code == '0', exit_code
        assert os.path.exists(os.path.join(data, testlib.TOKEN, 'td.binlog'))
        # the log is written to the standard error stream of the container
        logs = subprocess.run(['docker', 'logs', container], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True).stdout
        assert 'server started' in logs, logs[-1000:]
    finally:
        docker('rm', '-f', container, check=False)
    log('image: the bot answered, the server ran as telegram-bot-api, logged to docker logs and stopped with the code 0')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('build', help='build directory with telegram-bot-api and restart-test-fake-auth')
    parser.add_argument('--image', help='name of the built image to test')
    options = parser.parse_args()
    build_dir = os.path.abspath(options.build)
    work = tempfile.mkdtemp(prefix='telegram-bot-api-docker-test-')
    try:
        test_entrypoint(os.path.join(work, 'entrypoint'))
        test_server(build_dir, os.path.join(work, 'server'))
        if options.image:
            test_image(build_dir, os.path.join(work, 'image'), options.image)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('ALL DOCKER TESTS PASSED')


if __name__ == '__main__':
    main()
