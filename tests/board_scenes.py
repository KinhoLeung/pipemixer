"""Scene restoration against real PipeWire and PCM, using isolated paths."""
import copy
import json
import os
import shutil
import subprocess
import time

import board_graph as graph
from board_audio import capture, PROBE
from board_routing import Tui

CONFIG = '/tmp/board/scene-test-config'
DIRECTORY = CONFIG + '/pipemixer/scenes'
graph.ENV['XDG_CONFIG_HOME'] = CONFIG
BUSES = ['test_scene_a', 'test_scene_b', 'test_scene_guard']
EFFECTS = ['test_scene_eq', 'test_scene_custom']
SEND = 'test_scene_send'


def command(*args, check=True):
    result = subprocess.run([graph.BINARY, *args], env=graph.ENV, capture_output=True,
                            text=True, timeout=40)
    if check and result.returncode:
        raise AssertionError('%s: %s' % (args, result.stderr))
    return result


def write_scene(name, data):
    with open(DIRECTORY + '/' + name + '.json', 'w') as file:
        json.dump(data, file)


def level(name):
    return json.loads(command('--json', 'get-volume', name).stdout)


def parameters(name):
    return {p['name']: p['value'] for p in json.loads(command('--json', 'effect-params', name).stdout)}


def connect(source, destination):
    ports = graph.query('ports')
    for channel in ['FL', 'FR']:
        output = next(p for p in ports if p['node_name'] == source and p['direction'] == 'output' and p['channel'] == channel)
        inp = next(p for p in ports if p['node_name'] == destination and p['direction'] == 'input' and p['channel'] == channel)
        command('connect', source + ':' + output['name'], destination + ':' + inp['name'])


def clear_paths():
    command('delete-send', SEND)
    for name in EFFECTS:
        command('delete-effect', name)
    for name in BUSES[:2]:
        command('delete-bus', name)


