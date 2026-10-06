"""Persistent rules with startup scenes, audio-session restart and board reboot."""
import json
import os
import sys
import time

import board_startup as boot
import board_rules as rules

boot.graph.ENV['XDG_CONFIG_HOME'] = '/root/.config'
RULES = ['test_boot_route_' + c for c in ['left', 'right', 'late']]
PAIRS = [('pipemixer.bus.test_boot_mix.output:capture_' + c,
          'pipemixer.effect.test_boot_eq.input:playback_' + c) for c in ['FL', 'FR']]
LATE = (boot.EXTERNAL + ':monitor_FL', 'pipemixer.bus.test_boot_mix.input:playback_FL')


def prepare():
    assert rules.status()['engine_running'], 'Install/start the new audio supervisor first'
    assert not any(r['name'] in RULES for r in rules.status()['rules'])
    boot.prepare()
    for name, pair in zip(RULES, PAIRS + [LATE]):
        rules.command('create-route-rule', name, *pair)
    os.sync()
    print('PERSISTENT ROUTING FIXTURE PREPARED', flush=True)


def verify(reboot=False):
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        result = boot.command('--json', 'list-route-rules', check=False)
        assert result.returncode >= 0, result
        if result.returncode == 0 and json.loads(result.stdout)['engine_running']:
            break
        time.sleep(.25)
    else:
        raise AssertionError(open('/tmp/pipemixer-session.log').read())
    boot.verify(reboot)
    boot.graph.wait_for(lambda: rules.status()['engine_running'])
    boot.graph.wait_for(lambda: all(rules.state(n) == 'connected' for n in RULES[:2]))
    assert rules.state(RULES[2]) == 'waiting'
    rules.command('disconnect', *PAIRS[0])
    boot.graph.wait_for(lambda: rules.between(*PAIRS[0]))
    with open(boot.STATE) as file:
        saved = json.load(file)
    measured = boot.pcm()
    assert abs(measured - saved['reference_rms']) < saved['reference_rms'] * .06
    boot.graph.create_sink(boot.EXTERNAL)
    try:
        boot.graph.wait_for(lambda: rules.state(RULES[2]) == 'connected')
        volumes = json.loads(boot.command('--json', 'get-volume', 'pipemixer.bus.test_boot_mix.output').stdout)['channels']
        assert [round(c['percent']) for c in volumes] == [60, 90]
        assert boot.params(boot.EFFECTS[0])['mid:Gain'] == 3
        assert abs(boot.pcm() - saved['reference_rms']) < saved['reference_rms'] * .06
    finally:
        boot.graph.remove_sink(boot.EXTERNAL)
    boot.graph.wait_for(lambda: rules.state(RULES[2]) == 'waiting')
    print('PASS persistent routing engine after %s, lost link repair and late endpoint without resetting levels/EQ; PCM %.6f' %
          ('board reboot' if reboot else 'audio session restart', measured), flush=True)


def cleanup():
    for name in RULES: rules.command('delete-route-rule', name)
    boot.cleanup()
    assert not any(r['name'] in RULES for r in rules.status()['rules'])
    print('PERSISTENT ROUTING FIXTURE CLEANED', flush=True)


if __name__ == '__main__':
    if sys.argv[1] == 'prepare': prepare()
    elif sys.argv[1] == 'verify': verify('--reboot' in sys.argv)
    elif sys.argv[1] == 'cleanup': cleanup()
    else: raise SystemExit('Use prepare, verify [--reboot], or cleanup')
