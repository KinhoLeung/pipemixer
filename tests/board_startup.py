"""Persistent startup fixture: prepare, verify after reboot, then cleanup.

Copy helpers and audio-probe to /root/.local/share/pipemixer-tests first.
Only isolated virtual paths carry the test tones.
"""
import json
import os
import subprocess
import sys
import time

import board_graph as graph
import board_audio as audio
from board_routing import Tui

DATA = '/root/.local/share/pipemixer-tests'
STATE = DATA + '/startup-test-state.json'
SCENE = 'test_boot_restore'
BEFORE = 'test_boot_before'
BUSES = ['test_boot_mix', 'test_boot_out']
EFFECTS = ['test_boot_eq', 'test_boot_custom']
SEND = 'test_boot_send'
OPTIONAL = 'test_boot_optional'
EXTERNAL = graph.PREFIX + 'boot-external'
PROBE = DATA + '/audio-probe'
graph.ENV['XDG_CONFIG_HOME'] = '/root/.config'
audio.PROBE = PROBE
os.makedirs('/tmp/board', exist_ok=True)


def command(*args, check=True, env=None):
    result = subprocess.run([graph.BINARY, *args], env=env or graph.ENV, text=True,
                            capture_output=True, timeout=40)
    if check and result.returncode:
        raise AssertionError('%s: %s' % (args, result.stderr))
    return result


def params(name):
    return {p['name']: p['value'] for p in json.loads(command('--json', 'effect-params', name).stdout)}


def connect(source, destination):
    ports = graph.query('ports')
    for channel in ['FL', 'FR']:
        out = next(p for p in ports if p['node_name'] == source and p['direction'] == 'output' and p['channel'] == channel)
        inp = next(p for p in ports if p['node_name'] == destination and p['direction'] == 'input' and p['channel'] == channel)
        command('connect', source + ':' + out['name'], destination + ':' + inp['name'])