def pcm_level():
    tone = subprocess.Popen([PROBE, 'play', graph.PREFIX + 'scene-tone',
                             'pipemixer.bus.test_scene_a.input', '1000'], env=graph.ENV,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        graph.wait_for(lambda: any(n['name'] == graph.PREFIX + 'scene-tone' for n in graph.query()['nodes']))
        time.sleep(.4)
        return capture('pipemixer.bus.test_scene_b.output')[0]
    finally:
        tone.terminate(); tone.wait(timeout=3)
        graph.wait_for(lambda: not any(n['name'] == graph.PREFIX + 'scene-tone' for n in graph.query()['nodes']))


def main():
    ui = None
    before = {kind: command('get-default', kind).stdout.strip() for kind in ['sink', 'source']}
    baseline = graph.query()
    custom_file = '/tmp/board/scene-custom.conf'
    try:
        assert not any(n['group'] in BUSES + EFFECTS + [SEND] for n in baseline['nodes'])
        for name in BUSES[:2]:
            command('create-bus', name)
        command('create-effect', EFFECTS[0], 'eq')
        with open(custom_file, 'w') as file:
            file.write('filter.graph = { nodes = [ { type=builtin name=gain label=linear control={ Mult=0.5 Add=0 } } ]'
                       ' inputs=["gain:In"] outputs=["gain:Out"] }')
        command('create-effect', EFFECTS[1], '@' + custom_file)
        connect('pipemixer.bus.test_scene_a.output', 'pipemixer.effect.test_scene_eq.input')
        connect('pipemixer.effect.test_scene_eq.output', 'pipemixer.effect.test_scene_custom.input')
        command('create-send', SEND, 'pipemixer.effect.test_scene_custom.output', 'pipemixer.bus.test_scene_b.input')
        # WirePlumber retains filter controls by node name between test runs.
        # Start the fixture from the preset defaults, including wet/dry gains.
        time.sleep(.6)
        for name in EFFECTS:
            for param in json.loads(command('--json', 'effect-params', name).stdout):
                if param['writable']:
                    command('set-effect-param', name, param['name'], str(param['default']))
        for name in ['pipemixer.bus.test_scene_a.input', 'pipemixer.bus.test_scene_a.output',
                     'pipemixer.bus.test_scene_b.input', 'pipemixer.bus.test_scene_b.output',
                     'pipemixer.effect.test_scene_eq.input', 'pipemixer.effect.test_scene_eq.output',
                     'pipemixer.effect.test_scene_custom.input', 'pipemixer.effect.test_scene_custom.output',
                     'pipemixer.send.test_scene_send.input', 'pipemixer.send.test_scene_send.output']:
            command('set-volume', name, '100'); command('set-mute', name, 'off')
        command('set-volume', 'pipemixer.bus.test_scene_a.output', '70', 'FL')
        command('set-volume', 'pipemixer.bus.test_scene_a.output', '40', 'FR')
        command('set-volume', 'pipemixer.send.test_scene_send.output', '80')
        command('set-effect-param', EFFECTS[0], 'mid:Gain', '6')
        command('set-effect-param', EFFECTS[1], 'gain:Mult', '.25')
        reference = pcm_level()
        assert .002 < reference < .03, reference
        command('set-default', 'pipemixer.bus.test_scene_b.input')
        command('set-default', 'pipemixer.bus.test_scene_a.output')
        command('save-scene', 'test_scene_one')
        command('check-scene', 'test_scene_one')
        with open(DIRECTORY + '/test_scene_one.json') as file:
            scene = json.load(file)
        assert len(scene['paths']) == 5, scene['paths']
        assert all('id' not in n for n in scene['nodes'])
        assert next(p for p in scene['paths'] if p['name'] == EFFECTS[1])['graph'].startswith('{')
        assert os.stat(DIRECTORY + '/test_scene_one.json').st_mode & 0o777 == 0o600
        print('PASS self-contained scene saves buses, sends, custom graph, channels and defaults', flush=True)

        command('set-volume', 'pipemixer.bus.test_scene_a.output', '90')
        command('bypass-effect', EFFECTS[0], 'on')
        command('set-mute', 'pipemixer.send.test_scene_send.output', 'on')
        command('save-scene', 'test_scene_two')
        command('load-scene', 'test_scene_one')
        assert parameters(EFFECTS[0])['wet:Mult'] == 1
        volumes = level('pipemixer.bus.test_scene_a.output')['channels']
        assert [round(v['percent']) for v in volumes] == [70, 40], volumes
        assert command('get-mute', 'pipemixer.send.test_scene_send.output').stdout.strip() == 'off'
        assert abs(pcm_level() - reference) < reference * .06
        command('load-scene', 'test_scene_two')
        assert parameters(EFFECTS[0])['wet:Mult'] == 0
        assert command('get-mute', 'pipemixer.send.test_scene_send.output').stdout.strip() == 'on'
        command('load-scene', 'test_scene_one')
        print('PASS scene switching restores per-channel levels, mute, EQ and bypass', flush=True)

        old_ids = {n['name']: n['id'] for n in graph.query()['nodes'] if n['kind']}
        for kind, name in before.items():
            command('set-default', name)
        clear_paths()
        command('create-bus', BUSES[2])
        os.unlink(custom_file)
        command('load-scene', 'test_scene_one')
        current = graph.query()
        assert any(n['name'] in old_ids and n['id'] != old_ids[n['name']] for n in current['nodes'])
        assert any(n['group'] == BUSES[2] for n in current['nodes'])
        assert command('get-default', 'sink').stdout.strip() == 'pipemixer.bus.test_scene_b.input'
        assert command('get-default', 'source').stdout.strip() == 'pipemixer.bus.test_scene_a.output'
        assert parameters(EFFECTS[1])['gain:Mult'] == .25
        assert abs(pcm_level() - reference) < reference * .06
        ids = {n['name']: n['id'] for n in current['nodes']}
        command('load-scene', 'test_scene_one')
        assert ids == {n['name']: n['id'] for n in graph.query()['nodes']}
        print('PASS recreation uses new node/port IDs, preserves other paths and restores PCM RMS %.6f' % reference, flush=True)

        # Additional connections between saved nodes are removed on load.
        connect('pipemixer.bus.test_scene_a.output', 'pipemixer.bus.test_scene_b.input')
        command('load-scene', 'test_scene_one')
        assert not any(l['output']['node_name'] == 'pipemixer.bus.test_scene_a.output'
                       and l['input']['node_name'] == 'pipemixer.bus.test_scene_b.input' for l in graph.query('links'))

        before_params = parameters(EFFECTS[0])
        invalid = copy.deepcopy(scene)
        invalid['nodes'].append({'name': 'pipemixer.board-test.missing-device', 'class': 'Audio/Source',
                                 'channels': [], 'params': [], 'mute': False})
        write_scene('test_missing', invalid)
        assert command('load-scene', 'test_missing', check=False).returncode == 3
        invalid = copy.deepcopy(scene)
        next(p for p in invalid['paths'] if p['name'] == EFFECTS[0])['preset'] = 'voice'
        write_scene('test_conflict', invalid)
        assert command('load-scene', 'test_conflict', check=False).returncode == 3
        invalid = copy.deepcopy(scene)
        eq = next(n for n in invalid['nodes'] if n['name'] == 'pipemixer.effect.test_scene_eq.input')
        next(p for p in eq['params'] if p['name'] == 'mid:Gain')['value'] = 999
        write_scene('test_invalid_param', invalid)
        assert command('load-scene', 'test_invalid_param', check=False).returncode == 1
        assert parameters(EFFECTS[0]) == before_params
        invalid = copy.deepcopy(scene)
        ports = graph.query('ports')
        out = next(p for p in ports if p['node_name'] == 'pipemixer.bus.test_scene_b.output' and p['channel'] == 'FL')
        inp = next(p for p in ports if p['node_name'] == 'pipemixer.bus.test_scene_a.input' and p['channel'] == 'FL')
        invalid['links'].append({'output': {'node': out['node_name'], 'port': out['name']},
                                 'input': {'node': inp['node_name'], 'port': inp['name']}})
        write_scene('test_cycle_scene', invalid)
        assert command('check-scene', 'test_cycle_scene', check=False).returncode == 1
        assert command('load-scene', 'test_cycle_scene', check=False).returncode == 1
        with open(DIRECTORY + '/test_malformed.json', 'w') as file:
            file.write('{"format":"pipemixer.scene","nodes":[')
        assert command('check-scene', 'test_malformed', check=False).returncode == 1
        assert command('save-scene', '../escape', check=False).returncode == 2
        print('PASS missing devices, conflicting paths, invalid DSP values, cycles and malformed files reject safely', flush=True)

        concurrent = [subprocess.Popen([graph.BINARY, 'save-scene', 'test_concurrent'], env=graph.ENV,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(2)]
        for process in concurrent:
            output, error = process.communicate(timeout=12)
            assert process.returncode == 0, error
        command('check-scene', 'test_concurrent')
        offline_env = dict(graph.ENV, PIPEWIRE_REMOTE='pipemixer-no-server')
        assert subprocess.run([graph.BINARY, '--json', 'list-scenes'], env=offline_env,
                              capture_output=True, timeout=3).returncode == 0
        print('PASS atomic concurrent saves and scene listing without PipeWire', flush=True)

        ui = Tui()
        assert b'Scenes' in ui.send(b's')
        ui.send(b'\n')
        graph.wait_for(lambda: 'scene1' in json.loads(command('--json', 'list-scenes').stdout))
        command('set-volume', 'pipemixer.bus.test_scene_a.output', '20')
        ui.send(b'sj\n')
        graph.wait_for(lambda: round(level('pipemixer.bus.test_scene_a.output')['channels'][0]['percent']) == 70)
        ui.drain(.5); ui.resize(8, 30); ui.resize(24, 100)
        ui.send(b'sjj\n')
        graph.wait_for(lambda: 'scene1' not in json.loads(command('--json', 'list-scenes').stdout))
        ui.close(); ui = None
        print('PASS TUI scene save/load/delete and terminal resize', flush=True)
    finally:
        if ui:
            ui.close()
        for name in before.values():
            command('set-default', name)
        clear_paths(); command('delete-bus', BUSES[2])
        if os.path.exists(custom_file):
            os.unlink(custom_file)
        shutil.rmtree(CONFIG, ignore_errors=True)
    assert graph.query() == baseline
    graph.reference_check()
    print('SCENE SAVE/LOAD PASSED', flush=True)


if __name__ == '__main__':
    main()
