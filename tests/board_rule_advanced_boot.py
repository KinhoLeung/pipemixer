"""Version 2 batch rules, priorities and fallback after service/board restart."""
import json
import os
import sys
import time

import board_startup as boot
import board_rules as rules

boot.graph.ENV['XDG_CONFIG_HOME'] = '/root/.config'
RULES = ['test_boot_advanced_high', 'test_boot_advanced_low']
PREFIX = boot.graph.PREFIX + 'advanced-boot-'
SOURCES = [PREFIX + 'source' + str(i) for i in range(2)]
PRIMARY, BACKUP, LOW = [PREFIX + n for n in ['primary', 'backup', 'low']]
NAMES = SOURCES + [PRIMARY, BACKUP, LOW]


def records():
    return {r['name']: r for r in rules.status()['rules'] if r['name'] in RULES}


def targets():
    return {l['input']['node_name'] for l in boot.graph.query('links')
            if l['output']['node_name'] in SOURCES}


def ready(target, winner):
    data = records()
    selected = data[RULES[winner]]
    return (targets() == {target} and selected['state'] == 'connected'
            and selected['connected_pairs'] == 4
            and data[RULES[1 - winner]]['state'] == ('suppressed' if winner == 0 else 'waiting'))


def prepare():
    assert rules.status()['engine_running'], 'Install/start the new audio supervisor first'
    assert not records()
    assert not any(n['name'] in NAMES for n in boot.graph.query()['nodes'])
    boot.prepare()
    try:
        output = PREFIX + 'source*:monitor_*'
        rules.command('create-route-rule', RULES[0], output, PRIMARY + ':playback_*',
                      '--match', 'glob', '--priority', '100', '--exclusive-group', 'test_boot_monitor',
                      '--fallback', BACKUP + ':playback_*', '--switch-delay', '1500')
        rules.command('create-route-rule', RULES[1], output, LOW + ':playback_*',
                      '--match', 'glob', '--priority', '10', '--exclusive-group', 'test_boot_monitor')
        rules.command('check-route-rules', env=dict(boot.graph.ENV, PIPEWIRE_REMOTE='absent'))
        os.sync()
    except BaseException:
        for name in RULES: rules.command('delete-route-rule', name, check=False)
        boot.cleanup()
        raise
    print('ADVANCED ROUTING STARTUP FIXTURE PREPARED', flush=True)


def verify(reboot=False):
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        result = boot.command('--json', 'list-route-rules', check=False)
        if result.returncode == 0 and json.loads(result.stdout)['engine_running']: break
        time.sleep(.25)
    else: raise AssertionError(open('/tmp/pipemixer-session.log').read())
    boot.verify(reboot)
    data = records()
    assert set(data) == set(RULES)
    for index, name in enumerate(RULES):
        assert data[name]['priority'] == [100, 10][index]
        assert data[name]['exclusive_group'] == 'test_boot_monitor'
        assert data[name]['match'] == 'glob'
        with open('/root/.config/pipemixer/rules/' + name + '.json') as file:
            assert json.load(file)['version'] == 2
    assert data[RULES[0]]['fallbacks'] == [BACKUP + ':playback_*']
    assert data[RULES[0]]['switch_delay_ms'] == 1500
    try:
        for name in SOURCES + [BACKUP, LOW]: boot.graph.create_sink(name)
        boot.graph.wait_for(lambda: ready(BACKUP, 0))
        assert records()[RULES[0]]['using_fallback']
        for name in SOURCES:
            rules.command('set-volume', name, '77'); rules.command('set-mute', name, 'off')
        boot.graph.create_sink(PRIMARY)
        time.sleep(.4); assert targets() == {BACKUP}
        boot.graph.wait_for(lambda: ready(PRIMARY, 0))
        assert not records()[RULES[0]]['using_fallback']
        boot.graph.remove_sink(PRIMARY); boot.graph.wait_for(lambda: ready(BACKUP, 0))
        boot.graph.remove_sink(BACKUP); boot.graph.wait_for(lambda: ready(LOW, 1))
        boot.graph.create_sink(BACKUP); boot.graph.wait_for(lambda: ready(BACKUP, 0))
        for name in SOURCES:
            channels = json.loads(rules.command('--json', 'get-volume', name).stdout)['channels']
            assert all(round(c['percent']) == 77 for c in channels)
        with open(boot.STATE) as file: saved = json.load(file)
        assert abs(boot.pcm() - saved['reference_rms']) < saved['reference_rms'] * .06
        print('PASS persisted batch rules, priority, ordered fallback and stable failback after %s; scene PCM retained' %
              ('board reboot' if reboot else 'audio-session restart'), flush=True)
    finally:
        for name in reversed(NAMES): boot.graph.remove_sink(name)
    boot.graph.wait_for(lambda: all(r['state'] == 'waiting' for r in records().values()))
    assert rules.status()['engine_running']
    print('ADVANCED ROUTING STARTUP PASSED', flush=True)


def cleanup():
    for name in RULES: rules.command('delete-route-rule', name)
    for name in reversed(NAMES): boot.graph.remove_sink(name)
    boot.cleanup()
    assert not records()
    print('ADVANCED ROUTING STARTUP FIXTURE CLEANED', flush=True)


if __name__ == '__main__':
    if sys.argv[1] == 'prepare': prepare()
    elif sys.argv[1] == 'verify': verify('--reboot' in sys.argv)
    elif sys.argv[1] == 'cleanup': cleanup()
    else: raise SystemExit('Use prepare, verify [--reboot], or cleanup')
