"""Installed automation service across a real reboot, with reversible fixtures."""
import base64
import hashlib
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

import board_graph as graph

graph.ENV['XDG_CONFIG_HOME'] = '/root/.config'
DATA = Path('/root/.local/share/pipemixer-tests')
STATE = DATA / 'stage6-boot-state.json'
CONFIG = Path('/root/.config/pipemixer/automation.json')
PREFERENCE = CONFIG.with_name('automation-enabled')
INI = CONFIG.with_name('pipemixer.ini')
METADATA = Path('/usr/lib/pipewire-0.3/libpipewire-module-metadata.so')
SCENE = 'test_automation_boot_before'
AFTER = 'test_automation_boot_after'
BUS = 'test_automation_boot'
TARGET = 'pipemixer.bus.' + BUS + '.output'
PORT = 19090


def command(*args, check=True):
    result = subprocess.run([graph.BINARY, *args], env=graph.ENV, text=True,
                            capture_output=True, timeout=15)
    if check and result.returncode:
        raise AssertionError((args, result.returncode, result.stderr))
    return result


def status():
    return json.loads(command('--json', 'automation-status').stdout)


def wait(predicate, timeout=40):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.1)
    raise AssertionError('Installed automation state did not converge')


def ready():
    value = status()
    return value if value.get('ready') and len(value.get('rules', [])) == 2 else None


def volume():
    return json.loads(command('--json', 'get-volume', TARGET).stdout)['channels'][0]['percent']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def atomic(path, data):
    temporary = path.with_name(path.name + '.new')
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'wb') as file:
        file.write(data); file.flush(); os.fsync(file.fileno())
    os.replace(temporary, path)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def prepare():
    assert not STATE.exists() and not CONFIG.exists(), 'Existing fixture or user automation definitions'
    assert not status()['running'], status()
    assert command('get-startup-scene').stdout.strip() == 'off'
    assert json.loads(command('--json', 'recovery-status').stdout)['scene'] is None
    scenes = json.loads(command('--json', 'list-scenes').stdout)
    assert SCENE not in scenes and AFTER not in scenes
    before = graph.query()
    assert not any(node['group'] == BUS for node in before['nodes'])
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as spare:
        spare.bind(('127.0.0.1', PORT))
    command('save-scene', SCENE)
    state = {'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
             'ini_hash': digest(INI), 'metadata_hash': digest(METADATA),
             'nodes': sorted(n['name'] for n in before['nodes']),
             'preference': base64.b64encode(PREFERENCE.read_bytes()).decode() if PREFERENCE.exists() else None,
             'config_lock_existed': CONFIG.with_suffix('.json.lock').exists()}
    atomic(STATE, json.dumps(state).encode())
    try:
        document = {'format': 'pipemixer.automation', 'version': 1,
                    'osc': {'bind': '127.0.0.1', 'port': PORT, 'direct': False},
                    'rules': [
                        {'name': 'boot_late_device', 'on_start': True, 'hold_ms': 40,
                         'when': {'type': 'present', 'target': TARGET, 'value': True},
                         'actions': [{'type': 'fade-volume', 'target': TARGET,
                                      'value': 35, 'duration_ms': 300, 'curve': 'smooth'}]},
                        {'name': 'boot_osc', 'trigger': {'osc': '/test/boot/fader'},
                         'actions': [{'type': 'fade-volume', 'target': TARGET, 'value': 'input',
                                      'min': 0, 'max': 100, 'duration_ms': 300}]}]}
        source = DATA / 'stage6-boot-import.json'
        atomic(source, json.dumps(document).encode())
        command('import-automation', str(source))
        state['config_hash'] = digest(CONFIG)
        atomic(STATE, json.dumps(state).encode())
        command('start-automation'); wait(ready)
        assert status()['external']['osc_listening'], status()
        assert all(rule['fired'] == 0 for rule in status()['rules']), status()
        os.sync()
        print('AUTOMATION BOOT FIXTURE PREPARED; original boot ID ' + state['boot_id'], flush=True)
    except BaseException:
        cleanup()
        raise


