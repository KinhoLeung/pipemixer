"""Compressor controls, captured PCM, chain persistence and TUI on the board."""
import json
import shutil
import subprocess
import time

import board_graph as graph
from board_audio import capture, PROBE
from board_effect_chain import connect
from board_routing import Tui

graph.ENV['XDG_CONFIG_HOME'] = '/tmp/board/compressor-test-config'
NAME = 'test_compressor'
SOURCE = 'test_compressor_source'


def command(*args, check=True):
    result = subprocess.run([graph.BINARY, *args], env=graph.ENV, capture_output=True, text=True, timeout=40)
    if check and result.returncode:
        raise AssertionError('%s: %s' % (args, result.stderr))
    return result


def params():
    return {p['name']: p['value'] for p in json.loads(command('--json', 'effect-params', NAME).stdout)}


def set_param(name, value):
    command('set-effect-param', NAME, 'compressor1:' + name, str(value))


def main():
    tone = ui = None
    try:
        command('create-bus', SOURCE)
        command('create-effect', NAME, 'empty')
        command('add-effect-stage', NAME, 'compressor')
        for kind, name in [('bus', SOURCE), ('effect', NAME)]:
            for role in ['input', 'output']:
                command('set-volume', 'pipemixer.%s.%s.%s' % (kind, name, role), '100')
                command('set-mute', 'pipemixer.%s.%s.%s' % (kind, name, role), 'off')
        connect('pipemixer.bus.' + SOURCE + '.output', 'pipemixer.effect.' + NAME + '.input')
        tone = subprocess.Popen([PROBE, 'play', graph.PREFIX + 'compressor-tone', 'pipemixer.bus.' + SOURCE + '.input', '1000'],
                                env=graph.ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        graph.wait_for(lambda: any(n['name'] == graph.PREFIX + 'compressor-tone' for n in graph.query()['nodes']))
        set_param('Threshold dB', 0)
        flat = capture('pipemixer.effect.' + NAME + '.output')[0]
        assert .08 < flat < .10, flat
        ids = {n['id'] for n in graph.query()['nodes'] if n['group'] == NAME}
        for name, value in [('Threshold dB', -30), ('Ratio', 4), ('Attack ms', 1), ('Release ms', 100), ('Knee dB', 0)]:
            set_param(name, value)
        compressed = capture('pipemixer.effect.' + NAME + '.output')[0]
        assert .33 < compressed / flat < .39, (flat, compressed)
        set_param('Makeup dB', 6)
        boosted = capture('pipemixer.effect.' + NAME + '.output')[0]
        assert 1.97 < boosted / compressed < 2.02, (compressed, boosted)
        set_param('Makeup dB', 0)
        set_param('Mix', 0)
        assert abs(capture('pipemixer.effect.' + NAME + '.output')[0] - flat) < .002
        set_param('Mix', 1)
        command('bypass-effect-stage', NAME, 'compressor1', 'on')
        assert abs(capture('pipemixer.effect.' + NAME + '.output')[0] - flat) < .002
        command('bypass-effect-stage', NAME, 'compressor1', 'off')
        set_param('Ratio', 1)
        assert abs(capture('pipemixer.effect.' + NAME + '.output')[0] - flat) < .002
        set_param('Ratio', 4)
        assert ids == {n['id'] for n in graph.query()['nodes'] if n['group'] == NAME}
        print('PASS live compressor, ratio 1, makeup, parallel mix and stage bypass without node recreation: %.6f -> %.6f -> %.6f' %
              (flat, compressed, boosted), flush=True)
        for name, value in [('Threshold dB', 1), ('Ratio', .5), ('Attack ms', 0), ('Release ms', -1), ('Mix', 2)]:
            assert command('set-effect-param', NAME, 'compressor1:' + name, str(value), check=False).returncode == 3
        saved = params()
        command('save-scene', 'compressor')
        command('delete-effect', NAME)
        command('load-scene', 'compressor')
        assert params() == saved
        assert abs(capture('pipemixer.effect.' + NAME + '.output')[0] - compressed) < .002
        print('PASS compressor scene recreation restores parameters and real PCM', flush=True)

        ui = Tui(); ui.send(b'5'); ui.send(b'e')
        ordered = [n for n in sorted(graph.query()['nodes'], key=lambda n: n['id']) if n['kind'] == 'effect' and n['role'] == 'input']
        index = 3 + next(i for i, n in enumerate(ordered) if n['group'] == NAME)
        ui.send(b'j' * index + b'\n'); ui.send(b'\n'); ui.send(b'h')
        graph.wait_for(lambda: params()['compressor1:Threshold dB'] == -31)
        ui.send(b'\n')
        graph.wait_for(lambda: params()['compressor1:Threshold dB'] == -18)
        ui.send(b' ')
        graph.wait_for(lambda: json.loads(command('--json', 'effect-chain', NAME).stdout)[0]['bypass'])
        ui.send(b'\x1b'); ui.send(b'\x1b'); ui.close(); ui = None
        print('PASS TUI compressor parameter editing/reset and independent bypass', flush=True)
    finally:
        if ui:
            ui.close()
        if tone:
            tone.terminate(); tone.wait(timeout=3)
        command('clear-recovery', check=False)
        command('delete-effect', NAME, check=False)
        command('delete-bus', SOURCE, check=False)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'], ignore_errors=True)
    graph.reference_check()
    print('COMPRESSOR STAGE PASSED', flush=True)


if __name__ == '__main__':
    main()
