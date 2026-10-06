"""Sample ceilings, dynamics chains, persistence and TUI with captured PCM."""
import array
import json
import math
import shutil
import subprocess

import board_graph as graph
from board_audio import PROBE
from board_effect_chain import connect
from board_routing import Tui

graph.ENV['XDG_CONFIG_HOME'] = '/tmp/board/limiter-test-config'
NAME = 'test_limiter'
SOURCE = 'test_limiter_source'


def command(*args, check=True):
    result = subprocess.run([graph.BINARY, *args], env=graph.ENV, capture_output=True, text=True, timeout=40)
    if check and result.returncode:
        raise AssertionError('%s: %s' % (args, result.stderr))
    return result


def stats():
    path = '/tmp/board/limiter-pcm.raw'
    result = subprocess.run([PROBE, 'capture', 'pipemixer.effect.' + NAME + '.output', path], env=graph.ENV,
                            capture_output=True, timeout=12)
    assert result.returncode == 0, result.stderr
    pcm = array.array('f')
    with open(path, 'rb') as file:
        pcm.frombytes(file.read())
    assert len(pcm) == 192000
    steady = pcm[48000:]
    assert all(math.isfinite(x) for x in steady)
    return max(abs(x) for x in steady), math.sqrt(sum(x*x for x in steady) / len(steady))


def params():
    return {p['name']: p['value'] for p in json.loads(command('--json', 'effect-params', NAME).stdout)}


def main():
    tone = ui = None
    try:
        command('create-bus', SOURCE)
        command('create-effect', NAME, 'empty')
        command('add-effect-stage', NAME, 'gain')
        command('add-effect-stage', NAME, 'limiter')
        for kind, name in [('bus', SOURCE), ('effect', NAME)]:
            for role in ['input', 'output']:
                command('set-volume', 'pipemixer.%s.%s.%s' % (kind, name, role), '100')
                command('set-mute', 'pipemixer.%s.%s.%s' % (kind, name, role), 'off')
        connect('pipemixer.bus.' + SOURCE + '.output', 'pipemixer.effect.' + NAME + '.input')
        tone = subprocess.Popen([PROBE, 'play', graph.PREFIX + 'limiter-tone', 'pipemixer.bus.' + SOURCE + '.input', '1000'],
                                env=graph.ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        graph.wait_for(lambda: any(n['name'] == graph.PREFIX + 'limiter-tone' for n in graph.query()['nodes']))
        command('set-effect-param', NAME, 'gain1:Mult', '4')
        ids = {n['id'] for n in graph.query()['nodes'] if n['group'] == NAME}
        measurements = []
        for ceiling, boost in [(-12, 0), (-24, 24), (-6, 12)]:
            command('set-effect-param', NAME, 'limiter1:Ceiling dB', str(ceiling))
            command('set-effect-param', NAME, 'limiter1:Input dB', str(boost))
            peak, rms = stats()
            limit = 10 ** (ceiling / 20)
            assert peak <= limit + .000005 and rms > limit * .65, (ceiling, boost, peak, rms)
            measurements.append((ceiling, peak, rms))
        command('set-effect-param', NAME, 'limiter1:Ceiling dB', '-12')
        command('set-effect-param', NAME, 'limiter1:Input dB', '0')
        command('bypass-effect-stage', NAME, 'limiter1', 'on')
        bypass_peak, bypass_rms = stats()
        assert .49 < bypass_peak < .51 and .34 < bypass_rms < .36
        command('bypass-effect-stage', NAME, 'limiter1', 'off')
        assert ids == {n['id'] for n in graph.query()['nodes'] if n['group'] == NAME}
        print('PASS live limiter ceilings under boosted audio, and independent bypass: %s, bypass peak %.6f' % (measurements, bypass_peak), flush=True)
        for parameter, value in [('Ceiling dB', 1), ('Release ms', 0), ('Input dB', -1)]:
            assert command('set-effect-param', NAME, 'limiter1:' + parameter, str(value), check=False).returncode == 3

        command('add-effect-stage', NAME, 'compressor', '2')
        for parameter, value in [('Threshold dB', -24), ('Ratio', 4), ('Attack ms', 1), ('Release ms', 100), ('Knee dB', 0)]:
            command('set-effect-param', NAME, 'compressor1:' + parameter, str(value))
        combined_peak, combined_rms = stats()
        assert combined_peak < .13 and .065 < combined_rms < .09, (combined_peak, combined_rms)
        saved = params()
        command('save-scene', 'dynamics')
        command('set-startup-scene', 'dynamics')
        command('delete-effect', NAME)
        command('restore-startup')
        assert params() == saved
        assert [s['id'] for s in json.loads(command('--json', 'effect-chain', NAME).stdout)] == ['gain1', 'compressor1', 'limiter1']
        restored_peak, restored_rms = stats()
        assert abs(restored_rms - combined_rms) < .002
        print('PASS combined gain/compressor/limiter chain and startup restoration: peak %.6f RMS %.6f -> %.6f' %
              (combined_peak, combined_rms, restored_rms), flush=True)

        ui = Tui(); ui.send(b'5'); ui.send(b'e')
        ordered = [n for n in sorted(graph.query()['nodes'], key=lambda n: n['id']) if n['kind'] == 'effect' and n['role'] == 'input']
        index = 3 + next(i for i, n in enumerate(ordered) if n['group'] == NAME)
        ui.send(b'j' * index + b'\n'); ui.send(b'jj\n'); ui.send(b'h')
        graph.wait_for(lambda: params()['limiter1:Ceiling dB'] == -12.5)
        ui.send(b'\n')
        graph.wait_for(lambda: params()['limiter1:Ceiling dB'] == -1)
        ui.send(b' ')
        graph.wait_for(lambda: json.loads(command('--json', 'effect-chain', NAME).stdout)[2]['bypass'])
        ui.resize(8, 30); ui.resize(24, 100)
        ui.send(b'\x1b'); ui.send(b'\x1b'); ui.close(); ui = None
        print('PASS TUI limiter ceiling editing/reset, independent bypass and resizing', flush=True)
    finally:
        if ui:
            ui.close()
        if tone:
            tone.terminate(); tone.wait(timeout=3)
        command('clear-recovery', check=False)
        command('delete-effect', NAME, check=False)
        command('delete-bus', SOURCE, check=False)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'], ignore_errors=True)
        from pathlib import Path
        Path('/tmp/board/limiter-pcm.raw').unlink(missing_ok=True)
    graph.reference_check()
    print('LIMITER STAGE PASSED', flush=True)


if __name__ == '__main__':
    main()
