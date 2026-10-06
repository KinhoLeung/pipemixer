"""Real PCM tests for virtual buses and independent sends on the board."""
import array
import math
import os
import subprocess
import time

import board_graph as graph
from board_routing import Tui

PROBE = '/tmp/board/audio-probe'


def capture(*targets, sink_targets=()):
    processes = []
    paths = []
    try:
        for index, target in enumerate(targets):
            path = '/tmp/board/pcm-%d.raw' % index
            paths.append(path)
            mode = 'capture-sink' if target in sink_targets else 'capture'
            processes.append(subprocess.Popen([PROBE, mode, target, path], env=graph.ENV,
                                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE))
        for process in processes:
            _, error = process.communicate(timeout=12)
            assert process.returncode == 0, error
        results = []
        for path in paths:
            pcm = array.array('f')
            with open(path, 'rb') as file:
                pcm.frombytes(file.read())
            assert len(pcm) == 192000
            steady = pcm[48000:]
            assert all(math.isfinite(x) for x in steady)
            results.append(math.sqrt(sum(x * x for x in steady) / len(steady)))
        return results
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)
        for path in paths:
            if os.path.exists(path):
                os.unlink(path)


def main():
    names = ['test_mix', 'test_branch1', 'test_branch2']
    sends = ['test_send1', 'test_send2']
    tones = []
    ui = None
    created = None
    before = graph.run(graph.BINARY, 'get-default', 'sink').stdout
    try:
        concurrent = [subprocess.Popen([graph.BINARY, 'create-bus', 'test_race'], env=graph.ENV,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(2)]
        for process in concurrent:
            process.communicate(timeout=12)
        assert sum(p.returncode == 0 for p in concurrent) == 1, [p.returncode for p in concurrent]
        assert len([n for n in graph.query()['nodes'] if n['kind'] == 'bus' and n['group'] == 'test_race']) == 2
        graph.run(graph.BINARY, 'delete-bus', 'test_race')
        print('PASS concurrent creation cannot duplicate the same bus', flush=True)
        for name in names:
            graph.run(graph.BINARY, 'create-bus', name)
        assert {b['group'] for b in graph.query('buses')} >= set(names)
        assert graph.run(graph.BINARY, 'create-bus', names[0], check=False).returncode == 3
        assert graph.run(graph.BINARY, 'create-bus', '../invalid', check=False).returncode == 2
        for index, frequency in enumerate([440, 880]):
            tone = subprocess.Popen([PROBE, 'play', graph.PREFIX + 'tone%d' % index,
                                     'pipemixer.bus.test_mix.input', str(frequency)], env=graph.ENV,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            tones.append(tone)
            graph.wait_for(lambda: any(n['name'] == graph.PREFIX + 'tone%d' % index for n in graph.query()['nodes']))
            time.sleep(.3)
            if index == 0:
                single = capture('pipemixer.bus.test_mix.output')[0]
                assert .07 < single < .10, single
        mixed = capture('pipemixer.bus.test_mix.output')[0]
        assert mixed > single * 1.3 and mixed < single * 1.55, (single, mixed)
        print('PASS actual stereo mixing RMS: one=%.5f two=%.5f' % (single, mixed), flush=True)
        for index, name in enumerate(sends):
            graph.run(graph.BINARY, 'create-send', name, 'pipemixer.bus.test_mix.output',
                      'pipemixer.bus.' + names[index + 1] + '.input')
        graph.wait_for(lambda: len([l for l in graph.query('links')
                                   if 'pipemixer.send.' in (l['output']['node_name'] or '')]) >= 4)
        # WirePlumber can restore the previous test run's gain/mute by node name.
        for name in sends:
            graph.run(graph.BINARY, 'set-volume', 'pipemixer.send.' + name + '.output', '100')
            graph.run(graph.BINARY, 'set-mute', 'pipemixer.send.' + name + '.output', 'off')
        baseline = capture('pipemixer.bus.test_branch1.output', 'pipemixer.bus.test_branch2.output')
        assert all(abs(level - mixed) < .015 for level in baseline), baseline
        graph.run(graph.BINARY, 'set-volume', 'pipemixer.send.test_send1.output', '50')
        attenuated = capture('pipemixer.bus.test_branch1.output', 'pipemixer.bus.test_branch2.output')
        assert attenuated[0] < baseline[0] * .3, attenuated
        assert abs(attenuated[1] - baseline[1]) < .01, attenuated
        graph.run(graph.BINARY, 'set-mute', 'pipemixer.send.test_send1.output', 'on')
        muted = capture('pipemixer.bus.test_branch1.output', 'pipemixer.bus.test_branch2.output')
        assert muted[0] < .0001 and abs(muted[1] - baseline[1]) < .01, muted
        print('PASS independent path gain and mute: baseline=%s attenuated=%s muted=%s' %
              (baseline, attenuated, muted), flush=True)
        assert graph.run(graph.BINARY, 'create-send', 'test_cycle', 'pipemixer.bus.test_branch2.output',
                         'pipemixer.bus.test_mix.input', check=False).returncode == 1
        print('PASS feedback detection includes internal bus/send paths', flush=True)

        existing = {b['group'] for b in graph.query('buses')}
        ui = Tui()
        assert b'Audio buses' in ui.send(b'b')
        ui.send(b'\n')
        created = graph.wait_for(lambda: next((b['group'] for b in graph.query('buses') if b['group'] not in existing), None))
        # Nodes appear before asynchronous creation is acknowledged by the TUI.
        # The scene menu is available only after the pending operation finishes.
        graph.wait_for(lambda: b'Scenes' in ui.send(b's'))
        ui.send(b'\x1b')
        ui.close(); ui = None
        assert any(b['group'] == created for b in graph.query('buses'))
        graph.run(graph.BINARY, 'delete-bus', created)
        graph.run(graph.BINARY, 'delete-bus', created)
        created = None
        print('PASS TUI creation and audio path survives TUI exit', flush=True)
        assert graph.run(graph.BINARY, 'get-default', 'sink').stdout == before
    finally:
        if ui:
            ui.close()
        for tone in tones:
            tone.terminate(); tone.wait(timeout=3)
        for name in sends + ['test_cycle']:
            graph.run(graph.BINARY, 'delete-send', name)
        for name in reversed(names + ['test_race'] + ([created] if created else [])):
            graph.run(graph.BINARY, 'delete-bus', name)
    graph.reference_check()
    print('STAGE 3 PASSED', flush=True)


if __name__ == '__main__':
    main()
