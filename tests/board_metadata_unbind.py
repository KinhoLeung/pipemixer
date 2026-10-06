"""Reproduce metadata bind/disconnect while its exporter is briefly paused."""
import json
import os
import selectors
import shutil
import signal
import subprocess
import sys
import time

import board_graph as graph

graph.ENV['XDG_CONFIG_HOME'] = '/tmp/board/metadata-unbind-config'
BUS = 'test_metadata_unbind'
TARGET = 'pipemixer.bus.' + BUS + '.input'


def command(*args, check=True):
    result = subprocess.run([graph.BINARY, *args], env=graph.ENV, text=True,
                            capture_output=True, timeout=12)
    if check and result.returncode:
        raise AssertionError((args, result.returncode, result.stderr))
    return result


def receive(process, predicate, timeout):
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    result = b''; deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline:
            for key, _ in selector.select(min(.1, max(0, deadline - time.monotonic()))):
                data = os.read(key.fd, 8192)
                if not data:
                    return result
                result += data
                if predicate(result):
                    return result
        return result
    finally:
        selector.close()


def watch():
    return subprocess.Popen(['pw-metadata', '-m', '-n', 'default'], env=graph.ENV,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)


def main(expect_bug=False):
    before = graph.query()
    assert not any(n['group'] == BUS for n in before['nodes'])
    original = command('get-default', 'sink').stdout.strip()
    exporters = subprocess.check_output(['pidof', 'wireplumber'], text=True).split()
    assert len(exporters) == 1, exporters
    exporter = int(exporters[0]); paused = False; watcher = client = None
    def interrupted(signum, frame):
        raise InterruptedError('Metadata fixture interrupted')
    for signum in [signal.SIGTERM, signal.SIGINT, signal.SIGHUP]:
        signal.signal(signum, interrupted)
    command('create-bus', BUS)
    try:
        watcher = watch()
        initial = receive(watcher, lambda data: b"key:'default.audio.sink'" in data, 3)
        assert original.encode() in initial, initial
        os.kill(exporter, signal.SIGSTOP); paused = True
        for _ in range(8):
            client = watch()
            found = receive(client, lambda data: b'Found "default" metadata' in data, 1)
            assert b'Found "default" metadata' in found, found
            time.sleep(.03)  # Allow bind/flush while the exporter cannot pong.
            client.kill(); client.communicate(timeout=3); client = None
        os.kill(exporter, signal.SIGCONT); paused = False
        time.sleep(.2)
        result = command('set-default', TARGET, check=False)
        actual = command('get-default', 'sink').stdout.strip()
        assert actual == TARGET, (result.stderr, actual)
        updates = receive(watcher, lambda data: TARGET.encode() in data, .5)
        if expect_bug:
            assert result.returncode and 'timed out' in result.stderr, result
            assert TARGET.encode() not in updates, updates
            print('PASS original module reproduces lost live notifications after eight pending bind/disconnects; fresh metadata shows actual switch', flush=True)
        else:
            assert result.returncode == 0, result.stderr
            assert TARGET.encode() in updates, updates
            print('PASS upstream fix retains live metadata notifications and confirms the default switch after eight pending bind/disconnects', flush=True)
    finally:
        if paused:
            os.kill(exporter, signal.SIGCONT)
        for process in [client, watcher]:
            if process:
                process.terminate(); process.communicate(timeout=3)
        command('set-default', original, check=False)
        assert command('get-default', 'sink').stdout.strip() == original
        command('delete-bus', BUS)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'], ignore_errors=True)
    assert subprocess.check_output(['pidof', 'wireplumber'], text=True).split() == exporters
    after = graph.query()
    assert before == after
    print('METADATA UNBIND %s PASSED; exporter resumed and original default restored' %
          ('REPRODUCTION' if expect_bug else 'REGRESSION'), flush=True)


if __name__ == '__main__':
    main('--expect-bug' in sys.argv)
