"""Selective scene replay for late/recreated devices; real graph and PCM."""
import json
import os
import shutil
import subprocess
import time

import board_graph as graph
import board_audio as audio
import board_rules as rules

CONFIG = '/tmp/board/recovery-test-config'
graph.ENV['XDG_CONFIG_HOME'] = CONFIG
SCENE = 'test_recovery'
EXTERNAL = [graph.PREFIX + 'recover-' + c for c in ['a', 'b']]
BUS = 'test_recover_guard'
EFFECT = 'test_recover_eq'
SEND = 'test_recover_send'
GUARD = 'pipemixer.bus.' + BUS
EQ = 'pipemixer.effect.' + EFFECT
SENDING = 'pipemixer.send.' + SEND
STREAM = graph.PREFIX + 'recover-application'


def command(*args, check=True):
    result = subprocess.run([graph.BINARY, *args], env=graph.ENV, capture_output=True, text=True, timeout=40)
    if check and result.returncode:
        raise AssertionError('%s: %s' % (args, result.stderr))
    return result


def status():
    return json.loads(command('--json', 'recovery-status').stdout)


def restored(name):
    return next((n['state'] == 'restored' for n in status()['nodes'] if n['name'] == name), False)


def volumes(name):
    return [round(c['percent']) for c in json.loads(command('--json', 'get-volume', name).stdout)['channels']]


def gain():
    return next(p['value'] for p in json.loads(command('--json', 'effect-params', EFFECT).stdout) if p['name'] == 'mid:Gain')


def stream_channels_ready():
    result = command('--json', 'get-volume', STREAM, check=False)
    if result.returncode == 3: return False  # Adapter/node layout may still be changing.
    if result.returncode: raise AssertionError(result.stderr)
    return len(json.loads(result.stdout)['channels']) == 2


def connect(source, destination):
    ports = graph.query('ports')
    for c in ['FL', 'FR']:
        out = next(p for p in ports if p['node_name'] == source and p['direction'] == 'output' and p['channel'] == c)
        inp = next(p for p in ports if p['node_name'] == destination and p['direction'] == 'input' and p['channel'] == c)
        command('connect', source + ':' + out['name'], destination + ':' + inp['name'])


