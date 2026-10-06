"""Exercise live DSP parameters, bypass and custom graphs with captured PCM."""
import json
import os
import subprocess

import board_graph as graph
from board_audio import PROBE, capture
from board_routing import Tui


def params(name):
    return {p['name']: p for p in json.loads(graph.run(graph.BINARY, '--json', 'effect-params', name).stdout)}


def set_param(name, parameter, value):
    graph.run(graph.BINARY, 'set-effect-param', name, parameter, str(value))


def main():
    names = ['test_eq', 'test_voice', 'test_custom', 'test_bad']
    tones = []
    ui = None
    created = None
    default = graph.run(graph.BINARY, 'get-default', 'sink').stdout
    custom = '/tmp/board/test-filter.conf'
    broken = '/tmp/board/test-broken-filter.conf'
    try:
        graph.run(graph.BINARY, 'create-effect', names[0], 'eq')
        graph.run(graph.BINARY, 'create-effect', names[1], 'voice')
        assert {e['group'] for e in graph.query('effects')} >= set(names[:2])
        assert graph.run(graph.BINARY, 'create-effect', names[0], 'eq', check=False).returncode == 3
        eq_params = params('test_eq')
        assert eq_params['mid:Gain']['value'] == 0
        assert eq_params['bass:Freq']['default'] == 120
        assert eq_params['dry:Mult']['default'] == 0
        # A freshly created, unconnected effect must accept idle edits too.
        set_param('test_eq', 'bass:Freq', 125)
        assert params('test_eq')['bass:Freq']['value'] == 125
        set_param('test_eq', 'bass:Freq', 120)
        for parameter, value in [('mid:Gain', 999), ('mid:Q', 0), ('mid:Freq', 0), ('missing', 1)]:
            assert graph.run(graph.BINARY, 'set-effect-param', 'test_eq', parameter, str(value), check=False).returncode == 3
        assert graph.run(graph.BINARY, 'set-effect-param', 'test_eq', 'mid:Gain', 'nan', check=False).returncode == 2
        assert graph.run(graph.BINARY, 'effect-params', 'missing', check=False).returncode == 3

        for name, frequency in [('test_eq', 1000), ('test_voice', 440)]:
            tones.append(subprocess.Popen([PROBE, 'play', graph.PREFIX + name,
                                           'pipemixer.effect.' + name + '.input', str(frequency)],
                                          env=graph.ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            graph.wait_for(lambda: any(n['name'] == graph.PREFIX + name for n in graph.query()['nodes']))
            for role in ['input', 'output']:
                graph.run(graph.BINARY, 'set-volume', 'pipemixer.effect.' + name + '.' + role, '100')
                graph.run(graph.BINARY, 'set-mute', 'pipemixer.effect.' + name + '.' + role, 'off')
        eq_target = 'pipemixer.effect.test_eq.output'
        flat = capture(eq_target)[0]
        assert .08 < flat < .10, flat
        ids = {n['id'] for n in graph.query()['nodes'] if n['kind'] == 'effect' and n['group'] == 'test_eq'}
        set_param('test_eq', 'mid:Gain', 6)
        boosted = capture(eq_target)[0]
        assert 1.90 < boosted / flat < 2.08, (flat, boosted)
        assert {n['id'] for n in graph.query()['nodes'] if n['kind'] == 'effect' and n['group'] == 'test_eq'} == ids
        graph.run(graph.BINARY, 'bypass-effect', 'test_eq', 'on')
        bypassed = capture(eq_target)[0]
        assert abs(bypassed - flat) < .003, (flat, bypassed)
        graph.run(graph.BINARY, 'bypass-effect', 'test_eq', 'off')
        resumed = capture(eq_target)[0]
        assert abs(resumed - boosted) < .005, (boosted, resumed)
        print('PASS idle/live EQ without node recreation: flat=%.5f +6dB=%.5f bypass=%.5f resumed=%.5f' %
              (flat, boosted, bypassed, resumed), flush=True)

        voice_target = 'pipemixer.effect.test_voice.output'
        voice = capture(voice_target)[0]
        assert .07 < voice < .11, voice
        set_param('test_voice', 'gate:Open Threshold', 0)
        set_param('test_voice', 'gate:Close Threshold', 0)
        set_param('test_voice', 'highpass:Freq', 2000)
        filtered = capture(voice_target)[0]
        assert .001 < filtered < voice * .1, (voice, filtered)
        set_param('test_voice', 'highpass:Freq', 80)
        set_param('test_voice', 'gate:Open Threshold', .3)
        set_param('test_voice', 'gate:Close Threshold', .2)
        gated = capture(voice_target)[0]
        assert gated < .0001, gated
        graph.run(graph.BINARY, 'bypass-effect', 'test_voice', 'on')
        voice_bypass = capture(voice_target)[0]
        assert .08 < voice_bypass < .10, voice_bypass
        print('PASS voice high-pass and noise gate: normal=%.5f filtered=%.5f gated=%.5f bypass=%.5f' %
              (voice, filtered, gated, voice_bypass), flush=True)

        with open(custom, 'w') as file:
            file.write('filter.graph = { nodes = [ { type=builtin name=gain label=linear control={ Mult=0.5 Add=0 } } ]'
                       ' inputs = [ "gain:In" ] outputs = [ "gain:Out" ] }\n'
                       'capture.props = { node.name=must-not-override-managed-name }\n')
        graph.run(graph.BINARY, 'create-effect', 'test_custom', '@' + custom)
        tones.append(subprocess.Popen([PROBE, 'play', graph.PREFIX + 'custom',
                                       'pipemixer.effect.test_custom.input', '440'], env=graph.ENV,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        graph.wait_for(lambda: any(n['name'] == graph.PREFIX + 'custom' for n in graph.query()['nodes']))
        half = capture('pipemixer.effect.test_custom.output')[0]
        assert .040 < half < .049, half
        set_param('test_custom', 'gain:Mult', .25)
        quarter = capture('pipemixer.effect.test_custom.output')[0]
        assert .45 < quarter / half < .55, (half, quarter)
        assert graph.run(graph.BINARY, 'bypass-effect', 'test_custom', 'on', check=False).returncode == 3
        with open(broken, 'w') as file:
            file.write('filter.graph = { nodes = [ { type=builtin name=bad label=no-such-plugin } ] }')
        assert graph.run(graph.BINARY, 'create-effect', 'test_bad', '@' + broken, check=False).returncode == 1
        assert graph.run(graph.BINARY, 'create-effect', 'test_bad', '@/no-such-config', check=False).returncode == 1
        assert not any(n['group'] == 'test_bad' for n in graph.query()['nodes'])
        print('PASS custom graph, live gain and invalid graph cleanup: half=%.5f quarter=%.5f' % (half, quarter), flush=True)

        existing = {e['group'] for e in graph.query('effects')}
        ui = Tui()
        ui.send(b'5')  # Cards view avoids the shortcut opening the focused effect directly.
        assert b'Effect chains' in ui.send(b'e')
        ui.send(b'\n')
        created = graph.wait_for(lambda: next((e['group'] for e in graph.query('effects') if e['group'] not in existing), None))
        graph.wait_for(lambda: params(created).get('bass:Freq'))
        ui.drain(.4)
        ui.send(b'e')
        ordered = [n for n in sorted(graph.query()['nodes'], key=lambda n: n['id'])
                   if n['kind'] == 'effect' and n['role'] == 'input']
        selected = 3 + next(i for i, n in enumerate(ordered) if n['group'] == created)
        ui.send(b'j' * selected + b'\n')
        ui.send(b'\n')  # Open the selected processor's parameters.
        ui.drain(.3)
        ui.send(b'l')
        graph.wait_for(lambda: params(created)['bass:Freq']['value'] > 120)
        ui.send(b'\n')
        graph.wait_for(lambda: params(created)['bass:Freq']['value'] == 120)
        ui.send(b'B')
        graph.wait_for(lambda: params(created)['wet:Mult']['value'] == 0 and params(created)['dry:Mult']['value'] == 1)
        ui.resize(8, 30); ui.resize(24, 100)
        graph.run(graph.BINARY, 'delete-effect', created)
        ui.drain(.3)
        ui.close(); ui = None
        print('PASS TUI effect creation, parameter adjustment/reset, bypass, resize and removal', flush=True)
        assert graph.run(graph.BINARY, 'get-default', 'sink').stdout == default
    finally:
        if ui:
            ui.send(b'\x1b')
            ui.close()
        for tone in tones:
            tone.terminate(); tone.wait(timeout=3)
        for name in names + ([created] if created else []):
            graph.run(graph.BINARY, 'delete-effect', name)
        for path in [custom, broken]:
            if os.path.exists(path): os.unlink(path)
    graph.reference_check()
    print('STAGE 4 PASSED', flush=True)


if __name__ == '__main__':
    main()
