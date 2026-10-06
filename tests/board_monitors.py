"""Independent monitor mix/Solo, PCM isolation and persistent policy ownership."""
import json
import os
import shutil
import subprocess
import time

import board_graph as graph
import board_rules as rules
from board_audio import capture, PROBE

CONFIG = '/tmp/board/monitor-test-config'
graph.ENV['XDG_CONFIG_HOME'] = CONFIG
GROUPS = ['test_monitor_a', 'test_monitor_b', 'test_monitor_main', 'test_monitor_phones']
OUTPUTS = ['pipemixer.bus.' + n + '.output' for n in GROUPS]
INPUTS = ['pipemixer.bus.' + n + '.input' for n in GROUPS]
NAME = 'test_phones'
MONITOR = 'pipemixer.monitor.' + NAME


def record(name=NAME):
    return next((m for m in json.loads(rules.command('--json', 'list-monitors').stdout)['monitors'] if m['name'] == name), None)


def ready(sources, mode='mix'):
    data = record()
    return data and data['state'] == 'connected' and data['mode'] == mode and data['active_sources'] == sources


def level(reference, monitor_reference):
    main, phones = capture(OUTPUTS[2], OUTPUTS[3])
    assert abs(main - reference) < reference * .025, (main, reference)
    assert abs(phones - monitor_reference) < max(.00002, monitor_reference * .04), (phones, monitor_reference)
    return main, phones


