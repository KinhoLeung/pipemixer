"""Persistent editable dynamics chain across service and real board restarts."""
import json
import os
import subprocess
import sys
import time

import board_startup as boot

STATE = boot.DATA + '/dynamics-boot-state.json'
SCENE, BEFORE = 'test_dynamics_boot', 'test_dynamics_before'
EFFECT, SEND = 'test_dynamics_boot_fx', 'test_dynamics_boot_send'
BUSES = ['test_dynamics_boot_source', 'test_dynamics_boot_destination']
PARAMS = {'compressor1:Threshold dB': -30, 'compressor1:Ratio': 4,
          'compressor1:Attack ms': 1, 'compressor1:Release ms': 100,
          'compressor1:Knee dB': 0, 'limiter1:Ceiling dB': -18,
          'limiter1:Input dB': 6, 'limiter1:Release ms': 100}


def levels():
    tone_name = boot.graph.PREFIX + 'dynamics-boot-tone'
    tone = subprocess.Popen([boot.PROBE, 'play', tone_name, 'pipemixer.bus.' + BUSES[0] + '.input', '1000'],
                            env=boot.graph.ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        boot.graph.wait_for(lambda: any(n['name'] == tone_name for n in boot.graph.query()['nodes']))
        def volume_ready():
            result = boot.command('set-volume', tone_name, '100', check=False)
            if result.returncode:
                assert 'target has no volume channels' in result.stderr, result.stderr
            return result.returncode == 0
        boot.graph.wait_for(volume_ready)
        boot.command('set-mute', tone_name, 'off')
        return boot.audio.capture('pipemixer.effect.' + EFFECT + '.output', 'pipemixer.bus.' + BUSES[1] + '.output')
    finally:
        tone.terminate(); tone.wait(timeout=3)
        boot.graph.wait_for(lambda: not any(n['name'] == tone_name for n in boot.graph.query()['nodes']))


def wait_engine():
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        result = boot.command('--json', 'list-route-rules', check=False)
        if result.returncode == 0 and json.loads(result.stdout)['engine_running']:
            return
        time.sleep(.25)
    raise AssertionError('Persistent routing engine did not start')


def prepare():
    wait_engine()
    assert boot.command('get-startup-scene').stdout.strip() == 'off'
    assert json.loads(boot.command('--json', 'recovery-status').stdout)['scene'] is None
    assert not os.path.exists(STATE), 'Previous dynamics fixture still exists'
    scenes = json.loads(boot.command('--json', 'list-scenes').stdout)
    assert SCENE not in scenes and BEFORE not in scenes
    assert not any(n['group'] in BUSES + [EFFECT, SEND] for n in boot.graph.query()['nodes'])
    boot.command('save-scene', BEFORE)
    try:
        prepare_paths()
    except BaseException:
        cleanup()
        raise


def prepare_paths():
    for name in BUSES:
        boot.command('create-bus', name)
    boot.command('create-effect', EFFECT, 'empty')
    for processor in ['highpass', 'compressor', 'limiter']:
        boot.command('add-effect-stage', EFFECT, processor)
    for parameter, value in PARAMS.items():
        boot.command('set-effect-param', EFFECT, parameter, str(value))
    boot.command('bypass-effect-stage', EFFECT, 'highpass1', 'on')
    for kind, names in [('bus', BUSES), ('effect', [EFFECT])]:
        for name in names:
            for role in ['input', 'output']:
                target = 'pipemixer.%s.%s.%s' % (kind, name, role)
                boot.command('set-volume', target, '100'); boot.command('set-mute', target, 'off')
    boot.connect('pipemixer.bus.' + BUSES[0] + '.output', 'pipemixer.effect.' + EFFECT + '.input')
    boot.command('create-send', SEND, 'pipemixer.effect.' + EFFECT + '.output', 'pipemixer.bus.' + BUSES[1] + '.input')
    boot.command('set-volume', 'pipemixer.send.' + SEND + '.output', '60')
    boot.command('set-mute', 'pipemixer.send.' + SEND + '.output', 'off')
    reference = levels()
    assert .06 < reference[0] < .07 and .012 < reference[1] < .015, reference
    boot.command('save-scene', SCENE)
    boot.command('set-startup-scene', SCENE)
    with open(STATE, 'w') as file:
        json.dump({'reference': reference, 'boot_id': open('/proc/sys/kernel/random/boot_id').read().strip()}, file)
    os.chmod(STATE, 0o600); os.sync()
    print('DYNAMICS BOOT FIXTURE PREPARED: effect %.6f, Send %.6f' % tuple(reference), flush=True)


def verify(reboot=False):
    wait_engine()
    with open(STATE) as file:
        state = json.load(file)
    if reboot:
        assert open('/proc/sys/kernel/random/boot_id').read().strip() != state['boot_id']
    assert boot.command('get-startup-scene').stdout.strip() == SCENE
    chain = json.loads(boot.command('--json', 'effect-chain', EFFECT).stdout)
    assert [s['id'] for s in chain] == ['highpass1', 'compressor1', 'limiter1']
    assert [s['bypass'] for s in chain] == [True, False, False]
    actual = boot.params(EFFECT)
    for name, value in PARAMS.items():
        assert abs(actual[name] - value) < .00001, (name, actual[name], value)
    channels = json.loads(boot.command('--json', 'get-volume', 'pipemixer.send.' + SEND + '.output').stdout)['channels']
    assert all(abs(c['percent'] - 60) < .01 for c in channels)
    measured = levels()
    assert all(abs(a-b) < b*.035 for a, b in zip(measured, state['reference'])), (measured, state['reference'])
    print('PASS %s: installed DSP plugin, chain order, per-stage bypass, dynamics parameters, stereo connections and Send gain; PCM %.6f / %.6f' %
          ('real board reboot' if reboot else 'audio-service restart', measured[0], measured[1]), flush=True)


def cleanup():
    boot.command('set-startup-scene', 'off')
    boot.command('delete-send', SEND)
    boot.command('delete-effect', EFFECT)
    for name in BUSES:
        boot.command('delete-bus', name)
    boot.command('load-scene', BEFORE)
    boot.command('delete-scene', SCENE); boot.command('delete-scene', BEFORE)
    if os.path.exists(STATE):
        os.unlink(STATE)
    assert boot.command('get-startup-scene').stdout.strip() == 'off'
    assert json.loads(boot.command('--json', 'recovery-status').stdout)['scene'] is None
    boot.graph.reference_check()
    print('DYNAMICS BOOT FIXTURE CLEANED; previous audio settings restored', flush=True)


if __name__ == '__main__':
    if sys.argv[1] == 'prepare': prepare()
    elif sys.argv[1] == 'verify': verify('--reboot' in sys.argv)
    elif sys.argv[1] == 'cleanup': cleanup()
    else: raise SystemExit('Use prepare, verify [--reboot], or cleanup')
