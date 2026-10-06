"""Real ALSA Profile/Route scene recovery and card re-enumeration on Luckfox."""
import copy
import json
import os
import shutil
import subprocess
import time

import board_graph as graph
import board_audio as audio
import board_rules as rules

CONFIG = '/tmp/board/profile-test-config'
graph.ENV['XDG_CONFIG_HOME'] = CONFIG
SCENE, BEFORE = 'test_hardware_restore', 'test_hardware_before'
BUS, SEND, MONITOR = 'test_profile_bus', 'test_profile_send', 'test_profile_monitor'
LINKBUS = 'test_profile_dependency'
VIRTUAL = graph.PREFIX + 'profile-source'
PHONES = graph.PREFIX + 'profile-phones'
GUARD = 'pipemixer.bus.' + BUS
MON = 'pipemixer.monitor.' + MONITOR


def command(*args, check=True):
    result = subprocess.run([graph.BINARY, *args], env=graph.ENV, capture_output=True, text=True, timeout=40)
    if check and result.returncode: raise AssertionError('%s: %s' % (args, result.stderr))
    return result


def wait(predicate, timeout=35):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            value = predicate()
            if value: return value
        except (subprocess.SubprocessError, json.JSONDecodeError): pass
        time.sleep(.2)
    raise AssertionError('Hardware recovery did not converge: ' + command('--json', 'recovery-status', check=False).stdout)


def profiles(device):
    result = command('--json', 'list-profiles', device, check=False)
    return json.loads(result.stdout) if not result.returncode else []


def active(device):
    return next((p['name'] for p in profiles(device) if p['active']), None)


def volumes(node):
    return [round(c['percent']) for c in json.loads(command('--json', 'get-volume', node).stdout)['channels']]


def monitor_ready():
    records = json.loads(command('--json', 'list-monitors').stdout)['monitors']
    record = next((m for m in records if m['name'] == MONITOR), None)
    return record and record['state'] == 'connected' and record['mode'] == 'solo' and record['active_sources'] == [VIRTUAL]