def verify():
    state = json.loads(STATE.read_text())
    boot_id = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    assert boot_id != state['boot_id'], 'A real board reboot is required'
    current = wait(ready)
    assert digest(CONFIG) == state['config_hash'] and digest(INI) == state['ini_hash']
    assert digest(METADATA) == state['metadata_hash']
    assert PREFERENCE.read_text() == 'on\n'
    assert not current['jobs'] and all(rule['fired'] == 0 for rule in current['rules']), current
    assert current['external']['osc_listening'] and not current['config_error'], current
    assert json.loads(command('--json', 'list-route-rules').stdout)['engine_running']
    for process in ['pipewire', 'wireplumber']:
        assert len(subprocess.check_output(['pidof', process], text=True).split()) == 1
    subprocess.run(['/etc/init.d/S99pipemixer-automation', 'status'], check=True)
    subprocess.run(['/etc/init.d/S99pipemixer-automation', 'start'], check=True)
    assert status()['pid'] == current['pid'], status()
    print('PASS real reboot %s -> %s: definitions, preference, OSC listener and one audio session restored' %
          (state['boot_id'], boot_id), flush=True)
    try:
        command('create-bus', BUS)
        wait(lambda: not status()['active_fades'] and abs(volume() - 35) < .01)
        assert next(rule for rule in status()['rules'] if rule['name'] == 'boot_late_device')['fired'] == 1
        def string(value):
            data = value.encode() + b'\0'
            return data + b'\0' * (-len(data) % 4)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(3)
            client.sendto(string('/test/boot/fader') + string(',f') + struct.pack('>f', .75),
                          ('127.0.0.1', PORT))
            assert client.recv(2048).startswith(string('/pipemixer/reply'))
        wait(lambda: not status()['active_fades'] and abs(volume() - 75) < .01)
        assert next(rule for rule in status()['rules'] if rule['name'] == 'boot_osc')['fired'] == 1
        print('PASS late target triggers persisted condition/fade; persisted OSC mapping reaches 75%', flush=True)
        previous = status()['pid']; os.kill(previous, signal.SIGKILL)
        wait(lambda: status().get('ready') and status()['pid'] != previous)
        wait(lambda: not status()['active_fades'] and abs(volume() - 35) < .01)
        assert next(rule for rule in status()['rules'] if rule['name'] == 'boot_osc')['fired'] == 0
        assert status()['external']['osc_listening']
        print('PASS installed supervisor recovers SIGKILL; on_start reruns with fresh counters and OSC listener', flush=True)
        command('stop-automation'); wait(lambda: not status()['running'])
        subprocess.run(['/etc/init.d/S99pipemixer-automation', 'restart'], check=True)
        time.sleep(2)
        assert not status()['running'] and PREFERENCE.read_text() == 'off\n'
        print('PASS persistent off prevents restart after supervisor recreation', flush=True)
    finally:
        command('delete-bus', BUS, check=False)


def cleanup():
    state = json.loads(STATE.read_text())
    command('stop-automation'); wait(lambda: not status()['running'])
    if CONFIG.exists():
        assert 'config_hash' not in state or digest(CONFIG) == state['config_hash'], 'User changed automation definitions'
        CONFIG.unlink()
    command('delete-bus', BUS, check=False)
    command('load-scene', SCENE)
    command('save-scene', AFTER)
    directory = CONFIG.parent / 'scenes'
    assert json.loads((directory / (SCENE + '.json')).read_text()) == json.loads((directory / (AFTER + '.json')).read_text())
    command('delete-scene', AFTER); command('delete-scene', SCENE)
    if state['preference'] is None:
        PREFERENCE.unlink(missing_ok=True)
    else:
        atomic(PREFERENCE, base64.b64decode(state['preference']))
    if not state['config_lock_existed']:
        CONFIG.with_suffix('.json.lock').unlink(missing_ok=True)
    assert digest(INI) == state['ini_hash']
    assert sorted(n['name'] for n in graph.query()['nodes']) == state['nodes']
    assert command('get-startup-scene').stdout.strip() == 'off'
    assert json.loads(command('--json', 'recovery-status').stdout)['scene'] is None
    graph.reference_check()
    STATE.unlink(); os.sync()
    print('AUTOMATION BOOT FIXTURE CLEANED; original preferences, configuration and audio settings restored', flush=True)


if __name__ == '__main__':
    {'prepare': prepare, 'verify': verify, 'cleanup': cleanup}[sys.argv[1]]()