def pcm():
    name = graph.PREFIX + 'boot-tone'
    tone = subprocess.Popen([PROBE, 'play', name, 'pipemixer.bus.test_boot_mix.input', '1000'],
                            env=graph.ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        graph.wait_for(lambda: any(n['name'] == name for n in graph.query()['nodes']))
        time.sleep(.4)
        return audio.capture('pipemixer.bus.test_boot_out.output')[0]
    finally:
        tone.terminate(); tone.wait(timeout=3)
        graph.wait_for(lambda: not any(n['name'] == name for n in graph.query()['nodes']))


def clear_paths():
    for name in [SEND, OPTIONAL]:
        command('delete-send', name)
    for name in EFFECTS:
        command('delete-effect', name)
    for name in BUSES:
        command('delete-bus', name)


def prepare():
    assert not os.path.exists(STATE), 'Previous startup fixture still exists'
    scenes = json.loads(command('--json', 'list-scenes').stdout)
    assert SCENE not in scenes and BEFORE not in scenes
    assert not any(n['group'] in BUSES + EFFECTS + [SEND, OPTIONAL] for n in graph.query()['nodes'])
    previous = command('get-startup-scene').stdout.strip()
    before_nodes = sorted(n['name'] for n in graph.query()['nodes'])
    command('save-scene', BEFORE)
    tone = None
    custom = DATA + '/boot-custom.conf'
    try:
        for name in BUSES:
            command('create-bus', name)
        command('create-effect', EFFECTS[0], 'eq')
        with open(custom, 'w') as file:
            file.write('filter.graph = { nodes=[{ type=builtin name=gain label=linear control={Mult=0.5 Add=0} }]'
                       ' inputs=["gain:In"] outputs=["gain:Out"] }')
        command('create-effect', EFFECTS[1], '@' + custom)
        connect('pipemixer.bus.test_boot_mix.output', 'pipemixer.effect.test_boot_eq.input')
        connect('pipemixer.effect.test_boot_eq.output', 'pipemixer.effect.test_boot_custom.input')
        command('create-send', SEND, 'pipemixer.effect.test_boot_custom.output', 'pipemixer.bus.test_boot_out.input')
        for node in graph.query()['nodes']:
            if node['group'] in BUSES + EFFECTS + [SEND]:
                command('set-volume', node['name'], '100'); command('set-mute', node['name'], 'off')
        command('set-volume', 'pipemixer.bus.test_boot_mix.output', '60', 'FL')
        command('set-volume', 'pipemixer.bus.test_boot_mix.output', '90', 'FR')
        command('set-volume', 'pipemixer.send.test_boot_send.output', '75')
        command('set-effect-param', EFFECTS[0], 'mid:Gain', '3')
        command('set-effect-param', EFFECTS[1], 'gain:Mult', '.4')
        reference = pcm()
        assert .003 < reference < .04, reference
        graph.create_sink(EXTERNAL)
        command('create-send', OPTIONAL, EXTERNAL, 'pipemixer.bus.test_boot_mix.input')
        command('set-volume', EXTERNAL, '53', 'FL'); command('set-volume', EXTERNAL, '81', 'FR')
        command('set-mute', EXTERNAL, 'on')
        command('set-volume', 'pipemixer.send.' + OPTIONAL + '.output', '76')
        tone = subprocess.Popen([PROBE, 'play', graph.PREFIX + 'boot-transient', EXTERNAL, '440'],
                                env=graph.ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        graph.wait_for(lambda: any(n['name'] == graph.PREFIX + 'boot-transient' for n in graph.query()['nodes']))
        command('save-scene', SCENE)
        with open('/root/.config/pipemixer/scenes/' + SCENE + '.json') as file:
            saved = json.load(file)
        transient = next(n for n in saved['nodes'] if n['name'] == graph.PREFIX + 'boot-transient')
        assert [c['name'] for c in transient['channels']] == ['FL', 'FR'], transient
        assert command('set-startup-scene', 'test_no_such_scene', check=False).returncode == 1
        assert command('get-startup-scene').stdout.strip() == previous
        offline = dict(graph.ENV, PIPEWIRE_REMOTE='pipemixer-no-server')
        command('set-startup-scene', SCENE, env=offline)
        assert json.loads(command('--json', 'get-startup-scene', env=offline).stdout) == SCENE
        pointer = '/root/.config/pipemixer/startup-scene'
        assert os.stat(pointer).st_mode & 0o777 == 0o600
        assert command('restore-startup').returncode == 0
        ui = Tui()
        try:
            screen = ui.send(b's')
            assert b'Scenes' in screen and b'startup' in screen
            # Sorted test_boot_before, then test_boot_restore; select startup.
            names = json.loads(command('--json', 'list-scenes').stdout)
            index = names.index(SCENE)
            ui.send(b'j' * (3 + index * 3) + b'\n')
            assert command('get-startup-scene').stdout.strip() == SCENE
            ui.send(b's' + b'j' * (1 + len(names) * 3) + b'\n')
            assert command('get-startup-scene').stdout.strip() == 'off'
        finally:
            ui.close()
        command('set-startup-scene', SCENE)
        tone.terminate(); tone.wait(timeout=3); tone = None
        graph.remove_sink(EXTERNAL)
        clear_paths()
        os.unlink(custom)
        result = command('restore-startup')
        assert 'skipped' in result.stderr, result.stderr
        assert not any(n['group'] == OPTIONAL for n in graph.query()['nodes'])
        assert abs(pcm() - reference) < reference * .06
        clear_paths()
        state = {'previous_startup': previous, 'before_nodes': before_nodes, 'reference_rms': reference,
                 'before_boot_id': open('/proc/sys/kernel/random/boot_id').read().strip(),
                 'scene': SCENE}
        with open(STATE, 'w') as file:
            json.dump(state, file)
        os.chmod(STATE, 0o600)
        os.sync()
        print('PASS offline startup selection, TUI selection/disable, missing-device pruning and PCM %.6f' % reference, flush=True)
        print('STARTUP FIXTURE PREPARED', flush=True)
    except BaseException:
        command('set-startup-scene', previous, check=False)
        clear_paths()
        graph.remove_sink(EXTERNAL)
        command('load-scene', BEFORE, check=False)
        command('delete-scene', SCENE, check=False); command('delete-scene', BEFORE, check=False)
        raise
    finally:
        if tone:
            tone.terminate(); tone.wait(timeout=3)


def verify(require_reboot=False):
    with open(STATE) as file:
        state = json.load(file)
    if require_reboot:
        assert open('/proc/sys/kernel/random/boot_id').read().strip() != state['before_boot_id']
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        try:
            groups = {n['group'] for n in graph.query()['nodes']}
            if groups >= set(BUSES + EFFECTS + [SEND]):
                break
        except (subprocess.SubprocessError, json.JSONDecodeError):
            pass
        time.sleep(.5)
    else:
        raise AssertionError(open('/tmp/pipemixer-session.log').read())
    assert command('get-startup-scene').stdout.strip() == SCENE
    volumes = json.loads(command('--json', 'get-volume', 'pipemixer.bus.test_boot_mix.output').stdout)['channels']
    assert [round(c['percent']) for c in volumes] == [60, 90], volumes
    assert params(EFFECTS[0])['mid:Gain'] == 3
    assert abs(params(EFFECTS[1])['gain:Mult'] - .4) < 1e-6
    assert command('get-mute', 'pipemixer.send.test_boot_send.output').stdout.strip() == 'off'
    assert round(json.loads(command('--json', 'get-volume', 'pipemixer.send.test_boot_send.output').stdout)['channels'][0]['percent']) == 75
    assert not any(n['group'] == OPTIONAL for n in graph.query()['nodes'])
    with open('/root/.config/pipemixer/scenes/' + SCENE + '.json') as file:
        snapshot = json.load(file)
    assert snapshot['version'] == 2
    for saved_device in snapshot['devices']:
        profiles = json.loads(command('--json', 'list-profiles', saved_device['name']).stdout)
        assert next(p['name'] for p in profiles if p['active']) == saved_device['profile']
    for node in snapshot['nodes']:
        if 'device' not in node: continue
        current = json.loads(command('--json', 'get-volume', node['name']).stdout)['channels']
        assert [round(c['percent']) for c in current] == [round(c['volume']) for c in node['channels']]
        assert (command('get-mute', node['name']).stdout.strip() == 'on') == node['mute']
        if 'route' in node:
            routes = json.loads(command('--json', 'list-routes', node['name']).stdout)
            assert next(r['name'] for r in routes if r['active']) == node['route']
    measured = pcm()
    assert abs(measured - state['reference_rms']) < state['reference_rms'] * .06, measured
    for process in ['pipewire', 'wireplumber']:
        result = subprocess.run(['pidof', process], capture_output=True, text=True)
        assert len(result.stdout.split()) == 1, result.stdout
    subprocess.run(['/etc/init.d/S98pipemixer', 'status'], check=True)
    subprocess.run(['/etc/init.d/S98pipemixer', 'start'], check=True)
    for process in ['pipewire', 'wireplumber']:
        assert len(subprocess.run(['pidof', process], capture_output=True, text=True).stdout.split()) == 1
    print('PASS %s restores buses, custom effect, EQ, channel volumes and PCM RMS %.6f' %
          ('full board reboot' if require_reboot else 'audio-service restart', measured), flush=True)
    graph.create_sink(EXTERNAL)
    try:
        graph.wait_for(lambda: any(n['group'] == OPTIONAL for n in graph.query()['nodes']))
        graph.wait_for(lambda: [round(c['percent']) for c in json.loads(command('--json', 'get-volume', EXTERNAL).stdout)['channels']] == [53, 81])
        graph.wait_for(lambda: round(json.loads(command('--json', 'get-volume', 'pipemixer.send.' + OPTIONAL + '.output').stdout)['channels'][0]['percent']) == 76)
        assert command('get-mute', EXTERNAL).stdout.strip() == 'on'
        assert abs(pcm() - state['reference_rms']) < state['reference_rms'] * .06
        print('PASS saved hardware Profile/Route/levels and late external gains/mute/skipped send automatically restored', flush=True)
    finally:
        graph.remove_sink(EXTERNAL)
    graph.wait_for(lambda: not any(n['group'] == OPTIONAL for n in graph.query()['nodes']))
    print('STARTUP RESTORE PASSED', flush=True)


def cleanup():
    with open(STATE) as file:
        state = json.load(file)
    command('set-startup-scene', state['previous_startup'])
    clear_paths(); graph.remove_sink(EXTERNAL)
    command('load-scene', BEFORE)
    command('delete-scene', SCENE); command('delete-scene', BEFORE)
    assert sorted(n['name'] for n in graph.query()['nodes']) == state['before_nodes']
    os.unlink(STATE)
    graph.reference_check()
    print('STARTUP FIXTURE CLEANED', flush=True)


if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'prepare': prepare()
    elif mode == 'verify': verify('--reboot' in sys.argv)
    elif mode == 'cleanup': cleanup()
    else: raise SystemExit('Use prepare, verify [--reboot], or cleanup')