def pcm():
    tone = subprocess.Popen([audio.PROBE, 'play', graph.PREFIX + 'profile-tone', VIRTUAL, '440'], env=graph.ENV,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(.4)
        return audio.capture(GUARD + '.output', PHONES, sink_targets=[PHONES])
    finally:
        tone.terminate(); tone.wait(timeout=3)


def main():
    shutil.rmtree(CONFIG, ignore_errors=True)
    before = graph.query(); engine = None; saved_before = False
    devices = json.loads(command('--json', 'list', 'devices').stdout)
    device = next(d['name'] for d in devices if 'usb_gadget' in d['name'])
    original_profile = active(device)
    desired = 'output:stereo-fallback+input:stereo-fallback'
    assert original_profile == desired, original_profile
    sink = next(n['name'] for n in before['nodes'] if n['class'] == 'Audio/Sink' and 'usb_gadget' in n['name'])
    source = next(n['name'] for n in before['nodes'] if n['class'] == 'Audio/Source' and 'usb_gadget' in n['name'])
    try:
        command('save-scene', BEFORE); saved_before = True
        graph.create_sink(VIRTUAL); graph.create_sink(PHONES)
        command('create-bus', BUS); command('create-bus', LINKBUS)
        # The gadget PCM clock depends on its USB host. Keep its dependent
        # connection in a separate graph from the measured virtual main mix.
        command('create-send', SEND, sink, 'pipemixer.bus.' + LINKBUS + '.input')
        for c in ['FL', 'FR']: command('connect', VIRTUAL + ':monitor_' + c, GUARD + '.input:playback_' + c)
        for n in graph.query()['nodes']:
            if n['group'] in [BUS, LINKBUS, SEND]: command('set-volume', n['name'], '100'); command('set-mute', n['name'], 'off')
        command('set-volume', GUARD + '.output', '70')
        command('set-volume', sink, '61', 'FL'); command('set-volume', sink, '83', 'FR'); command('set-mute', sink, 'on')
        command('set-volume', source, '47'); command('set-mute', source, 'on')
        command('create-monitor', MONITOR, PHONES, VIRTUAL)
        command('solo-monitor', MONITOR, VIRTUAL, 'on')
        engine = rules.start(); wait(monitor_ready)
        command('set-volume', MON + '.output', '100'); command('set-mute', MON + '.output', 'off')
        command('save-scene', SCENE); command('set-startup-scene', SCENE)
        path = CONFIG + '/pipemixer/scenes/' + SCENE + '.json'
        saved = json.load(open(path))
        assert saved['version'] == 2
        assert next(d['profile'] for d in saved['devices'] if d['name'] == device) == desired
        assert all('id' not in d and 'index' not in d for d in saved['devices'])
        sink_saved = next(n for n in saved['nodes'] if n['name'] == sink)
        assert sink_saved['device'] == device and sink_saved['route'] == 'analog-output'
        print('PASS scene v2 saves stable hardware device/profile/route names', flush=True)
        off = next(p['index'] for p in profiles(device) if p['name'] == 'off')
        command('set-profile', device, str(off))
        wait(lambda: not any(n['name'] == sink for n in graph.query()['nodes']))
        command('restore-startup')
        wait(lambda: monitor_ready() and active(device) == desired)
        assert volumes(sink) == [61, 83] and volumes(source) == [47, 47]
        assert command('get-mute', sink).stdout.strip() == 'on' and command('get-mute', source).stdout.strip() == 'on'
        routes = json.loads(command('--json', 'list-routes', sink).stdout)
        assert next(r['name'] for r in routes if r['active']) == 'analog-output'
        assert any(n['group'] == SEND for n in graph.query()['nodes'])
        print('PASS present card restores Profile from off, waits for nodes, then Route/gain/mute/send/monitor', flush=True)

        command('set-volume', GUARD + '.output', '57')
        reference = pcm(); assert reference[0] > .001 and reference[1] > .05, reference
        old_id = next(d['id'] for d in json.loads(command('--json', 'list', 'devices').stdout) if d['name'] == device)
        old_serial = next(o['info']['props']['object.serial'] for o in graph.dump() if o['type'].endswith(':Device') and o['info']['props'].get('device.name') == device)
        command('set-profile', device, str(off)); time.sleep(1.5)
        assert active(device) == 'off', 'Completed device generation must respect manual profile changes'
        wp = subprocess.run(['pidof', 'wireplumber'], capture_output=True, text=True, check=True).stdout.split()
        assert len(wp) == 1
        os.kill(int(wp[0]), 15)
        wait(lambda: active(device) == desired and monitor_ready())
        wait(lambda: volumes(sink) == [61, 83] and volumes(source) == [47, 47])
        new_id = next(d['id'] for d in json.loads(command('--json', 'list', 'devices').stdout) if d['name'] == device)
        new_serial = next(o['info']['props']['object.serial'] for o in graph.dump() if o['type'].endswith(':Device') and o['info']['props'].get('device.name') == device)
        assert new_serial != old_serial, (old_serial, new_serial)
        wait(lambda: next(d['state'] for d in json.loads(command('--json', 'recovery-status').stdout)['devices'] if d['name'] == device) == 'restored')
        current = json.loads(command('--json', 'recovery-status').stdout)
        assert next(d['state'] for d in current['devices'] if d['name'] == device) == 'restored', current
        assert next(n['route_state'] for n in current['nodes'] if n['name'] == sink) == 'restored'
        assert command('get-mute', sink).stdout.strip() == 'on' and command('get-mute', source).stdout.strip() == 'on'
        assert volumes(GUARD + '.output') == [57, 57]
        wait(lambda: len([l for l in graph.query('links') if l['output']['node_name'] == sink and l['input']['node_name'] == 'pipemixer.send.' + SEND + '.input']) == 2)
        measured = pcm()
        assert all(abs(a - b) < b * .06 for a, b in zip(measured, reference)), (measured, reference)
        assert engine.poll() is None
        print('PASS re-enumerated real card restores saved Profile/Route/levels and dependent send, keeps live bus gain and Solo; IDs %d -> %d PCM %s' % (old_id, new_id, measured), flush=True)

        bad = copy.deepcopy(saved); next(d for d in bad['devices'] if d['name'] == device)['profile'] = 'no-such-hardware-profile'
        bad_path = CONFIG + '/pipemixer/scenes/test_profile_bad.json'
        with open(bad_path, 'w') as file: json.dump(bad, file)
        assert command('check-scene', 'test_profile_bad').returncode == 0
        assert command('load-scene', 'test_profile_bad', check=False).returncode != 0
        assert active(device) == desired
        command('set-startup-scene', 'test_profile_bad'); command('restore-startup')
        wait(lambda: next(d['state'] for d in json.loads(command('--json', 'recovery-status').stdout)['devices'] if d['name'] == device) == 'blocked')
        assert active(device) == desired and monitor_ready()
        command('set-startup-scene', SCENE); command('restore-startup')
        bad = copy.deepcopy(saved); next(n for n in bad['nodes'] if n['name'] == sink)['route'] = 'no-such-hardware-route'
        with open(bad_path, 'w') as file: json.dump(bad, file)
        assert command('load-scene', 'test_profile_bad', check=False).returncode != 0
        command('set-startup-scene', 'test_profile_bad'); command('restore-startup')
        wait(lambda: next(n['route_state'] for n in json.loads(command('--json', 'recovery-status').stdout)['nodes'] if n['name'] == sink) == 'blocked')
        assert active(device) == desired and monitor_ready()
        legacy = copy.deepcopy(saved); legacy['version'] = 1; legacy.pop('devices')
        for n in legacy['nodes']: n.pop('device', None); n.pop('route', None)
        with open(CONFIG + '/pipemixer/scenes/test_profile_legacy.json', 'w') as file: json.dump(legacy, file)
        command('check-scene', 'test_profile_legacy'); command('load-scene', 'test_profile_legacy')
        print('PASS unavailable hardware names reject/wait without changing mode, blocked Route status and v1 scene compatibility', flush=True)
    finally:
        command('clear-recovery', check=False)
        command('delete-monitor', MONITOR, check=False)
        if engine and engine.poll() is None:
            wait(lambda: not any(n['group'] == MONITOR for n in graph.query()['nodes']))
        rules.stop(engine)
        command('delete-send', SEND, check=False); command('delete-bus', BUS, check=False); command('delete-bus', LINKBUS, check=False)
        for name in [VIRTUAL, PHONES]: graph.remove_sink(name)
        if saved_before: command('load-scene', BEFORE)
        if saved_before:
            saved = json.load(open(CONFIG + '/pipemixer/scenes/' + BEFORE + '.json'))
            for node in saved['nodes']:
                assert volumes(node['name']) == [round(c['volume']) for c in node['channels']]
                assert (command('get-mute', node['name']).stdout.strip() == 'on') == node['mute']
        for name in [SCENE, BEFORE, 'test_profile_bad', 'test_profile_legacy']: command('delete-scene', name, check=False)
        command('set-startup-scene', 'off', check=False); command('clear-recovery', check=False)
        shutil.rmtree(CONFIG, ignore_errors=True)
        wait(lambda: len(subprocess.run(['pidof', 'wireplumber'], capture_output=True, text=True).stdout.split()) == 1)
        assert sorted(n['name'] for n in graph.query()['nodes']) == sorted(n['name'] for n in before['nodes'])
        graph.reference_check()
    print('HARDWARE PROFILE/ROUTE RECOVERY PASSED', flush=True)


if __name__ == '__main__': main()
