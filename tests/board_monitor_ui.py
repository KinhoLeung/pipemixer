"""Monitor creation, editable mix/Solo and listen source controls in the TUI."""
import json
import shutil
import subprocess
import time

import board_graph as graph
import board_rules as rules
from board_audio import capture, PROBE
from board_terminal import ViewTui

CONFIG = '/tmp/board/monitor-ui-config'
graph.ENV['XDG_CONFIG_HOME'] = CONFIG
GROUPS = ['test_ui_monitor_' + c for c in ['a', 'b', 'main', 'phones']]
OUTPUTS = ['pipemixer.bus.' + name + '.output' for name in GROUPS]
INPUTS = ['pipemixer.bus.' + name + '.input' for name in GROUPS]
NAME = 'monitor1'


def record():
    return next((m for m in json.loads(rules.command('--json', 'list-monitors').stdout)['monitors'] if m['name'] == NAME), None)


def ready(sources, mode='mix'):
    r = record()
    return r and r['state'] == 'connected' and r['active_sources'] == sources and r['mode'] == mode


def focus(ui, source, destination=INPUTS[3]):
    query = 'test_ui_monitor_'
    ui.send(b'/\x15' + query.encode() + b'\n')
    def index(direction, node):
        ports = sorted((p for p in graph.query('ports') if p['direction'] == direction and query in p['node_name']),
                       key=lambda p: (p['node_name'], p['name'], p['id']))
        return next(i for i, p in enumerate(ports) if p['node_name'] == node and p['channel'] == 'FL')
    ui.send(b'g' + b'j' * index('output', source) + b'l' * index('input', destination))


def pick_node(ui, node):
    names = sorted({p['node_name'] for p in graph.query('ports') if p['direction'] == 'output'
                    and not p['node_name'].startswith('pipemixer.monitor.')})
    ui.send(b'g' + b'j' * names.index(node) + b'\n')


def menu(ui, keys):
    ui.send(b'M\n' + keys)


