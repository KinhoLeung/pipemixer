"""Multi-select routing, channel matching, combined cycles and rollback."""
import subprocess
import time

import board_graph as graph
from board_audio import PROBE
from board_terminal import ViewTui

NAMES = [graph.PREFIX + 'test_batch_' + c for c in ['a', 'b', 'c', 'd']]
FAIL = 'test_batch_failure'
BAD = graph.PREFIX + FAIL + '_zbad'
GOOD = graph.PREFIX + FAIL + '_good'
SOURCE = 'pipemixer.bus.' + FAIL + '.output'


def connect(source, channel, target, other=None):
    graph.run(graph.BINARY, 'connect', source + ':monitor_' + channel, target + ':playback_' + (other or channel))


def disconnect(source, channel, target, other=None):
    graph.run(graph.BINARY, 'disconnect', source + ':monitor_' + channel, target + ':playback_' + (other or channel))


def pair(source, target, channel):
    return [l for l in graph.query('links') if l['output']['node_name'] == source and l['input']['node_name'] == target
            and l['output']['port_name'] == 'monitor_' + channel]


def done(ui, text='complete'):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        ui.drain(.05)
        if text in ui.screen.text:
            # Rollback remains alive briefly to observe late registry globals.
            if text == 'rolled back': ui.drain(1.2)
            return
    raise AssertionError(ui.screen.text)


def main():
    ui = reject = tone = None
    assert not any(n['name'] in NAMES + [BAD, GOOD, SOURCE] for n in graph.query()['nodes'])
    try:
        for name in NAMES: graph.create_sink(name)
        connect(NAMES[0], 'FL', NAMES[2]); original = pair(NAMES[0], NAMES[2], 'FL')[0]['id']
        connect(NAMES[0], 'FL', NAMES[1]); outside = pair(NAMES[0], NAMES[1], 'FL')[0]['id']
        ui = ViewTui(); ui.send(b'r/test_batch_\ngnjjnllllNllNc'); done(ui)
        assert len(graph.query('links')) == 9, graph.query('links')
        assert pair(NAMES[0], NAMES[2], 'FL')[0]['id'] == original
        ports = {p['id']: p for p in graph.query('ports')}
        assert all(ports[l['output']['port_id']]['channel'] == ports[l['input']['port_id']]['channel'] for l in graph.query('links'))
        ui.send(b'c'); done(ui)
        assert len(graph.query('links')) == 9
        ui.send(b'd'); done(ui)
        assert [l['id'] for l in graph.query('links')] == [outside]
        disconnect(NAMES[0], 'FL', NAMES[1])
        print('PASS multi-source/target node marking, stereo pairing, idempotency and selected-only disconnect', flush=True)

        connect(NAMES[1], 'FL', NAMES[0]); connect(NAMES[3], 'FL', NAMES[2], 'FR')
        original_links = graph.query('links')
        ui.send(b'ugxjjjjjxllllllXhhhXc')
        assert 'feedback loop' in ui.screen.text, ui.screen.text
        assert graph.query('links') == original_links
        disconnect(NAMES[1], 'FL', NAMES[0]); disconnect(NAMES[3], 'FL', NAMES[2], 'FR')
        print('PASS proposed links forming a combined feedback cycle rejected before any mutation', flush=True)

        ui.send(b'ugvgxllllllX')
        assert 'O:2 I:2' in ui.screen.text, ui.screen.text
        ui.send(b'zZc'); done(ui); assert len(graph.query('links')) == 2
        ui.send(b'd'); done(ui); assert not graph.query('links')
        ui.send(b'uvvg')  # Clear, cycle node -> type -> flat.
        ui.send(b'nllllllN')
        assert 'O:2 I:2' in ui.screen.text, ui.screen.text
        graph.remove_sink(NAMES[3]); ui.drain(.4)
        assert 'O:2 I:0' in ui.screen.text, ui.screen.text
        graph.create_sink(NAMES[3]); ui.drain(.4)
        assert 'O:2 I:0' in ui.screen.text
        ui.send(b'c'); assert not graph.query('links')
        ui.send(b'/test_batch_a\n'); assert 'O:0 I:0' in ui.screen.text
        ui.send(b'/\x15test_batch_\n')
        print('PASS disappeared/recycled ports lose marks; applying a filter clears selection', flush=True)

        # Use a fresh output channel for the incompatible target: PipeWire
        # can reuse an already negotiated format when a port fans out.
        graph.run(graph.BINARY, 'create-bus', FAIL); graph.create_sink(GOOD)
        reject = subprocess.Popen([PROBE, 'reject-digital', BAD], env=graph.ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        graph.wait_for(lambda: any(p['node_name'] == BAD for p in graph.query('ports')))
        graph.wait_for(lambda: any(p['node_name'] == GOOD and p['name'] == 'playback_FR' for p in graph.query('ports')))
        connect(NAMES[0], 'FR', NAMES[2]); original = pair(NAMES[0], NAMES[2], 'FR')[0]['id']
        tone = subprocess.Popen([PROBE, 'play', graph.PREFIX + 'test_batch_tone',
                                 'pipemixer.bus.' + FAIL + '.input', '1000'], env=graph.ENV,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        graph.wait_for(lambda: any(n['name'] == graph.PREFIX + 'test_batch_tone' for n in graph.query()['nodes']))
        time.sleep(.4)
        baseline = {l['id'] for l in graph.query('links')}
        ui.send(b'/\x15test_batch_failure\ngjjjjnXlllXc'); done(ui, 'rolled back')
        graph.wait_for(lambda: {l['id'] for l in graph.query('links')} == baseline)
        tone.terminate(); tone.wait(timeout=3); tone = None
        reject.terminate(); reject.wait(timeout=3); reject = None
        graph.run(graph.BINARY, 'delete-bus', FAIL); graph.remove_sink(GOOD)
        ui.send(b'/\x15test_batch_\n')
        ui.send(b'ugnjjnllllNllNc\x1b'); done(ui, 'rolled back')
        graph.wait_for(lambda: [l['id'] for l in graph.query('links')] == [original])
        ui.resize(8, 30); ui.resize(40, 140)
        ui.close(); ui = None
        assert [l['id'] for l in graph.query('links')] == [original]
        print('PASS real negotiation failure and cancellation roll back only new links, preserve existing links and keep TUI alive', flush=True)
    finally:
        if ui: ui.send(b'\x1b'); ui.drain(1.3); ui.close()
        if reject: reject.terminate(); reject.wait(timeout=3)
        if tone: tone.terminate(); tone.wait(timeout=3)
        graph.run(graph.BINARY, 'delete-bus', FAIL)
        graph.remove_sink(GOOD)
        for name in reversed(NAMES): graph.remove_sink(name)
    graph.reference_check()
    print('BATCH ROUTING OPERATIONS PASSED', flush=True)


if __name__ == '__main__': main()
