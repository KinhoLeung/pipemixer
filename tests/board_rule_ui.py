"""TUI rule creation, enable/disable/delete and closing the interface."""
import json
import shutil
import time

import board_graph as graph
import board_rules as rules
from board_routing import Tui, sorted_ports

CONFIG = '/tmp/board/rule-ui-config'
graph.ENV['XDG_CONFIG_HOME'] = CONFIG
NAMES = [graph.PREFIX + 'rule-ui-' + n for n in ['source', 'destination']]


def main():
    shutil.rmtree(CONFIG, ignore_errors=True)
    assert not any(n['name'] in NAMES for n in graph.query()['nodes'])
    daemon = ui = None
    try:
        for name in NAMES:
            graph.create_sink(name)
        pair = (rules.endpoint(NAMES[0], 'output'), rules.endpoint(NAMES[1], 'input'))
        daemon = rules.start()
        graph.wait_for(lambda: rules.status()['engine_running'])
        ui = Tui()
        assert b'Routing matrix' in ui.send(b'r')
        outputs, inputs = sorted_ports('output'), sorted_ports('input')
        row = next(i for i, p in enumerate(outputs) if p['node_name'] == NAMES[0] and p['channel'] == 'FL')
        column = next(i for i, p in enumerate(inputs) if p['node_name'] == NAMES[1] and p['channel'] == 'FL')
        ui.send(b'g' + b'j' * row + b'l' * column + b'a')
        graph.wait_for(lambda: rules.state('route1') == 'connected')
        ui.close(); ui = None
        assert rules.between(*pair)
        graph.remove_sink(NAMES[1]); graph.create_sink(NAMES[1])
        graph.wait_for(lambda: rules.state('route1') == 'connected')
        print('PASS matrix saves stable-name rule; closed TUI does not stop reconnecting', flush=True)

        ui = Tui()
        screen = ui.send(b'a')
        assert b'Automatic routing' in screen and b'connected' in screen, screen
        ui.send(b'\n')
        graph.wait_for(lambda: not rules.between(*pair))
        assert rules.state('route1') == 'disabled'
        ui.send(b'a\n')
        graph.wait_for(lambda: rules.state('route1') == 'connected')
        ui.send(b'a'); ui.resize(8, 30); ui.resize(24, 100)
        ui.send(b'j\n')
        graph.wait_for(lambda: not rules.between(*pair))
        assert not rules.status()['rules']
        print('PASS rule menu shows state, toggles, deletes and handles terminal resize', flush=True)
    finally:
        if ui: ui.close()
        rules.stop(daemon)
        for name in reversed(NAMES): graph.remove_sink(name)
        shutil.rmtree(CONFIG, ignore_errors=True)
    graph.reference_check()
    print('AUTOMATIC ROUTING TUI PASSED', flush=True)


if __name__ == '__main__': main()
