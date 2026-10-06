"""Priority arbitration per source node, explicit groups and TUI adjustment."""
import json
import shutil

import board_graph as graph
import board_rules as rules
from board_routing import Tui

CONFIG = '/tmp/board/rule-priority-config'
graph.ENV['XDG_CONFIG_HOME'] = CONFIG
SOURCES = [graph.PREFIX + 'priority-source-' + str(i) for i in range(2)]
TARGETS = [graph.PREFIX + 'priority-' + n for n in ['low', 'high', 'fanout']]


def record(name):
    return next(r for r in rules.status()['rules'] if r['name'] == name)


def destinations(source):
    return {l['input']['node_name'] for l in graph.query('links') if l['output']['node_name'] == source}


def main():
    shutil.rmtree(CONFIG, ignore_errors=True)
    assert not any(n['name'] in SOURCES + TARGETS for n in graph.query()['nodes'])
    daemon = ui = None
    try:
        for name in [SOURCES[0], TARGETS[0], TARGETS[2]]: graph.create_sink(name)
        rules.command('create-route-rule', 'low', graph.PREFIX + 'priority-source-*:*', TARGETS[0] + ':*',
                      '--match', 'glob', '--priority', '10', '--exclusive-group', 'monitor')
        rules.command('create-route-rule', 'high', SOURCES[0] + ':*', TARGETS[1] + ':*',
                      '--match', 'glob', '--priority', '50', '--exclusive-group', 'monitor')
        assert rules.command('set-route-priority', 'low', '1000001', check=False).returncode == 2
        daemon = rules.start()
        graph.wait_for(lambda: destinations(SOURCES[0]) == {TARGETS[0]})
        rules.command('set-volume', SOURCES[0], '77')
        graph.create_sink(TARGETS[1])
        graph.wait_for(lambda: destinations(SOURCES[0]) == {TARGETS[1]})
        graph.wait_for(lambda: rules.state('low') == 'suppressed')
        graph.create_sink(SOURCES[1])
        graph.wait_for(lambda: destinations(SOURCES[1]) == {TARGETS[0]})
        assert rules.state('low') == 'partial' and record('low')['suppressed_pairs'] == 2
        print('PASS priority preemption, suppression and independent winners per source node', flush=True)

        graph.remove_sink(TARGETS[1])
        graph.wait_for(lambda: destinations(SOURCES[0]) == {TARGETS[0]})
        graph.create_sink(TARGETS[1])
        graph.wait_for(lambda: destinations(SOURCES[0]) == {TARGETS[1]})
        rules.command('set-route-priority', 'low', '50')
        assert destinations(SOURCES[0]) == {TARGETS[1]}, 'Equal priority uses lexical rule names'
        rules.command('set-route-priority', 'low', '80')
        graph.wait_for(lambda: destinations(SOURCES[0]) == {TARGETS[0]})
        volumes = json.loads(rules.command('--json', 'get-volume', SOURCES[0]).stdout)['channels']
        assert all(round(c['percent']) == 77 for c in volumes)
        print('PASS missing preferred target downgrades, return/priority edits switch and retain gain', flush=True)

        rules.command('create-route-rule', 'fanout', rules.endpoint(SOURCES[0], 'output'), rules.endpoint(TARGETS[2], 'input'))
        graph.wait_for(lambda: destinations(SOURCES[0]) == {TARGETS[0], TARGETS[2]})
        rules.command('set-route-priority', 'low', '50')
        graph.wait_for(lambda: destinations(SOURCES[0]) == {TARGETS[1], TARGETS[2]})
        ui = Tui(); screen = ui.send(b'a')
        assert b'h/l: priority' in screen
        ordered = rules.status()['rules']
        slot = 2 * next(i for i, r in enumerate(ordered) if r['name'] == 'low')
        ui.send(b'j' * slot + b'l')
        graph.wait_for(lambda: record('low')['priority'] == 60)
        graph.wait_for(lambda: destinations(SOURCES[0]) == {TARGETS[0], TARGETS[2]})
        ui.close(); ui = None
        rules.command('set-route-priority', 'low', '60', 'off')
        graph.wait_for(lambda: destinations(SOURCES[0]) == set(TARGETS))
        assert record('low')['exclusive_group'] == ''
        rules.command('set-route-priority', 'low', '-10', 'monitor')
        graph.wait_for(lambda: destinations(SOURCES[0]) == {TARGETS[1], TARGETS[2]})
        print('PASS ungrouped fan-out survives, TUI changes priority, group clearing and negative priorities', flush=True)
    finally:
        if ui: ui.close()
        rules.stop(daemon)
        for name in reversed(SOURCES + TARGETS): graph.remove_sink(name)
        shutil.rmtree(CONFIG, ignore_errors=True)
    graph.reference_check()
    print('PRIORITY ROUTING PASSED', flush=True)


if __name__ == '__main__': main()