def pcm():
    tone = subprocess.Popen([audio.PROBE, 'play', graph.PREFIX + 'recover-tone', EXTERNAL[0], '1000'],
                            env=graph.ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(.4)
        return audio.capture(GUARD + '.output')[0]
    finally:
        tone.terminate(); tone.wait(timeout=3)


def wait_links():
    def ready():
        g = graph.query()
        return restored(EXTERNAL[0]) and any(n['name'] == SENDING + '.output' for n in g['nodes']) and len([
            l for l in g['links'] if l['input']['node_name'] == SENDING + '.input'
            or l['output']['node_name'] == SENDING + '.output']) == 4
    try:
        graph.wait_for(ready)
    except AssertionError:
        print('RECOVERY STATUS', status(), flush=True)
        print('RECOVERY NODES', graph.query()['nodes'], flush=True)
        print('RECOVERY LEVELS', command('--json', 'get-volume', EXTERNAL[0], check=False).stdout,
              command('get-mute', EXTERNAL[0], check=False).stdout, flush=True)
        print('ENGINE LOG', open('/tmp/board/rules-engine.log').read(), flush=True)
        raise


def main():
    shutil.rmtree(CONFIG, ignore_errors=True)
    before = graph.query()
    defaults = {k: command('get-default', k).stdout.strip() for k in ['sink', 'source']}
    engine = tone = None
    try:
        for name in EXTERNAL: graph.create_sink(name)
        command('create-bus', BUS); command('create-effect', EFFECT, 'eq')
        command('create-send', SEND, EXTERNAL[0], EQ + '.input')
        connect(EQ + '.output', GUARD + '.input')
        time.sleep(.6)
        for param in json.loads(command('--json', 'effect-params', EFFECT).stdout):
            if param['writable']: command('set-effect-param', EFFECT, param['name'], str(param['default']))
        for n in graph.query()['nodes']:
            if n['group'] in [BUS, EFFECT, SEND]:
                command('set-volume', n['name'], '100'); command('set-mute', n['name'], 'off')
        command('set-volume', EXTERNAL[0], '53', 'FL'); command('set-volume', EXTERNAL[0], '81', 'FR')
        command('set-mute', EXTERNAL[0], 'off'); command('set-volume', EXTERNAL[1], '42'); command('set-mute', EXTERNAL[1], 'on')
        command('set-volume', SENDING + '.output', '76'); command('set-volume', GUARD + '.output', '70')
        command('set-effect-param', EFFECT, 'mid:Gain', '6')
        command('set-default', EXTERNAL[1])
        command('save-scene', SCENE); command('set-startup-scene', SCENE)
        saved_path = CONFIG + '/pipemixer/scenes/' + SCENE + '.json'
        original = open(saved_path, 'rb').read()
        serials = {o['info']['props']['node.name']: o['info']['props']['object.serial'] for o in graph.dump()
                   if o['type'].endswith(':Node') and o['info']['props'].get('node.name') in EXTERNAL}
        for name in EXTERNAL: graph.remove_sink(name)
        command('delete-send', SEND)
        result = command('restore-startup'); assert 'skipped' in result.stderr, result.stderr
        command('set-volume', GUARD + '.output', '57'); command('set-effect-param', EFFECT, 'mid:Gain', '12')
        engine = rules.start(); graph.wait_for(lambda: status()['engine_running'])
        assert status()['scene'] == SCENE and not restored(EXTERNAL[0])
        graph.create_sink(EXTERNAL[0]); wait_links()
        assert volumes(EXTERNAL[0]) == [53, 81] and volumes(SENDING + '.output') == [76, 76]
        assert volumes(GUARD + '.output') == [57, 57] and gain() == 12
        assert not restored(EXTERNAL[1])
        measured = pcm(); assert measured > .0005, measured
        graph.create_sink(EXTERNAL[1]); graph.wait_for(lambda: restored(EXTERNAL[1]))
        assert volumes(EXTERNAL[1]) == [42, 42] and command('get-mute', EXTERNAL[1]).stdout.strip() == 'on'
        graph.wait_for(lambda: command('get-default', 'sink').stdout.strip() == EXTERNAL[1])
        assert open(saved_path, 'rb').read() == original
        print('PASS late devices replay separate channel gain/mute/default and skipped send, retain live bus/EQ; PCM %.6f' % measured, flush=True)

        command('set-volume', EXTERNAL[0], '23'); command('set-mute', EXTERNAL[0], 'on')
        command('set-volume', SENDING + '.output', '65')
        time.sleep(1.2)
        assert volumes(EXTERNAL[0]) == [23, 23] and command('get-mute', EXTERNAL[0]).stdout.strip() == 'on'
        rules.stop(engine); engine = rules.start(); graph.wait_for(lambda: status()['engine_running'])
        time.sleep(.8)
        assert volumes(EXTERNAL[0]) == [23, 23] and volumes(SENDING + '.output') == [65, 65]
        assert volumes(GUARD + '.output') == [57, 57] and gain() == 12
        print('PASS engine restart preserves completed generations and live adjustments', flush=True)
        for cycle in range(2):
            graph.remove_sink(EXTERNAL[0]); graph.create_sink(EXTERNAL[0]); wait_links()
            assert volumes(EXTERNAL[0]) == [53, 81] and command('get-mute', EXTERNAL[0]).stdout.strip() == 'off'
            assert volumes(SENDING + '.output') == [76, 76] and volumes(GUARD + '.output') == [57, 57] and gain() == 12
        now = next(o['info']['props']['object.serial'] for o in graph.dump()
                   if o['type'].endswith(':Node') and o['info']['props'].get('node.name') == EXTERNAL[0])
        assert now != serials[EXTERNAL[0]]
        again = pcm(); assert abs(again - measured) < measured * .06, (again, measured)
        print('PASS repeated endpoint generation changes restore only reappearing node and links; PCM %.6f' % again, flush=True)

        def start_stream(target):
            with open('/tmp/board/recovery-stream.log', 'a') as log:
                process = subprocess.Popen([audio.PROBE, 'play-reconnect', STREAM, target, '440'], env=graph.ENV,
                                           stdout=subprocess.DEVNULL, stderr=log)
            try:
                graph.wait_for(lambda: any(n['name'] == STREAM for n in graph.query()['nodes']))
                if not restored(STREAM): graph.wait_for(lambda: stream_at(target))
                graph.wait_for(stream_channels_ready)
            except BaseException:
                print('STREAM PROCESS', process.poll(), 'NODES', graph.query()['nodes'], flush=True)
                process.terminate(); process.wait(timeout=3)
                print(open('/tmp/board/recovery-stream.log').read()[-4096:], flush=True)
                raise
            return process

        def stream_at(target):
            links = [l for l in graph.query('links') if l['output']['node_name'] == STREAM]
            return len(links) == 2 and all(l['input']['node_name'] == target for l in links)

        tone = start_stream(EXTERNAL[1]); command('set-target', STREAM, EXTERNAL[1])
        graph.wait_for(stream_channels_ready)
        command('set-volume', STREAM, '35', 'FL'); command('set-volume', STREAM, '55', 'FR')
        command('save-scene', SCENE); command('load-scene', SCENE)
        command('set-volume', STREAM, '29'); command('set-target', STREAM, GUARD + '.input')
        graph.wait_for(lambda: stream_at(GUARD + '.input'))
        graph.remove_sink(EXTERNAL[1]); graph.create_sink(EXTERNAL[1])
        graph.wait_for(lambda: stream_at(EXTERNAL[1]))
        assert volumes(STREAM) == [29, 29], volumes(STREAM)
        tone.terminate(); tone.wait(timeout=3); tone = None
        graph.wait_for(lambda: not any(n['name'] == STREAM for n in graph.query()['nodes']))
        tone = start_stream(GUARD + '.input')
        graph.wait_for(lambda: restored(STREAM) and stream_at(EXTERNAL[1]))
        assert volumes(STREAM) == [35, 55], volumes(STREAM)
        tone.terminate(); tone.wait(timeout=3); tone = None
        print('PASS returning stream replays channel gains/target; returning target alone preserves live stream gain', flush=True)

        # A deliberate manual disconnect on unchanged generations stays disconnected.
        command('disconnect', EQ + '.output:capture_FL', GUARD + '.input:playback_FL')
        time.sleep(1.2)
        assert not rules.between(EQ + '.output:capture_FL', GUARD + '.input:playback_FL')
        command('clear-recovery'); graph.wait_for(lambda: status()['scene'] is None)
        graph.remove_sink(EXTERNAL[0]); graph.create_sink(EXTERNAL[0]); time.sleep(1)
        assert not rules.between(EXTERNAL[0] + ':monitor_FL', SENDING + '.input:playback_FL')
        print('PASS manual disconnect retained, explicit clear disables replay', flush=True)
    finally:
        if tone:
            tone.terminate(); tone.wait(timeout=3)
        command('clear-recovery', check=False); rules.stop(engine)
        command('delete-send', SEND, check=False); command('delete-effect', EFFECT, check=False); command('delete-bus', BUS, check=False)
        for name in EXTERNAL: graph.remove_sink(name)
        for name in defaults.values():
            if name: command('set-default', name)
        command('delete-scene', SCENE, check=False)
        shutil.rmtree(CONFIG, ignore_errors=True)
        assert sorted(n['name'] for n in graph.query()['nodes']) == sorted(n['name'] for n in before['nodes'])
        graph.reference_check()
    print('SELECTIVE RECOVERY TESTS PASSED', flush=True)


if __name__ == '__main__': main()
