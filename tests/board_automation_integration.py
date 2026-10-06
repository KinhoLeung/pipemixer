"""Automation conditions and actions using live route and monitor policies."""
import json
import os
import shutil
import subprocess
import time
import board_graph as graph

graph.ENV['XDG_CONFIG_HOME'] = '/tmp/board/automation-integration-config'
NAMES = ['test_auto_policy_a', 'test_auto_policy_b', 'test_auto_policy_phones']
OUT = ['pipemixer.bus.' + name + '.output' for name in NAMES]
IN = ['pipemixer.bus.' + name + '.input' for name in NAMES]
FX = 'test_auto_policy_fx'
MONITOR = 'test_auto_policy_monitor'
ROUTE = 'test_auto_policy_left'


def command(*args, check=True):
    result = subprocess.run([graph.BINARY, *args], env=graph.ENV, text=True,
                            capture_output=True, timeout=35)
    if check and result.returncode:
        raise AssertionError((args, result.returncode, result.stderr))
    return result


def status():
    return json.loads(command('--json', 'automation-status').stdout)


def rule(name):
    return next((r for r in status().get('rules', []) if r['name'] == name), None)


def monitor():
    return next((m for m in json.loads(command('--json', 'list-monitors').stdout)['monitors']
                 if m['name'] == MONITOR), None)


def route():
    return next((r for r in json.loads(command('--json', 'list-route-rules').stdout)['rules']
                 if r['name'] == ROUTE), None)


def volume():
    return json.loads(command('--json', 'get-volume', OUT[2]).stdout)['channels'][0]['percent']


def main():
    before = graph.query()
    daemon = None
    try:
        for name in NAMES:
            command('create-bus', name)
        command('create-effect', FX, 'empty')
        command('add-effect-stage', FX, 'compressor')
        command('set-effect-param', FX, 'compressor1:Ratio', '2')
        command('set-mute', OUT[2], 'off')
        command('create-route-rule', ROUTE, OUT[0] + ':capture_FL', IN[1] + ':playback_FL')
        command('enable-route-rule', ROUTE, 'off')
        command('create-monitor', MONITOR, IN[2], OUT[0], OUT[1])
        daemon = subprocess.Popen([graph.BINARY, 'routing-daemon'], env=graph.ENV,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        graph.wait_for(lambda: monitor() and monitor()['state'] == 'connected')
        default_sink = command('get-default', 'sink').stdout.strip()
        rules = [
            {'name': 'activate', 'when': {'elapsed_ms': 86400000}, 'actions': [
                {'type': 'route-rule', 'target': ROUTE, 'value': True},
                {'type': 'monitor-source', 'target': MONITOR, 'parameter': OUT[1]},
                {'type': 'monitor-solo', 'target': MONITOR, 'parameter': OUT[0]}]},
            {'name': 'release', 'when': {'elapsed_ms': 86400000}, 'actions': [
                {'type': 'route-rule', 'target': ROUTE, 'value': False},
                {'type': 'monitor-solo', 'target': MONITOR, 'parameter': 'off'},
                {'type': 'monitor-source', 'target': MONITOR, 'parameter': 'off'}]},
            {'name': 'link', 'when': {'type': 'link', 'target': OUT[0] + ':capture_FL',
                                    'input': IN[1] + ':playback_FL', 'value': True},
             'actions': [{'type': 'volume', 'target': OUT[2], 'value': 80}],
             'otherwise': [{'type': 'volume', 'target': OUT[2], 'value': 40}]},
            {'name': 'ratio', 'when': {'type': 'parameter', 'target': FX,
                                     'parameter': 'compressor1:Ratio', 'above': 3, 'hysteresis': .5},
             'actions': [{'type': 'mute', 'target': OUT[2], 'value': True}],
             'otherwise': [{'type': 'mute', 'target': OUT[2], 'value': False}]},
            {'name': 'default', 'on_start': True,
             'when': {'type': 'default', 'target': default_sink, 'parameter': 'sink'},
             'actions': [{'type': 'volume', 'target': OUT[2], 'value': 25}]}
        ]
        path = '/tmp/board/auto-integration.json'
        with open(path, 'w') as file:
            json.dump({'format': 'pipemixer.automation', 'version': 1, 'rules': rules}, file)
        command('import-automation', path)
        command('start-automation')
        graph.wait_for(lambda: rule('default') and rule('default')['state'] == 'active')
        assert abs(volume() - 25) < .01
        command('trigger-automation', 'activate')
        graph.wait_for(lambda: route()['state'] == 'connected' and monitor()['mode'] == 'solo'
                       and monitor()['active_sources'] == [OUT[0]])
        graph.wait_for(lambda: rule('link')['state'] == 'active' and abs(volume() - 80) < .01)
        command('trigger-automation', 'release')
        graph.wait_for(lambda: route()['state'] == 'disabled' and monitor()['mode'] == 'mix'
                       and monitor()['active_sources'] == OUT[:2])
        graph.wait_for(lambda: rule('link')['state'] == 'idle' and abs(volume() - 40) < .01)
        print('PASS route-rule enable/disable, monitor source/Solo/mix, live link and default-device conditions', flush=True)
        command('set-effect-param', FX, 'compressor1:Ratio', '4')
        graph.wait_for(lambda: rule('ratio')['state'] == 'active'
                       and command('get-mute', OUT[2]).stdout.strip() == 'on')
        command('set-effect-param', FX, 'compressor1:Ratio', '2.8')
        time.sleep(.2)
        assert rule('ratio')['state'] == 'active'
        command('set-effect-param', FX, 'compressor1:Ratio', '2.4')
        graph.wait_for(lambda: rule('ratio')['state'] == 'idle'
                       and command('get-mute', OUT[2]).stdout.strip() == 'off')
        assert command('get-default', 'sink').stdout.strip() == default_sink
        print('PASS live effect-parameter condition, hysteresis and release; default device unchanged', flush=True)
    finally:
        command('stop-automation', check=False)
        command('delete-monitor', MONITOR, check=False)
        if daemon:
            graph.wait_for(lambda: not any(n['kind'] == 'monitor' and n['group'] == MONITOR
                                          for n in graph.query()['nodes']))
            daemon.terminate()
            daemon.wait(timeout=5)
        command('delete-route-rule', ROUTE, check=False)
        command('delete-effect', FX, check=False)
        for name in NAMES:
            command('delete-bus', name, check=False)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'], ignore_errors=True)
    after = graph.query()
    assert {n['name'] for n in before['nodes']} == {n['name'] for n in after['nodes']}
    assert {l['id'] for l in before['links']} == {l['id'] for l in after['links']}
    graph.reference_check()
    print('AUTOMATION POLICY INTEGRATION PASSED', flush=True)


if __name__ == '__main__':
    main()