def main():
    shutil.rmtree(CONFIG, ignore_errors=True)
    ui = daemon = None; tones = []
    try:
        for name in GROUPS: rules.command('create-bus', name)
        for node in OUTPUTS:
            rules.command('set-volume', node, '100'); rules.command('set-mute', node, 'off')
        rules.command('set-volume', OUTPUTS[1], '50')
        for source in OUTPUTS[:2]:
            for channel in ['FL', 'FR']: rules.command('connect', source + ':capture_' + channel, INPUTS[2] + ':playback_' + channel)
        main_ids = {l['id'] for l in graph.query('links') if l['input']['node_name'] == INPUTS[2]}
        for i, frequency in enumerate([440, 880]):
            tones.append(subprocess.Popen([PROBE, 'play', graph.PREFIX + 'ui-monitor-tone' + str(i), INPUTS[i], str(frequency)],
                                          env=graph.ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        daemon = rules.start(); ui = ViewTui(); ui.send(b'r'); focus(ui, OUTPUTS[0])
        ui.send(b'M'); assert 'Independent monitors' in ui.screen.text, ui.screen.text
        ui.send(b'\n'); assert 'Monitor output' in ui.screen.text
        ui.send(b'\n'); graph.wait_for(lambda: ready([OUTPUTS[0]]))
        assert 'Monitor monitor1' in ui.screen.text, ui.screen.text
        rules.command('set-volume', 'pipemixer.monitor.monitor1.output', '100'); rules.command('set-mute', 'pipemixer.monitor.monitor1.output', 'off')
        baseline = capture(OUTPUTS[2])[0]
        ui.send(b'jj\n'); assert 'normal mix sources' in ui.screen.text
        pick_node(ui, OUTPUTS[1]); graph.wait_for(lambda: ready(OUTPUTS[:2])); ui.send(b'\x1b')
        main, phones = capture(OUTPUTS[2], OUTPUTS[3]); assert abs(main - baseline) < .002 and abs(phones - main) < .002
        print('PASS TUI creates a monitor from selected source/destination and edits its normal mix, while main PCM stays unchanged', flush=True)

        focus(ui, OUTPUTS[0]); ui.send(b'S'); graph.wait_for(lambda: ready([OUTPUTS[0]], 'solo'))
        ui.drain(1.2); assert 'Monitor:monitor1 solo [1 Solo]' in ui.screen.text, ui.screen.text
        focus(ui, OUTPUTS[1]); ui.send(b'S'); graph.wait_for(lambda: ready(OUTPUTS[:2], 'solo'))
        ui.send(b'S'); graph.wait_for(lambda: ready([OUTPUTS[0]], 'solo'))
        ui.send(b'U'); graph.wait_for(lambda: ready(OUTPUTS[:2]))
        ui.send(b'L'); graph.wait_for(lambda: ready([OUTPUTS[1]], 'listen'))
        focus(ui, OUTPUTS[0]); ui.send(b'S'); graph.wait_for(lambda: ready([OUTPUTS[0]], 'solo'))
        ui.send(b'U'); graph.wait_for(lambda: ready([OUTPUTS[1]], 'listen'))
        assert {l['id'] for l in graph.query('links') if l['input']['node_name'] == INPUTS[2]} == main_ids
        print('PASS S multi-Solo, U clear and L listening source controls; clearing Solo restores previous listening source and preserves main links', flush=True)

        menu(ui, b'j\n'); assert 'listening source' in ui.screen.text
        pick_node(ui, OUTPUTS[2]); graph.wait_for(lambda: ready([OUTPUTS[2]], 'listen'))
        ui.send(b'\x1b')
        menu(ui, b'jjj\n'); assert 'Solo sources' in ui.screen.text
        pick_node(ui, OUTPUTS[1]); graph.wait_for(lambda: ready([OUTPUTS[1]], 'solo')); ui.resize(8, 32); ui.resize(30, 120)
        ui.send(b'\x1b'); menu(ui, b'jjjj\n'); graph.wait_for(lambda: ready([OUTPUTS[2]], 'listen'))
        ui.send(b'\x1b'); menu(ui, b'\n'); graph.wait_for(lambda: ready(OUTPUTS[:2])); ui.send(b'\x1b')
        ui.send(b'v'); ui.send(b'M\n'); assert 'Monitor monitor1' in ui.screen.text; ui.send(b'\x1b')
        # Mouse opens the menu item without connecting the matrix beneath it.
        ui.send(b'M'); before = graph.query('links')
        ui.send(b'\x1b[<64;45;9M\x1b[<65;45;9M'); ui.send(b'\x1b'); assert graph.query('links') == before
        ui.send(b'r'); ui.close(); ui = None
        main, phones = capture(OUTPUTS[2], OUTPUTS[3]); assert abs(main - baseline) < .002 and abs(phones - main) < .002
        assert daemon.poll() is None
        print('PASS menu listen/Solo/mix operations, resize and matrix/menu input isolation; audio continues after closing TUI', flush=True)

        ui = ViewTui(); ui.send(b'M\n' + b'j' * 5 + b'\n'); graph.wait_for(lambda: record()['state'] == 'disabled')
        ui.send(b'\n'); graph.wait_for(lambda: ready(OUTPUTS[:2]))
        ui.send(b'j\n'); graph.wait_for(lambda: record() is None); ui.close(); ui = None
        assert {l['id'] for l in graph.query('links') if l['input']['node_name'] == INPUTS[2]} == main_ids
        print('PASS monitor can be disabled, enabled and deleted from the main TUI; main paths retained', flush=True)
    finally:
        if ui:
            ui.send(b'\x1b'); ui.close()
        for process in tones: process.terminate(); process.wait(timeout=3)
        rules.command('delete-monitor', NAME, check=False)
        if daemon and daemon.poll() is None:
            graph.wait_for(lambda: not any(n['kind'] == 'monitor' and n['group'] == NAME for n in graph.query()['nodes']))
        rules.stop(daemon)
        for name in reversed(GROUPS): rules.command('delete-bus', name)
        shutil.rmtree(CONFIG, ignore_errors=True)
    graph.reference_check()
    print('MONITOR TUI AND SOURCE SWITCHING PASSED', flush=True)


if __name__ == '__main__': main()
