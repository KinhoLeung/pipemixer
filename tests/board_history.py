"""Bounded history, aligned float WAV exports and reconnects on the board."""
import array
import json
import math
import os
import shutil
import struct
import subprocess
import time
import board_graph as graph
from board_audio import PROBE

graph.ENV['XDG_CONFIG_HOME'] = '/tmp/board/history-test-config'
ROOT = '/tmp/board/history-exports'
NAMES = ['test_history_a', 'test_history_b']
SOURCES = ['pipemixer.bus.' + name + '.output' for name in NAMES]

def command(*args, check=True, env=None):
    result = subprocess.run([graph.BINARY, *args], env=env or graph.ENV, text=True,
                            capture_output=True, timeout=35)
    if check and result.returncode:
        raise AssertionError((args, result.returncode, result.stderr))
    return result

def status(name='historytest'):
    return json.loads(command('--json', 'history-status', name).stdout)

def wav(path):
    with open(path, 'rb') as f:
        header = f.read(56)
        data = f.read()
    assert header[:4] == b'RIFF' and header[8:16] == b'WAVEfmt ', header
    assert struct.unpack_from('<HHIIHH', header, 20) == (3, 2, 48000, 384000, 8, 32)
    assert struct.unpack_from('<I', header, 4)[0] == len(data) + 48
    assert struct.unpack_from('<I', header, 52)[0] == len(data)
    assert struct.unpack_from('<I', header, 44)[0] == len(data)//8
    pcm = array.array('f'); pcm.frombytes(data)
    assert all(math.isfinite(x) for x in pcm)
    return pcm

def check_tone(pcm, frequency):
    assert len(pcm) > 48000, len(pcm)
    # Channel balance and zero crossings distinguish the separate sources.
    left = pcm[0::2]; right = pcm[1::2]
    rms = math.sqrt(sum(x*x for x in left)/len(left))
    assert abs(rms - .125/math.sqrt(2)) < .002, rms
    assert max(abs(a-b) for a,b in zip(left,right)) < 1e-6
    crossings = sum(a <= 0 < b for a,b in zip(left,left[1:]))
    measured = crossings*48000/len(left)
    assert abs(measured-frequency) < 3, (frequency, measured)
    return rms

def tone(index, frequency):
    name = NAMES[index]
    for role in ['input', 'output']:
        command('set-volume', 'pipemixer.bus.'+name+'.'+role, '100')
        command('set-mute', 'pipemixer.bus.'+name+'.'+role, 'off')
    process = subprocess.Popen([PROBE, 'play', graph.PREFIX+'history-tone'+str(index),
                                'pipemixer.bus.'+name+'.input', str(frequency)],
                               env=graph.ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(.3)
    return process

def main():
    before = graph.query(); names = {n['name'] for n in before['nodes']}; links = {l['id'] for l in before['links']}
    tones = []
    shutil.rmtree(ROOT, ignore_errors=True); os.mkdir(ROOT)
    try:
        for name in NAMES: command('create-bus', name)
        tones = [tone(0, 440), tone(1, 880)]
        command('start-history', 'historytest', '5', *SOURCES)
        assert command('start-history', 'historytest', '5', *SOURCES, check=False).returncode == 3
        assert command('start-history', 'duplicate', '5', SOURCES[0], SOURCES[0], check=False).returncode == 1
        assert command('start-history', 'huge', '120', *SOURCES, check=False).returncode == 1
        assert command('start-history', '../bad', '1', SOURCES[0], check=False).returncode == 2
        assert command('start-history', 'invalid', '0', SOURCES[0], check=False).returncode == 2
        assert command('--timeout','500','start-history','missing','1','no.such.source',check=False).returncode == 3
        time.sleep(2)
        command('--timeout', '30000', 'export-history', 'historytest', ROOT+'/initial', '1')
        manifest = json.load(open(ROOT+'/initial/session.json'))
        assert manifest['frames_per_track'] == 48000 and len(manifest['tracks']) == 2, manifest
        levels = [check_tone(wav(ROOT+'/initial/track%02d-part0001.wav'%(i+1)), f) for i,f in enumerate([440,880])]
        assert command('export-history','historytest',ROOT+'/initial','1',check=False).returncode == 1
        assert command('export-history','historytest',ROOT+'/invalid','6',check=False).returncode == 1
        hidden = [o for o in graph.dump() if o['type'].endswith(':Node') and o.get('info',{}).get('props',{}).get('node.name','').startswith('pipemixer.history.historytest.')]
        assert len(hidden) == 2 and not any(n['name'].startswith('pipemixer.history.') for n in graph.query()['nodes'])
        print('PASS separate stereo WAV exports, accurate tone/RMS, matching timelines and input bounds:',levels,flush=True)
        samples = []
        for _ in range(12):
            time.sleep(5); samples.append(status())
        rss = [s['rss_kb'] for s in samples]; cpu = [s['cpu_percent_one_core'] for s in samples]
        assert max(rss)-min(rss) < 1024, rss
        assert all(s['buffered_seconds']==5 and all(t['dropped_frames']==0 and t['invalid_samples']==0 for t in s['tracks']) for s in samples), samples[-1]
        command('export-history','historytest',ROOT+'/wrapped','3')
        for i,f in enumerate([440,880]): check_tone(wav(ROOT+'/wrapped/track%02d-part0001.wav'%(i+1)),f)
        print('PASS 60 s bounded cache after repeated wraps: RSS %d..%d KiB; CPU %.2f..%.2f%% of one core; drops 0'%(min(rss),max(rss),min(cpu),max(cpu)),flush=True)
        tones[1].terminate();tones[1].wait(timeout=3);tones[1]=None
        command('delete-bus',NAMES[1]);time.sleep(1.5)
        s=status();assert not s['tracks'][1]['available'],s
        command('export-history','historytest',ROOT+'/missing','1')
        assert max(abs(x) for x in wav(ROOT+'/missing/track02-part0001.wav')) == 0
        check_tone(wav(ROOT+'/missing/track01-part0001.wav'),440)
        assert json.load(open(ROOT+'/missing/session.json'))['tracks'][1]['gap_frames']==48000
        command('create-bus',NAMES[1]);tones[1]=tone(1,660);time.sleep(1.5)
        assert status()['tracks'][1]['available']
        command('export-history','historytest',ROOT+'/reconnected','1')
        check_tone(wav(ROOT+'/reconnected/track02-part0001.wav'),660)
        print('PASS absent track fills silence while other track continues; stable-name reconnect resumes capture',flush=True)
        command('stop-history','historytest')
        graph.wait_for(lambda: not json.loads(command('--json','list-history').stdout))
        assert command('history-status','historytest',check=False).returncode == 3
    finally:
        command('stop-history','historytest',check=False)
        for p in tones:
            if p:p.terminate();p.wait(timeout=3)
        for name in NAMES:command('delete-bus',name,check=False)
        shutil.rmtree(ROOT,ignore_errors=True);shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'],ignore_errors=True)
    after=graph.query();assert names=={n['name'] for n in after['nodes']};assert links=={l['id'] for l in after['links']}
    graph.reference_check();print('AUDIO HISTORY PASSED; user audio paths preserved',flush=True)

if __name__=='__main__':main()
