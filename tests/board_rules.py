"""Automatic routing, endpoint reappearance and ownership on real PipeWire."""
import json
import fcntl
import os
import shutil
import subprocess
import time

import board_graph as graph

CONFIG = '/tmp/board/rule-test-config'
DIRECTORY = CONFIG + '/pipemixer/rules'
graph.ENV['XDG_CONFIG_HOME'] = CONFIG
NAMES = [graph.PREFIX + 'rule-' + n for n in ['source', 'destination', 'other', 'guard']]


def command(*args, check=True, env=None):
    result = subprocess.run([graph.BINARY, *args], env=env or graph.ENV, text=True,
                            capture_output=True, timeout=12)
    if check and result.returncode:
        raise AssertionError('%s: %s' % (args, result.stderr))
    return result


def endpoint(node, direction, channel='FL'):
    port = next(p for p in graph.query('ports') if p['node_name'] == node
                and p['direction'] == direction and p['channel'] == channel)
    return node + ':' + port['name']


def status():
    return json.loads(command('--json', 'list-route-rules').stdout)


def state(name):
    return next(r['state'] for r in status()['rules'] if r['name'] == name)


def between(output, inp):
    out = next((p for p in graph.query('ports') if p['node_name'] + ':' + p['name'] == output), None)
    dest = next((p for p in graph.query('ports') if p['node_name'] + ':' + p['name'] == inp), None)
    if not out or not dest:
        return []
    return [l for l in graph.query('links') if l['output']['port_id'] == out['id']
            and l['input']['port_id'] == dest['id']]


def start(env=None):
    log = open('/tmp/board/rules-engine.log', 'a')
    process = subprocess.Popen([graph.BINARY, 'routing-daemon'], env=env or graph.ENV,
                               stdout=subprocess.DEVNULL, stderr=log)
    process.test_log = log
    return process


def stop(process):
    if process:
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=4)
        process.test_log.close()


