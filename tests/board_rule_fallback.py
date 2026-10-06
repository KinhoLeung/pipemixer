"""Ordered stereo failover, stable failback, PCM and negotiation errors."""
import json
import os
import shutil
import subprocess
import time

import board_graph as graph
import board_rules as rules
from board_audio import capture, PROBE
from board_routing import Tui

CONFIG = '/tmp/board/rule-fallback-config'
graph.ENV['XDG_CONFIG_HOME'] = CONFIG
SOURCE_GROUP = 'test_fallback_source'
SOURCE = 'pipemixer.bus.' + SOURCE_GROUP + '.output'
GROUPS = ['test_fallback_primary', 'test_fallback_one', 'test_fallback_two']
INPUTS = ['pipemixer.bus.' + n + '.input' for n in GROUPS]
OUTPUTS = ['pipemixer.bus.' + n + '.output' for n in GROUPS]


def record():
    return next(r for r in rules.status()['rules'] if r['name'] == 'listen')


def targets():
    return {l['input']['node_name'] for l in graph.query('links') if l['output']['node_name'] == SOURCE}


def ready(index):
    r = record()
    return targets() == {INPUTS[index]} and r['state'] == 'connected' and r['connected_pairs'] == 2 and r['active_input'] == INPUTS[index] + ':*'


def level(index, reference):
    measured = capture(OUTPUTS[index])[0]
    assert abs(measured - reference) < reference * .06, (measured, reference)
    return measured


