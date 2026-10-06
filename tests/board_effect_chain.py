"""Structural effect edits, live bypass, scene round trips and real PCM."""
import json
import os
import shutil
import subprocess
import time

import board_graph as graph
from board_audio import capture, PROBE
from board_routing import Tui
from board_rules import start, stop

graph.ENV['XDG_CONFIG_HOME'] = '/tmp/board/chain-test-config'
NAME = 'test_chain'
BUSES = ['test_chain_source', 'test_chain_destination', 'test_chain_guard']
SEND = 'test_chain_send'


def command(*args, check=True):
    result = subprocess.run([graph.BINARY, *args], env=graph.ENV, capture_output=True, text=True, timeout=40)
    if check and result.returncode:
        raise AssertionError('%s: %s' % (args, result.stderr))
    return result


def stages(waiting=False):
    result = command('--json', 'effect-chain', NAME, check=not waiting)
    return json.loads(result.stdout) if result.returncode == 0 else None


def params():
    return {p['name']: p['value'] for p in json.loads(command('--json', 'effect-params', NAME).stdout)}


def volume(name):
    return json.loads(command('--json', 'get-volume', name).stdout)['channels']


def connect(source, destination):
    ports = graph.query('ports')
    for channel in ['FL', 'FR']:
        output = next(p for p in ports if p['node_name'] == source and p['direction'] == 'output' and p['channel'] == channel)
        inp = next(p for p in ports if p['node_name'] == destination and p['direction'] == 'input' and p['channel'] == channel)
        command('connect', source + ':' + output['name'], destination + ':' + inp['name'])