def main():
    assert not any(n['name'] in NAMES for n in graph.query()['nodes'])
    defaults = {k: command('get-default', k).stdout for k in ['sink', 'source']}
    shutil.rmtree(CONFIG, ignore_errors=True)
    daemon = foreign = None
    pairs = [(NAMES[0] + ':monitor_' + c, NAMES[1] + ':playback_' + c) for c in ['FL', 'FR']]
    offline = dict(graph.ENV, PIPEWIRE_REMOTE='pipemixer-no-server')
    try:
        for c, pair in zip(['left', 'right'], pairs):
            command('create-route-rule', c, *pair, env=offline)
        command('check-route-rules', env=offline)
        assert os.stat(DIRECTORY + '/left.json').st_mode & 0o777 == 0o600
        assert command('create-route-rule', 'invalid', 'id:42', pairs[0][1], check=False).returncode == 2
        assert command('create-route-rule', 'left', *pairs[0], check=False).returncode == 1
        daemon = start()
        graph.wait_for(lambda: status()['engine_running'])
        assert state('left') == 'waiting'
        duplicate = command('routing-daemon', check=False)
        assert duplicate.returncode == 1, duplicate
        graph.create_sink(NAMES[1]); graph.create_sink(NAMES[0])
        graph.wait_for(lambda: all(state(n) == 'connected' for n in ['left', 'right']))
        assert all(len(between(*p)) == 1 for p in pairs)
        command('set-volume', NAMES[0], '77')
        print('PASS offline persistent rules, single engine and automatic stereo links', flush=True)

        previous = next(n['id'] for n in graph.query()['nodes'] if n['name'] == NAMES[1])
        graph.remove_sink(NAMES[1])
        graph.wait_for(lambda: state('left') == 'waiting')
        graph.create_sink(NAMES[2]); graph.create_sink(NAMES[3])
        time.sleep(1.2)
        assert not any(l['output']['node_name'] == NAMES[0] for l in graph.query('links'))
        graph.create_sink(NAMES[1])
        current = next(n['id'] for n in graph.query()['nodes'] if n['name'] == NAMES[1])
        assert previous != current
        graph.wait_for(lambda: all(state(n) == 'connected' for n in ['left', 'right']))
        volumes = json.loads(command('--json', 'get-volume', NAMES[0]).stdout)['channels']
        assert all(round(c['percent']) == 77 for c in volumes), volumes
        print('PASS changed endpoint IDs reconnect by name, no fallback, source gain retained', flush=True)

        reverse = (endpoint(NAMES[1], 'output'), endpoint(NAMES[0], 'input'))
        command('create-route-rule', 'reverse', *reverse)
        graph.wait_for(lambda: state('reverse') == 'blocked')
        assert not between(*reverse)
        command('delete-route-rule', 'reverse')

        manual = (endpoint(NAMES[0], 'output', 'FR'), endpoint(NAMES[2], 'input', 'FR'))
        command('connect', *manual)
        manual_id = between(*manual)[0]['id']
        command('create-route-rule', 'manual', *manual)
        graph.wait_for(lambda: state('manual') == 'connected')
        command('enable-route-rule', 'manual', 'off')
        graph.wait_for(lambda: state('manual') == 'disabled')
        command('delete-route-rule', 'manual'); time.sleep(1.2)
        assert [l['id'] for l in between(*manual)] == [manual_id]
        print('PASS feedback rejection and pre-existing manual connections retained', flush=True)

        foreign_env = dict(graph.ENV, XDG_CONFIG_HOME=CONFIG + '-foreign')
        shutil.rmtree(CONFIG + '-foreign', ignore_errors=True)
        foreign_pair = (endpoint(NAMES[2], 'output'), endpoint(NAMES[1], 'input'))
        command('create-route-rule', 'foreign', *foreign_pair, env=foreign_env)
        foreign = start(foreign_env)
        graph.wait_for(lambda: between(*foreign_pair))
        foreign_id = between(*foreign_pair)[0]['id']
        command('enable-route-rule', 'left', 'off')
        graph.wait_for(lambda: not between(*pairs[0]))
        assert [l['id'] for l in between(*foreign_pair)] == [foreign_id]
        command('enable-route-rule', 'left', 'on')
        graph.wait_for(lambda: state('left') == 'connected')
        print('PASS disable removes only owned links, independent rule sets coexist', flush=True)

        command('save-scene', 'test_rule_lock')
        with open(graph.ENV['XDG_RUNTIME_DIR'] + '/pipemixer-scene.lock', 'r') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            command('disconnect', *pairs[0])
            time.sleep(1.3)
            assert not between(*pairs[0]), 'Engine must wait while a scene owns the lock'
        graph.wait_for(lambda: between(*pairs[0]))
        command('load-scene', 'test_rule_lock')
        print('PASS route engine waits for scene transactions and resumes afterwards', flush=True)

        path = DIRECTORY + '/left.json'
        original = open(path).read()
        with open(path, 'w') as file:
            file.write('{"format":"pipemixer.route-rule","enabled":true,"enabled":false}')
        assert command('check-route-rules', env=offline, check=False).returncode == 1
        time.sleep(1.3)
        command('disconnect', *pairs[0])
        graph.wait_for(lambda: between(*pairs[0]))
        with open(path, 'w') as file:
            file.write(original)
        graph.wait_for(lambda: state('left') == 'connected')
        assert daemon.poll() is None
        print('PASS malformed reload keeps previous valid rules and reconnects lost links', flush=True)

        stop(daemon); daemon = None
        assert all(between(*p) for p in pairs)
        command('enable-route-rule', 'left', 'off', env=offline)
        daemon = start()
        graph.wait_for(lambda: not between(*pairs[0]))
        assert between(*pairs[1])
        assert [l['id'] for l in between(*foreign_pair)] == [foreign_id]
        command('delete-route-rule', 'right')
        graph.wait_for(lambda: not between(*pairs[1]))
        print('PASS daemon restart reads persistence and cleans only disabled/deleted owned links', flush=True)
    finally:
        stop(daemon); stop(foreign)
        for name in reversed(NAMES):
            graph.remove_sink(name)
        shutil.rmtree(CONFIG, ignore_errors=True); shutil.rmtree(CONFIG + '-foreign', ignore_errors=True)
        for k, value in defaults.items():
            assert command('get-default', k).stdout == value
    graph.reference_check()
    print('AUTOMATIC ROUTING CORE PASSED', flush=True)


if __name__ == '__main__':
    main()