def main():
    shutil.rmtree(CONFIG, ignore_errors=True)
    assert not any(n['group'] in GROUPS + [SOURCE_GROUP] for n in graph.query()['nodes'])
    daemon = tone = ui = None
    rejectors = []
    try:
        rules.command('create-bus', SOURCE_GROUP); rules.command('create-bus', GROUPS[2])
        rules.command('set-volume', SOURCE, '77'); rules.command('set-mute', SOURCE, 'off')
        rules.command('create-route-rule', 'listen', SOURCE + ':*', INPUTS[0] + ':*', '--match', 'glob',
                      '--priority', '80', '--exclusive-group', 'monitor', '--fallback', INPUTS[1] + ':*',
                      '--fallback', INPUTS[2] + ':*', '--switch-delay', '1500')
        rules.command('check-route-rules', env=dict(graph.ENV, PIPEWIRE_REMOTE='absent'))
        original = open(CONFIG + '/pipemixer/rules/listen.json').read()
        assert rules.command('set-route-fallbacks', 'listen', INPUTS[0] + ':*', check=False).returncode == 1
        assert open(CONFIG + '/pipemixer/rules/listen.json').read() == original
        daemon = rules.start(); graph.wait_for(lambda: ready(2))
        assert record()['using_fallback']
        tone = subprocess.Popen([PROBE, 'play', graph.PREFIX + 'fallback-tone',
                                 'pipemixer.bus.' + SOURCE_GROUP + '.input', '1000'], env=graph.ENV,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        graph.wait_for(lambda: any(n['name'] == graph.PREFIX + 'fallback-tone' for n in graph.query()['nodes']))
        reference = capture(OUTPUTS[2])[0]; assert .035 < reference < .045, reference
        rules.command('create-bus', GROUPS[1])
        time.sleep(.5); assert targets() == {INPUTS[2]}, 'Preferred target must settle before failback'
        graph.wait_for(lambda: ready(1)); first = level(1, reference)
        for _ in range(3):
            rules.command('create-bus', GROUPS[0]); time.sleep(.3)
            assert targets() == {INPUTS[1]}
            rules.command('delete-bus', GROUPS[0]); time.sleep(.3)
        rules.command('create-bus', GROUPS[0]); time.sleep(.5)
        assert targets() == {INPUTS[1]}
        graph.wait_for(lambda: ready(0)); primary = level(0, reference)
        assert not record()['using_fallback']
        rules.command('delete-bus', GROUPS[0]); graph.wait_for(lambda: ready(1))
        rules.command('delete-bus', GROUPS[1]); graph.wait_for(lambda: ready(2))
        second = level(2, reference)
        print('PASS ordered stereo failover and stable failback survive flapping; PCM %.6f / %.6f / %.6f / %.6f' %
              (reference, first, primary, second), flush=True)

        volumes = json.loads(rules.command('--json', 'get-volume', SOURCE).stdout)['channels']
        assert all(round(c['percent']) == 77 for c in volumes)
        rules.command('set-mute', SOURCE, 'on'); assert capture(OUTPUTS[2])[0] < .00001
        assert rules.command('get-mute', SOURCE).stdout.strip() == 'on'
        rules.command('set-mute', SOURCE, 'off')
        ui = Tui(); screen = ui.send(b'a'); assert b'fallback' in screen, screen
        ui.close(); ui = None
        print('PASS source gain/mute preserved, fallback status visible and TUI exit keeps engine running', flush=True)

        rules.command('save-scene', 'fallback_owner')
        scene_path = CONFIG + '/pipemixer/scenes/fallback_owner.json'
        with open(scene_path) as file: scene = json.load(file)
        owned = [link for link in scene['links'] if link.get('route_rule') == 'listen']
        assert len(owned) == 2 and all(len(link['rule_set']) == 16 for link in owned)
        rules.stop(daemon); daemon = None
        for channel in ['FL', 'FR']:
            rules.command('disconnect', SOURCE + ':capture_' + channel, INPUTS[2] + ':playback_' + channel)
        assert not targets()
        rules.command('load-scene', 'fallback_owner'); assert ready(2)
        daemon = rules.start()
        rules.command('enable-route-rule', 'listen', 'off'); graph.wait_for(lambda: not targets())
        assert any(l['output']['node_name'] == graph.PREFIX + 'fallback-tone' for l in graph.query('links'))
        rules.command('enable-route-rule', 'listen', 'on'); graph.wait_for(lambda: ready(2))
        level(2, reference)
        owned[0]['rule_set'] = 'invalid'
        with open(scene_path, 'w') as file: json.dump(scene, file)
        assert rules.command('check-scene', 'fallback_owner', check=False).returncode == 1
        rules.command('delete-scene', 'fallback_owner')
        print('PASS scene restores rule ownership; disabling clears restored links and keeps manual input', flush=True)

        log_start = os.path.getsize('/tmp/board/rules-engine.log')
        for name in INPUTS[:2]:
            rejectors.append(subprocess.Popen([PROBE, 'reject', name], env=graph.ENV,
                                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        graph.wait_for(lambda: all(any(p['node_name'] == n for p in graph.query('ports')) for n in INPUTS[:2]))
        errors = set(); deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            with open('/tmp/board/rules-engine.log') as file:
                file.seek(log_start); log = file.read()
            for name in INPUTS[:2]:
                if "target '%s:*' failed" % name in log: errors.add(name)
            for link in graph.query('links'):
                if link['output']['node_name'] == SOURCE and link['state'] == 'error': errors.add(link['input']['node_name'])
            if errors == set(INPUTS[:2]) and ready(2): break
            assert daemon.poll() is None, open('/tmp/board/rules-engine.log').read()
            time.sleep(.03)
        assert errors == set(INPUTS[:2]) and ready(2), (errors, targets())
        print('PASS two incompatible candidates fail real negotiation; next healthy target is selected', flush=True)

        rules.command('enable-route-rule', 'listen', 'off'); graph.wait_for(lambda: not targets())
        rules.command('set-route-fallbacks', 'listen', 'off', '--switch-delay', '0')
        assert record()['fallbacks'] == [] and record()['switch_delay_ms'] == 0
        path = CONFIG + '/pipemixer/rules/listen.json'; good = open(path).read()
        for key, value in [('priority', 4294967296), ('priority', 1.5), ('switch_delay_ms', 60001), ('fallbacks', [INPUTS[0] + ':*'])]:
            data = json.loads(good); data[key] = value
            with open(path, 'w') as file: json.dump(data, file)
            assert rules.command('check-route-rules', check=False).returncode == 1, (key, value)
        with open(path, 'w') as file: file.write(good)
        print('PASS offline fallback edits, duplicate target rejection and strict numeric/JSON validation', flush=True)
    finally:
        if ui: ui.close()
        if tone: tone.terminate(); tone.wait(timeout=3)
        for process in rejectors:
            if process.poll() is None: process.terminate()
            process.wait(timeout=3)
        rules.stop(daemon)
        for name in reversed(GROUPS + [SOURCE_GROUP]): rules.command('delete-bus', name)
        shutil.rmtree(CONFIG, ignore_errors=True)
    graph.reference_check()
    print('FALLBACK ROUTING PASSED', flush=True)


if __name__ == '__main__': main()
