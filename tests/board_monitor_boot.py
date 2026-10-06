"""Persistent monitor policy, Solo/listen restoration plus prior startup goals."""
import json
import os
import subprocess
import sys
import time

import board_rule_advanced_boot as advanced
import board_startup as boot
import board_rules as rules

NAME = 'test_boot_monitor'
DESTINATION = boot.graph.PREFIX + 'monitor-boot-phones'
SOLO = 'pipemixer.bus.test_boot_mix.output'
LISTEN = 'pipemixer.bus.test_boot_out.output'
MONITOR = 'pipemixer.monitor.' + NAME


def record():
    return next((m for m in json.loads(boot.command('--json', 'list-monitors').stdout)['monitors'] if m['name'] == NAME), None)


def ready(source, mode):
    r = record()
    return r and r['state'] == 'connected' and r['mode'] == mode and r['active_sources'] == [source]


def pcm(source, mode):
    boot.graph.wait_for(lambda: ready(source, mode))
    tone = subprocess.Popen([boot.PROBE, 'play', boot.graph.PREFIX + 'monitor-boot-tone',
                             'pipemixer.bus.test_boot_mix.input', '1000'], env=boot.graph.ENV,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(.4)
        main, phones, direct = boot.audio.capture(LISTEN, DESTINATION, source, sink_targets=[DESTINATION])
        with open(boot.STATE) as file: expected = json.load(file)['reference_rms']
        assert abs(main - expected) < expected * .06, (main, expected)
        assert abs(phones - direct) < direct * .04, (phones, direct)
        return main, phones
    finally:
        tone.terminate(); tone.wait(timeout=3)


def prepare():
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        result = boot.command('--json', 'list-route-rules', check=False)
        if result.returncode == 0 and json.loads(result.stdout)['engine_running']: break
        time.sleep(.25)
    else: raise AssertionError('Audio supervisor did not start the routing engine')
    assert record() is None
    advanced.prepare()
    try:
        offline = dict(boot.graph.ENV, PIPEWIRE_REMOTE='pipemixer-no-server')
        boot.command('create-monitor', NAME, DESTINATION, SOLO, env=offline)
        boot.command('set-monitor-source', NAME, LISTEN, env=offline)
        boot.command('solo-monitor', NAME, SOLO, 'on', env=offline)
        boot.command('check-monitors', env=offline)
        boot.graph.wait_for(lambda: any(n['name'] == MONITOR + '.output' for n in boot.graph.query()['nodes']))
        boot.command('set-volume', MONITOR + '.output', '100'); boot.command('set-mute', MONITOR + '.output', 'off')
        os.sync()
        print('MONITOR STARTUP FIXTURE PREPARED', flush=True)
    except BaseException:
        boot.command('delete-monitor', NAME, check=False)
        boot.graph.wait_for(lambda: not any(n['name'].startswith(MONITOR) for n in boot.graph.query()['nodes']))
        advanced.cleanup()
        raise


def verify(reboot=False):
    advanced.verify(reboot)
    boot.graph.wait_for(lambda: record() and record()['state'] == 'waiting')
    r = record()
    assert r['enabled'] and r['sources'] == [SOLO] and r['solo'] == [SOLO] and r['listen'] == LISTEN
    assert r['mode'] == 'solo' and r['destination'] == DESTINATION
    try:
        boot.graph.create_sink(DESTINATION)
        boot.graph.wait_for(lambda: ready(SOLO, 'solo'))
        before = pcm(SOLO, 'solo')
        # The native engine also recreates its own audio worker if removed.
        node = next(n for n in boot.graph.query()['nodes'] if n['name'] == MONITOR + '.input')
        boot.graph.run('pw-cli', 'destroy', str(node['id']))
        boot.graph.wait_for(lambda: ready(SOLO, 'solo'))
        recreated = pcm(SOLO, 'solo')
        assert abs(before[1] - recreated[1]) < before[1] * .04
        boot.command('clear-monitor-solo', NAME)
        after = pcm(LISTEN, 'listen')
        assert abs(before[0] - after[0]) < before[0] * .025
        boot.command('set-monitor-source', NAME, 'mix'); mixed = pcm(SOLO, 'mix')
        assert abs(mixed[1] - before[1]) < before[1] * .04
        # Keep the identical fixture ready for the following full reboot.
        boot.command('set-monitor-source', NAME, LISTEN); boot.command('solo-monitor', NAME, SOLO, 'on')
        boot.graph.wait_for(lambda: ready(SOLO, 'solo')); os.sync()
        print('PASS %s restores monitor, normal mix, listening source and active Solo; late headphones reconnect; worker recreated; main PCM %.6f, Solo %.6f, restored listen %.6f' %
              ('board reboot' if reboot else 'audio-service restart', before[0], before[1], after[1]), flush=True)
    finally:
        boot.graph.remove_sink(DESTINATION)
    print('MONITOR STARTUP RESTORE PASSED', flush=True)


def cleanup():
    boot.command('delete-monitor', NAME)
    boot.graph.wait_for(lambda: not any(n['name'].startswith(MONITOR) for n in boot.graph.query()['nodes']))
    boot.graph.remove_sink(DESTINATION)
    advanced.cleanup()
    assert record() is None
    print('MONITOR STARTUP FIXTURE CLEANED', flush=True)


if __name__ == '__main__':
    if sys.argv[1] == 'prepare': prepare()
    elif sys.argv[1] == 'verify': verify('--reboot' in sys.argv)
    elif sys.argv[1] == 'cleanup': cleanup()
    else: raise SystemExit('Use prepare, verify [--reboot], or cleanup')
