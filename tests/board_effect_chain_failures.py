"""A bad graph leaves the original intact; a failed replacement rolls back."""
import json
import shutil
import subprocess
import time

import board_graph as graph
from board_audio import capture, PROBE
from board_effect_chain import connect

graph.ENV['XDG_CONFIG_HOME'] = '/tmp/board/chain-failure-config'
NAME = 'test_chain_failure'
SOURCE = 'test_chain_failure_source'
DESTINATION = 'test_chain_failure_destination'
SEND = 'test_chain_failure_send'


def command(*args, check=True):
    result = subprocess.run([graph.BINARY, *args], env=graph.ENV, capture_output=True, text=True, timeout=40)
    if check and result.returncode:
        raise AssertionError('%s: %s' % (args, result.stderr))
    return result


def main():
    tone = None
    try:
        for name in [SOURCE, DESTINATION]:
            command('create-bus', name)
        command('create-effect', NAME, 'eq')
        for kind, names in [('bus', [SOURCE, DESTINATION]), ('effect', [NAME])]:
            for name in names:
                for role in ['input', 'output']:
                    target = 'pipemixer.%s.%s.%s' % (kind, name, role)
                    command('set-volume', target, '100')
                    command('set-mute', target, 'off')
        for parameter, value in [('bass:Gain', '0'), ('mid:Gain', '6'), ('treble:Gain', '0'),
                                 ('mid:Freq', '1000'), ('mid:Q', '1'), ('wet:Mult', '1'), ('dry:Mult', '0')]:
            command('set-effect-param', NAME, parameter, value)
        output = 'pipemixer.effect.' + NAME + '.output'
        connect('pipemixer.bus.' + SOURCE + '.output', 'pipemixer.effect.' + NAME + '.input')
        command('create-send', SEND, output, 'pipemixer.bus.' + DESTINATION + '.input')
        command('set-volume', 'pipemixer.send.' + SEND + '.output', '55')
        command('set-mute', 'pipemixer.send.' + SEND + '.output', 'off')
        tone = subprocess.Popen([PROBE, 'play', graph.PREFIX + 'chain-failure-tone',
                                 'pipemixer.bus.' + SOURCE + '.input', '1000'], env=graph.ENV,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        graph.wait_for(lambda: any(n['name'] == graph.PREFIX + 'chain-failure-tone' for n in graph.query()['nodes']))
        baseline = capture(output)[0]
        destination = capture('pipemixer.bus.' + DESTINATION + '.output')[0]
        assert .16 < baseline < .19
        command('save-scene', 'failure')
        path = graph.ENV['XDG_CONFIG_HOME'] + '/pipemixer/scenes/failure.json'
        with open(path) as file:
            doc = json.load(file)
        for item in doc['paths']:
            if item['kind'] == 'effect' and item['name'] == NAME:
                item.update(preset='custom', chain='[{"id":"gain1","type":"gain"}]',
                            graph='{nodes=[{type=builtin name=gain1 label=no-such-plugin}] inputs=["gain1:In"] outputs=["gain1:Out"]}')
        with open(path, 'w') as file:
            json.dump(doc, file)
        ids = {n['id'] for n in graph.query()['nodes'] if n['group'] == NAME}
        failed = command('load-scene', 'failure', check=False)
        assert failed.returncode == 1 and 'original chain retained' in failed.stderr, failed
        assert ids == {n['id'] for n in graph.query()['nodes'] if n['group'] == NAME}
        assert abs(capture(output)[0] - baseline) < .003
        print('PASS invalid graph validation leaves current nodes, links and PCM intact', flush=True)

        edit = subprocess.Popen([graph.BINARY, 'add-effect-stage', NAME, 'gain'], env=graph.ENV,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 15
            destroyed = False
            while edit.poll() is None and time.monotonic() < deadline:
                node = next((n for n in graph.query()['nodes'] if n['group'] == NAME and n['role'] == 'input' and n['id'] not in ids), None)
                if node:
                    graph.run('pw-cli', 'destroy', str(node['id']))
                    destroyed = True
                    break
                time.sleep(.02)
            stdout, stderr = edit.communicate(timeout=25)
            assert destroyed and edit.returncode == 1 and 'original chain restored' in stderr, (destroyed, edit.returncode, stdout, stderr)
        finally:
            if edit.poll() is None:
                edit.terminate(); edit.wait(timeout=3)
        chain = json.loads(command('--json', 'effect-chain', NAME).stdout)
        assert [s['id'] for s in chain] == ['bass', 'mid', 'treble']
        restored = capture(output)[0]
        restored_destination = capture('pipemixer.bus.' + DESTINATION + '.output')[0]
        assert abs(restored - baseline) < .003, (baseline, restored)
        assert abs(restored_destination - destination) < .003, (destination, restored_destination)
        print('PASS replacement worker failure restores the original graph, parameters, connections and Send gain: %.6f -> %.6f' %
              (baseline, restored), flush=True)
    finally:
        if tone:
            tone.terminate(); tone.wait(timeout=3)
        command('clear-recovery', check=False)
        command('delete-send', SEND, check=False)
        command('delete-effect', NAME, check=False)
        for name in [SOURCE, DESTINATION]:
            command('delete-bus', name, check=False)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'], ignore_errors=True)
    graph.reference_check()
    print('EFFECT CHAIN FAILURE STAGE PASSED', flush=True)


if __name__ == '__main__':
    main()