def main():
    shutil.rmtree(CONFIG, ignore_errors=True)
    assert not any(n['group'] in GROUPS + [NAME] for n in graph.query()['nodes'])
    daemon = foreign = None; tones = []
    defaults = {kind: rules.command('get-default', kind).stdout for kind in ['sink', 'source']}
    offline = dict(graph.ENV, PIPEWIRE_REMOTE='pipemixer-no-server')
    try:
        rules.command('create-monitor', NAME, INPUTS[3], *OUTPUTS[:2], env=offline)
        rules.command('check-monitors', env=offline)
        path = CONFIG + '/pipemixer/monitors/' + NAME + '.json'
        assert os.stat(path).st_mode & 0o777 == 0o600
        original = open(path).read()
        for args in [('create-monitor', '../invalid', INPUTS[3]),
                     ('create-monitor', 'invalid', 'id:9'),
                     ('set-monitor-sources', NAME, OUTPUTS[0], OUTPUTS[0]),
                     ('solo-monitor', NAME, INPUTS[3], 'on')]:
            assert rules.command(*args, env=offline, check=False).returncode != 0, args
        assert open(path).read() == original
        daemon = rules.start(); graph.wait_for(lambda: record()['state'] == 'waiting')
        for name in GROUPS: rules.command('create-bus', name)
        graph.wait_for(lambda: any(p['node_name'] == MONITOR + '.output' for p in graph.query('ports')))
        for node in OUTPUTS + [MONITOR + '.output']:
            rules.command('set-volume', node, '100'); rules.command('set-mute', node, 'off')
        rules.command('set-volume', OUTPUTS[1], '50')
        for source in OUTPUTS[:2]:
            for channel in ['FL', 'FR']: rules.command('connect', source + ':capture_' + channel, INPUTS[2] + ':playback_' + channel)
        manual_ids = {l['id'] for l in graph.query('links') if l['input']['node_name'] == INPUTS[2]}
        for i, frequency in enumerate([440, 880]):
            tones.append(subprocess.Popen([PROBE, 'play', graph.PREFIX + 'monitor-tone' + str(i), INPUTS[i], str(frequency)],
                                          env=graph.ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        graph.wait_for(lambda: ready(OUTPUTS[:2]))
        time.sleep(.4)
        single_a, single_b, mixed, phones = capture(OUTPUTS[0], OUTPUTS[1], OUTPUTS[2], OUTPUTS[3])
        assert .085 < single_a < .092 and .010 < single_b < .012, (single_a, single_b)
        assert abs(phones - mixed) < mixed * .03
        rules.command('solo-monitor', NAME, OUTPUTS[0], 'on'); graph.wait_for(lambda: ready([OUTPUTS[0]], 'solo'))
        solo_a = level(mixed, single_a)
        rules.command('solo-monitor', NAME, OUTPUTS[1], 'on'); graph.wait_for(lambda: ready(OUTPUTS[:2], 'solo'))
        level(mixed, mixed)
        rules.command('solo-monitor', NAME, OUTPUTS[0], 'off'); graph.wait_for(lambda: ready([OUTPUTS[1]], 'solo'))
        solo_b = level(mixed, single_b)
        rules.command('clear-monitor-solo', NAME); graph.wait_for(lambda: ready(OUTPUTS[:2])); level(mixed, mixed)
        assert {l['id'] for l in graph.query('links') if l['input']['node_name'] == INPUTS[2]} == manual_ids
        assert all(round(c['percent']) == 50 for c in json.loads(rules.command('--json', 'get-volume', OUTPUTS[1]).stdout)['channels'])
        print('PASS independent stereo mix and multi-Solo: main %.6f unchanged, solo A %.6f / B %.6f; source gain and main link IDs retained' %
              (mixed, solo_a[1], solo_b[1]), flush=True)

        rules.command('set-monitor-source', NAME, OUTPUTS[1]); graph.wait_for(lambda: ready([OUTPUTS[1]], 'listen')); level(mixed, single_b)
        rules.command('solo-monitor', NAME, OUTPUTS[0], 'on'); graph.wait_for(lambda: ready([OUTPUTS[0]], 'solo'))
        rules.command('set-monitor-source', NAME, OUTPUTS[2]); time.sleep(1.2)
        assert ready([OUTPUTS[0]], 'solo'); level(mixed, single_a)
        rules.command('clear-monitor-solo', NAME); graph.wait_for(lambda: ready([OUTPUTS[2]], 'listen')); level(mixed, mixed)
        late = graph.PREFIX + 'monitor-late-source'
        rules.command('set-monitor-source', NAME, late); graph.wait_for(lambda: record()['state'] == 'waiting'); level(mixed, 0)
        graph.create_sink(late); graph.wait_for(lambda: ready([late], 'listen')); level(mixed, 0)
        graph.remove_sink(late)
        rules.command('set-monitor-source', NAME, 'mix'); graph.wait_for(lambda: ready(OUTPUTS[:2])); level(mixed, mixed)
        assert record()['sources'] == OUTPUTS[:2]
        print('PASS listening source switches independently; clearing Solo restores the previous choice; missing selected source stays silent and reconnects without fallback', flush=True)

        rules.command('set-volume', MONITOR + '.output', '50'); level(mixed, mixed / 8)
        rules.command('set-mute', MONITOR + '.output', 'on'); level(mixed, 0)
        rules.command('solo-monitor', NAME, OUTPUTS[0], 'on'); graph.wait_for(lambda: ready([OUTPUTS[0]], 'solo'))
        assert rules.command('get-mute', MONITOR + '.output').stdout.strip() == 'on'
        rules.command('set-mute', MONITOR + '.output', 'off'); level(mixed, single_a / 8)
        rules.command('set-volume', MONITOR + '.output', '100')
        rules.stop(daemon); daemon = rules.start(); graph.wait_for(lambda: ready([OUTPUTS[0]], 'solo')); level(mixed, single_a)
        rules.command('delete-bus', GROUPS[3]); graph.wait_for(lambda: record()['state'] == 'waiting')
        rules.command('create-bus', GROUPS[3]); graph.wait_for(lambda: ready([OUTPUTS[0]], 'solo'))
        rules.command('set-volume', OUTPUTS[3], '100'); rules.command('set-mute', OUTPUTS[3], 'off'); level(mixed, single_a)
        rules.command('clear-monitor-solo', NAME); graph.wait_for(lambda: ready(OUTPUTS[:2]))
        print('PASS monitor gain/mute isolated; Solo survives engine restart and headphone endpoint reappearance', flush=True)

        with open(path, 'w') as file: file.write('{ broken')
        assert rules.command('check-monitors', env=offline, check=False).returncode == 1
        time.sleep(1.3); level(mixed, mixed)
        assert daemon.poll() is None
        with open(path, 'w') as file: file.write(original)
        rules.command('save-scene', 'monitor_owner'); rules.command('check-scene', 'monitor_owner')
        with open(CONFIG + '/pipemixer/scenes/monitor_owner.json') as file: scene = json.load(file)
        owned = [p for p in scene['paths'] if p['kind'] == 'monitor']
        assert len(owned) == 1 and len(owned[0]['monitor_set']) == 16
        rules.command('load-scene', 'monitor_owner'); graph.wait_for(lambda: ready(OUTPUTS[:2])); level(mixed, mixed)
        rules.command('delete-scene', 'monitor_owner')
        print('PASS malformed reload keeps working mix; scene saves/loads monitor worker and managed link ownership', flush=True)

        foreign_env = dict(graph.ENV, XDG_CONFIG_HOME=CONFIG + '-foreign')
        rules.command('create-monitor', NAME, INPUTS[3], OUTPUTS[0], env=foreign_env)
        foreign = rules.start(foreign_env)
        graph.wait_for(lambda: json.loads(rules.command('--json', 'list-monitors', env=foreign_env).stdout)['monitors'][0]['state'] == 'conflict')
        rules.command('delete-monitor', NAME, env=foreign_env); time.sleep(1.2); assert ready(OUTPUTS[:2]); level(mixed, mixed)
        rules.stop(foreign); foreign = None
        rules.command('connect', OUTPUTS[2] + ':capture_FL', MONITOR + '.input:playback_FL')
        graph.wait_for(lambda: record()['state'] == 'conflict')
        graph.wait_for(lambda: not any(l['output']['node_name'] == MONITOR + '.output' for l in graph.query('links')))
        rules.command('disconnect', OUTPUTS[2] + ':capture_FL', MONITOR + '.input:playback_FL')
        graph.wait_for(lambda: ready(OUTPUTS[:2])); level(mixed, mixed)
        rules.command('enable-monitor', NAME, 'off'); graph.wait_for(lambda: not any(l['output']['node_name'].startswith(MONITOR) or l['input']['node_name'].startswith(MONITOR) for l in graph.query('links')))
        level(mixed, 0)
        rules.command('enable-monitor', NAME, 'on'); graph.wait_for(lambda: ready(OUTPUTS[:2])); level(mixed, mixed)
        rules.command('delete-monitor', NAME); graph.wait_for(lambda: not any(n['kind'] == 'monitor' and n['group'] == NAME for n in graph.query()['nodes']))
        assert {l['id'] for l in graph.query('links') if l['input']['node_name'] == INPUTS[2]} == manual_ids
        assert all(rules.command('get-default', k).stdout == v for k, v in defaults.items())
        print('PASS foreign configuration cannot steal/delete monitor; unmanaged monitor inputs stop output, deletion/disable preserve main paths and defaults', flush=True)
    finally:
        for process in tones: process.terminate(); process.wait(timeout=3)
        rules.stop(foreign)
        if os.path.exists(CONFIG + '/pipemixer/monitors/' + NAME + '.json'):
            with open(CONFIG + '/pipemixer/monitors/' + NAME + '.json', 'w') as file: file.write(original)
        rules.command('delete-monitor', NAME, check=False)
        if daemon and daemon.poll() is None:
            graph.wait_for(lambda: not any(n['kind'] == 'monitor' and n['group'] == NAME for n in graph.query()['nodes']))
        rules.stop(daemon)
        graph.remove_sink(graph.PREFIX + 'monitor-late-source')
        for name in reversed(GROUPS): rules.command('delete-bus', name)
        shutil.rmtree(CONFIG, ignore_errors=True); shutil.rmtree(CONFIG + '-foreign', ignore_errors=True)
    graph.reference_check()
    print('INDEPENDENT MONITOR AND SOLO PASSED', flush=True)


if __name__ == '__main__': main()
