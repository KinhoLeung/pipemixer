"""Channel-aware glob expansion and batch rule creation in the TUI."""
import json
import shutil
import time

import board_graph as graph
import board_rules as rules
from board_routing import Tui, sorted_ports

CONFIG = '/tmp/board/rule-batch-config'
graph.ENV['XDG_CONFIG_HOME'] = CONFIG
SOURCES = [graph.PREFIX + 'batch-source-' + str(i) for i in range(3)]
TARGETS = [graph.PREFIX + 'batch-target-' + str(i) for i in range(2)]
MONO = graph.PREFIX + 'batch-mono'


def record(name):
    return next(r for r in rules.status()['rules'] if r['name'] == name)


def count(name, number):
    return record(name)['state'] == 'connected' and record(name)['connected_pairs'] == number


def main():
    shutil.rmtree(CONFIG, ignore_errors=True)
    assert not any(n['name'] in SOURCES + TARGETS + [MONO] for n in graph.query()['nodes'])
    daemon = ui = None
    try:
        rules.command('create-route-rule', 'batch', graph.PREFIX + 'batch-source-*:*',
                      graph.PREFIX + 'batch-target-*:*', '--match', 'glob',
                      env=dict(graph.ENV, PIPEWIRE_REMOTE='absent'))
        assert json.load(open(CONFIG + '/pipemixer/rules/batch.json'))['version'] == 2
        assert rules.command('create-route-rule', 'bad', 'a:*', 'b:*', '--match', 'regex', check=False).returncode == 2
        for name in SOURCES[:2] + TARGETS: graph.create_sink(name)
        manual = (rules.endpoint(SOURCES[0], 'output'), rules.endpoint(TARGETS[0], 'input'))
        rules.command('connect', *manual)
        manual_id = rules.between(*manual)[0]['id']
        daemon = rules.start()
        graph.wait_for(lambda: count('batch', 8))
        ports = {p['id']: p for p in graph.query('ports')}
        links = [l for l in graph.query('links') if l['output']['node_name'] in SOURCES]
        assert len(links) == 8
        assert all(ports[l['output']['port_id']]['channel'] == ports[l['input']['port_id']]['channel'] for l in links)
        graph.create_sink(SOURCES[2]); graph.wait_for(lambda: count('batch', 12))
        graph.remove_sink(TARGETS[1]); graph.wait_for(lambda: count('batch', 6))
        graph.create_sink(TARGETS[1]); graph.wait_for(lambda: count('batch', 12))
        print('PASS glob fan-out matches multiple nodes, pairs channels and expands on hotplug', flush=True)

        rules.command('enable-route-rule', 'batch', 'off')
        graph.wait_for(lambda: len(graph.query('links')) == 1)
        assert rules.between(*manual)[0]['id'] == manual_id
        rules.command('delete-route-rule', 'batch')
        graph.run('pw-cli', 'create-node', 'adapter',
                  '{ factory.name=support.null-audio-sink node.name=' + MONO +
                  ' media.class=Audio/Sink object.linger=true audio.position=[MONO] priority.session=0 }')
        graph.wait_for(lambda: any(n['name'] == MONO for n in graph.query()['nodes']))
        rules.command('create-route-rule', 'mismatch', SOURCES[0] + ':*', MONO + ':*', '--match', 'glob')
        graph.wait_for(lambda: rules.state('mismatch') == 'blocked')
        assert not any(l['input']['node_name'] == MONO for l in graph.query('links'))
        rules.command('delete-route-rule', 'mismatch')
        print('PASS owned batch links removed, manual link retained, incompatible channels blocked', flush=True)

        ui = Tui(); ui.send(b'r')
        outputs, inputs = sorted_ports('output'), sorted_ports('input')
        row = next(i for i, p in enumerate(outputs) if p['node_name'] == SOURCES[0] and p['channel'] == 'FL')
        col = next(i for i, p in enumerate(inputs) if p['node_name'] == TARGETS[0] and p['channel'] == 'FL')
        ui.send(b'g' + b'j' * row + b'l' * col + b'A')
        graph.wait_for(lambda: count('route1', 2))
        assert record('route1')['match'] == 'glob'
        ui.close(); ui = None
        graph.remove_sink(TARGETS[0]); graph.wait_for(lambda: rules.state('route1') == 'waiting')
        graph.create_sink(TARGETS[0]); graph.wait_for(lambda: count('route1', 2))
        ui = Tui()
        screen = ui.send(b'a')
        assert b'glob' in screen and b'2/2' in screen, screen
        ui.close(); ui = None
        print('PASS matrix A saves a stereo batch rule, counts are visible and reconnect with UI closed', flush=True)
    finally:
        if ui: ui.close()
        rules.stop(daemon)
        for name in reversed(SOURCES + TARGETS + [MONO]): graph.remove_sink(name)
        shutil.rmtree(CONFIG, ignore_errors=True)
    graph.reference_check()
    print('BATCH MATCHING PASSED', flush=True)


if __name__ == '__main__': main()
