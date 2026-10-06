"""Run on the board against an already running PipeWire instance.

Creates only isolated null sinks and removes them in finally blocks.
PIPEMIXER_BINARY selects the binary under test.
"""
import json
import os
import subprocess
import sys
import time

BINARY = os.environ.get('PIPEMIXER_BINARY', '/tmp/board/pipemixer')
ENV = dict(os.environ, XDG_RUNTIME_DIR='/run/user/0')
PREFIX = 'pipemixer.board-test.'


def run(*args, check=True):
    result = subprocess.run(args, env=ENV, text=True, capture_output=True, timeout=12)
    if check and result.returncode:
        print(result.stderr, file=sys.stderr, flush=True)
        result.check_returncode()
    return result


def query(kind='graph'):
    return json.loads(run(BINARY, '--json', 'list', kind).stdout)


def dump():
    return json.loads(run('pw-dump').stdout)


def wait_for(predicate):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.1)
    raise AssertionError('PipeWire state did not converge')


def create_sink(name):
    assert not any(n['name'] == name for n in query()['nodes']), name
    run('pw-cli', 'create-node', 'adapter',
        '{ factory.name=support.null-audio-sink node.name=' + name +
        ' node.description="PipeMixer isolated test" media.class=Audio/Sink'
        ' object.linger=true audio.position=[FL FR] priority.session=0 }')
    return wait_for(lambda: next((n for n in query()['nodes'] if n['name'] == name), None))


def remove_sink(name):
    node = next((n for n in query()['nodes'] if n['name'] == name), None)
    if node:
        run('pw-cli', 'destroy', str(node['id']))
        wait_for(lambda: not any(n['name'] == name for n in query()['nodes']))


def reference_check():
    graph = query()
    reference = dump()
    hidden = {o['id'] for o in reference
              if o['type'].endswith(':Node') and
              (o.get('info', {}).get('props', {}).get('node.name', '').startswith('pipemixer.peak.')
               or o.get('info', {}).get('props', {}).get('pipemixer.internal') in (True, 'true'))}
    ports = {}
    for obj in reference:
        if not obj['type'].endswith(':Port'):
            continue
        props = obj['info']['props']
        if props.get('node.id') in hidden:
            continue
        if 'audio' in props.get('format.dsp', '') or props.get('audio.channel'):
            ports[obj['id']] = props
    actual = {p['id']: p for p in graph['ports']}
    assert actual.keys() == ports.keys(), (actual.keys(), ports.keys())
    for ident, expected in ports.items():
        port = actual[ident]
        assert port['name'] == expected['port.name']
        assert port['node_id'] == expected['node.id']
        assert port['direction'] == ('output' if expected['port.direction'] == 'out' else 'input')
    links = {o['id']: o['info'] for o in reference if o['type'].endswith(':Link')
             and o['info']['output-port-id'] in ports and o['info']['input-port-id'] in ports}
    assert {l['id'] for l in graph['links']} == links.keys()
    for link in graph['links']:
        expected = links[link['id']]
        assert link['output']['port_id'] == expected['output-port-id']
        assert link['input']['port_id'] == expected['input-port-id']
    assert query('ports') == graph['ports']
    assert query('links') == graph['links']
    print('PASS graph agrees with pw-dump:', len(ports), 'audio ports,', len(links), 'links', flush=True)


def main():
    reference_check()
    before_default = run(BINARY, 'get-default', 'sink').stdout
    names = [PREFIX + 'source', PREFIX + 'destination']
    try:
        for name in names:
            create_sink(name)
        ports = query('ports')
        out = next(p for p in ports if p['node_name'] == names[0] and p['direction'] == 'output' and p['channel'] == 'FL')
        inp = next(p for p in ports if p['node_name'] == names[1] and p['direction'] == 'input' and p['channel'] == 'FL')
        run('pw-link', str(out['id']), str(inp['id']))
        wait_for(lambda: any(l['output']['port_id'] == out['id'] and l['input']['port_id'] == inp['id']
                             for l in query('links')))
        reference_check()
        run('pw-link', '-d', str(out['id']), str(inp['id']))
        wait_for(lambda: not any(l['output']['port_id'] == out['id'] and l['input']['port_id'] == inp['id']
                                 for l in query('links')))
        print('PASS external link creation/removal is reflected in graph', flush=True)
        for _ in range(15):
            query()
        assert run(BINARY, 'list', 'invalid', check=False).returncode == 2
        assert run(BINARY, 'get-default', 'sink').stdout == before_default
        print('PASS repeated queries, invalid arguments, default device unchanged', flush=True)
    finally:
        for name in reversed(names):
            remove_sink(name)
    reference_check()
    print('STAGE 1 PASSED', flush=True)


if __name__ == '__main__':
    main()