def main():
    ui = tone = engine = None
    before = {kind: command('get-default', kind).stdout for kind in ['sink', 'source']}
    try:
        command('clear-recovery')
        command('delete-send', SEND)
        command('delete-effect', NAME)
        for name in BUSES:
            command('delete-bus', name)
        engine = start()
        for name in BUSES:
            command('create-bus', name)
        command('create-effect', NAME, 'empty')
        command('create-effect', 'pmcheck_user_effect', 'empty')
        assert any(n['group'] == 'pmcheck_user_effect' for n in graph.query()['nodes'])
        command('delete-effect', 'pmcheck_user_effect')
        assert stages() == []
        command('add-effect-stage', NAME, 'gain')
        assert stages()[0]['id'] == 'gain1'
        command('set-effect-param', NAME, 'gain1:Mult', '.5')
        for kind, names in [('bus', BUSES), ('effect', [NAME])]:
            for name in names:
                for role in ['input', 'output']:
                    node = 'pipemixer.%s.%s.%s' % (kind, name, role)
                    command('set-volume', node, '100')
                    command('set-mute', node, 'off')
        source = 'pipemixer.bus.' + BUSES[0] + '.output'
        output = 'pipemixer.effect.' + NAME + '.output'
        connect(source, 'pipemixer.effect.' + NAME + '.input')
        command('create-send', SEND, output, 'pipemixer.bus.' + BUSES[1] + '.input')
        command('set-volume', 'pipemixer.send.' + SEND + '.output', '35')
        command('set-mute', 'pipemixer.send.' + SEND + '.output', 'off')
        command('set-volume', 'pipemixer.bus.' + BUSES[2] + '.output', '23')
        guard = command('--json', 'get-volume', 'pipemixer.bus.' + BUSES[2] + '.output').stdout
        send_gain = volume('pipemixer.send.' + SEND + '.output')
        tone = subprocess.Popen([PROBE, 'play', graph.PREFIX + 'chain-tone',
                                 'pipemixer.bus.' + BUSES[0] + '.input', '440'], env=graph.ENV,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        graph.wait_for(lambda: any(n['name'] == graph.PREFIX + 'chain-tone' for n in graph.query()['nodes']))
        half = capture(output)[0]
        assert .040 < half < .049, half
        ids = {n['id'] for n in graph.query()['nodes'] if n['kind'] == 'effect' and n['group'] == NAME}
        command('bypass-effect-stage', NAME, 'gain1', 'on')
        unity = capture(output)[0]
        assert .08 < unity < .10, unity
        assert {n['id'] for n in graph.query()['nodes'] if n['kind'] == 'effect' and n['group'] == NAME} == ids
        command('bypass-effect-stage', NAME, 'gain1', 'off')
        print('PASS empty chain, adding gain and live per-stage bypass: %.6f -> %.6f' % (half, unity), flush=True)

        command('add-effect-stage', NAME, 'gate')
        assert params()['gain1:Mult'] == .5
        assert volume('pipemixer.send.' + SEND + '.output') == send_gain
        command('set-effect-param', NAME, 'gate1:Open Threshold', '.08')
        command('set-effect-param', NAME, 'gate1:Close Threshold', '.07')
        closed = capture(output)[0]
        assert closed < .0001, closed
        command('move-effect-stage', NAME, 'gate1', '1')
        opened = capture(output)[0]
        assert .040 < opened < .049, opened
        assert command('--json', 'get-volume', 'pipemixer.bus.' + BUSES[2] + '.output').stdout == guard
        assert volume('pipemixer.send.' + SEND + '.output') == send_gain
        destination = capture('pipemixer.bus.' + BUSES[1] + '.output')[0]
        assert .0015 < destination < .0024, destination
        print('PASS processor order affects PCM; manual stereo links and Send gain survive: %.6f -> %.6f, send %.6f' %
              (closed, opened, destination), flush=True)

        command('save-scene', 'chain_a')
        command('load-scene', 'chain_a')
        command('set-effect-param', NAME, 'gain1:Mult', '.25')
        command('move-effect-stage', NAME, 'gain1', '1')
        time.sleep(1)
        assert params()['gain1:Mult'] == .25, params()
        assert capture(output)[0] < .0001
        command('save-scene', 'chain_b')
        command('load-scene', 'chain_a')
        assert [s['id'] for s in stages()] == ['gate1', 'gain1']
        assert params()['gain1:Mult'] == .5
        assert abs(capture(output)[0] - half) < .003
        command('load-scene', 'chain_b')
        assert [s['id'] for s in stages()] == ['gain1', 'gate1']
        assert params()['gain1:Mult'] == .25
        command('remove-effect-stage', NAME, 'gate1')
        assert [s['id'] for s in stages()] == ['gain1']
        assert abs(capture(output)[0] - half / 2) < .003
        print('PASS changed chains load over existing chains; active recovery preserves manual edits', flush=True)

        for args in [('add-effect-stage', NAME, 'missing'), ('add-effect-stage', NAME, 'gain', '9'),
                     ('remove-effect-stage', NAME, 'missing'), ('move-effect-stage', NAME, 'gain1', '9')]:
            assert command(*args, check=False).returncode == 3
        assert len(stages()) == 1
        ui = Tui()
        ui.send(b'5'); ui.send(b'e')
        ordered = [n for n in sorted(graph.query()['nodes'], key=lambda n: n['id']) if n['kind'] == 'effect' and n['role'] == 'input']
        index = 3 + next(i for i, n in enumerate(ordered) if n['group'] == NAME)
        assert b'Chain ' in ui.send(b'j' * index + b'\n')
        ui.send(b'\n'); ui.send(b'l')
        graph.wait_for(lambda: params()['gain1:Mult'] > .25)
        ui.send(b'\n')
        graph.wait_for(lambda: params()['gain1:Mult'] == 1)
        ui.send(b'\x1b'); ui.send(b'a')
        ui.send(b'jjjjj\n')  # Gain is the sixth processor.

        def wait_edit():
            output = b''
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                output += ui.drain(.2)
                if b'connections restored' in output:
                    return
            raise AssertionError('TUI edit did not finish: ' + repr(output[-1000:]))

        wait_edit()
        assert len(stages()) == 2
        ui.send(b'K')
        wait_edit()
        assert stages()[0]['id'] == 'gain2'
        ui.send(b' ')
        graph.wait_for(lambda: stages()[0]['bypass'])
        ui.send(b'B')
        graph.wait_for(lambda: params()['wet:Mult'] == 0 and params()['dry:Mult'] == 1)
        ui.send(b'd'); ui.send(b'j\n')
        wait_edit()
        assert len(stages()) == 1
        ui.resize(8, 30); ui.resize(24, 100)
        command('delete-effect', NAME)
        ui.drain(.3); ui.send(b'\x1b'); ui.close(); ui = None
        command('delete-send', SEND)
        command('create-effect', NAME, 'empty')
        ui = Tui(); ui.send(b'5'); ui.send(b'e')
        ordered = [n for n in sorted(graph.query()['nodes'], key=lambda n: n['id']) if n['kind'] == 'effect' and n['role'] == 'input']
        index = 3 + next(i for i, n in enumerate(ordered) if n['group'] == NAME)
        ui.send(b'j' * index + b'\n'); ui.send(b'a'); ui.send(b'jjjjj\n')
        ui.process.terminate()
        ui.process.wait(timeout=15)
        assert ui.process.returncode == 0
        os.close(ui.master); ui.log.close(); ui = None
        assert len(stages()) == 1
        print('PASS TUI signal exit completes an in-progress structural edit', flush=True)
        assert {kind: command('get-default', kind).stdout for kind in before} == before
        print('PASS TUI add/reorder/remove, parameter edit/reset, stage/global bypass, resizing and external removal', flush=True)
    finally:
        if ui:
            try:
                ui.close()
            except AssertionError:
                pass  # Preserve the original failure and still clean fixtures.
        if tone:
            tone.terminate(); tone.wait(timeout=3)
        if engine:
            stop(engine)
        command('clear-recovery', check=False)
        command('delete-send', SEND, check=False)
        command('delete-effect', NAME, check=False)
        command('delete-effect', 'pmcheck_user_effect', check=False)
        for name in BUSES:
            command('delete-bus', name, check=False)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'], ignore_errors=True)
    graph.reference_check()
    print('EFFECT CHAIN STAGE PASSED', flush=True)


if __name__ == '__main__':
    main()
