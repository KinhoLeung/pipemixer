"""Display groups, independent folding, UTF-8 search, filtering and hotplug."""
import subprocess

import board_graph as graph
from board_terminal import ViewTui
from board_audio import PROBE

NAMES = [graph.PREFIX + 'test_view_' + name for name in ['a', 'b']]
BUS, SEND, EFFECT = 'test_view_bus', 'test_view_send', 'test_view_eq'


def main():
    ui = tone = None
    assert not any(n['name'] in NAMES or n['group'] in [BUS, SEND, EFFECT] for n in graph.query()['nodes'])
    try:
        for name in NAMES: graph.create_sink(name)
        ui = ViewTui(); ui.send(b'r/test_view_\nv')
        assert 'node/All' in ui.screen.text, ui.screen.text
        ui.send(b'g ')
        assert 'folded' in ui.screen.text and not graph.query('links'), ui.screen.text
        ui.send(b'zZ')
        assert 'Input:  group:' in ui.screen.text and 'folded' in ui.screen.text, ui.screen.text
        ui.send(b'Zgjllll\n')
        graph.wait_for(lambda: any(l['output']['node_name'] == NAMES[0] and l['input']['node_name'] == NAMES[1]
                                  and l['output']['port_name'] == 'monitor_FL' for l in graph.query('links')))
        before = graph.query('links')
        ui.send(b'z'); assert graph.query('links') == before
        ui.send(b'vgzZ')
        assert 'type/All' in ui.screen.text and 'Devices' in ui.screen.text and 'folded' in ui.screen.text
        assert graph.query('links') == before
        ui.send(b'zZ')
        print('PASS node/type groups and independent folding preserve actual links; expanded ports remain routable', flush=True)

        ui.send(b'/xyz\x1b')
        assert 'Find: test_view_' in ui.screen.text, ui.screen.text
        ui.send('/\x15界\n'.encode())
        assert 'Find: 界' in ui.screen.text and 'No matching' in ui.screen.text, ui.screen.text
        ui.send(b'/\x7f\n')
        assert 'Find: ' not in ui.screen.text and 'No matching' not in ui.screen.text, ui.screen.text
        ui.send(b'/test_view_\n')
        graph.remove_sink(NAMES[1]); ui.drain(.4)
        assert not graph.query('links')
        graph.create_sink(NAMES[1]); ui.drain(.4)
        ui.send(b'vgjlll\n')
        graph.wait_for(lambda: any(l['output']['node_name'] == NAMES[0] and l['input']['node_name'] == NAMES[1] for l in graph.query('links')))
        print('PASS cancel/backspace/UTF-8 search and filtered endpoint disappearance/reappearance', flush=True)

        for args in [('create-bus', BUS), ('create-send', SEND, *NAMES), ('create-effect', EFFECT, 'eq')]:
            graph.run(graph.BINARY, *args)
        tone = subprocess.Popen([PROBE, 'play', graph.PREFIX + 'test_view_app', NAMES[0], '440'], env=graph.ENV,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        graph.wait_for(lambda: any(n['name'] == graph.PREFIX + 'test_view_app' for n in graph.query()['nodes']))
        ui.send(b'vvffg')
        assert 'type/Buses' in ui.screen.text and 'Buses' in ui.screen.text, ui.screen.text
        ui.send(b'fg'); assert 'type/Sends' in ui.screen.text and 'Sends' in ui.screen.text, ui.screen.text
        ui.send(b'fg'); assert 'type/Effects' in ui.screen.text and 'Effects' in ui.screen.text, ui.screen.text
        ui.send(b'fg'); assert 'type/Applications' in ui.screen.text and 'Applications' in ui.screen.text, ui.screen.text
        ui.resize(8, 30); ui.resize(40, 140); ui.resize(24, 100)
        ui.send(b'/\x15missing\nr')
        ui.send(b'r'); assert 'Routing matrix' in ui.screen.text and 'No matching' in ui.screen.text
        ui.close(); ui = None
        print('PASS buses/sends/effects/applications classified correctly, resize and empty-view reopen', flush=True)
    finally:
        if ui: ui.send(b'\x1b'); ui.close()
        if tone: tone.terminate(); tone.wait(timeout=3)
        graph.run(graph.BINARY, 'delete-send', SEND)
        graph.run(graph.BINARY, 'delete-effect', EFFECT)
        graph.run(graph.BINARY, 'delete-bus', BUS)
        for name in reversed(NAMES): graph.remove_sink(name)
    graph.reference_check()
    print('ROUTING GROUPS PASSED', flush=True)


if __name__ == '__main__': main()
