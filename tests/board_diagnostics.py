"""Analytic PCM meters, profiler/resource queries and diagnostics UI on the board."""
import json
import math
import os
import shutil
import subprocess
import time
import board_graph as graph
from board_audio import PROBE
from board_routing import Tui, sorted_ports

graph.ENV['XDG_CONFIG_HOME']='/tmp/board/diagnostics-test-config'
FX='test_diagnostics_fx'

def command(*args,check=True):
    r=subprocess.run([graph.BINARY,*args],env=graph.ENV,text=True,capture_output=True,timeout=15)
    if check and r.returncode: raise AssertionError((args,r.stderr))
    return r

def main():
    before=graph.query(); links={l['id'] for l in before['links']}; names={n['name'] for n in before['nodes']}
    tone=ui=None
    try:
        command('create-effect',FX,'empty')
        command('add-effect-stage',FX,'gain');command('add-effect-stage',FX,'gain')
        target='pipemixer.effect.'+FX+'.output'
        for role in ['input','output']:
            command('set-volume','pipemixer.effect.'+FX+'.'+role,'100');command('set-mute','pipemixer.effect.'+FX+'.'+role,'off')
        tone=subprocess.Popen([PROBE,'play',graph.PREFIX+'diagnostics-tone','pipemixer.effect.'+FX+'.input','1000'],env=graph.ENV,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        graph.wait_for(lambda:any(n['name']==graph.PREFIX+'diagnostics-tone' for n in graph.query()['nodes']))
        measured=json.loads(command('--json','meter',target,'700').stdout)
        assert measured['rate']==48000 and measured['frames']>24000,measured
        assert [c['name'] for c in measured['channels']]==['FL','FR']
        for c in measured['channels']:
            assert abs(c['peak']-.125)<.002 and abs(c['rms']-.125/math.sqrt(2))<.002,c
            assert not c['clipped_samples'] and not c['invalid_samples']
        status=json.loads(command('--json','diagnostics','1000').stdout)
        assert status['profiler_available'] and status['rss_kb']>0 and status['memory_available_kb']>0,status
        node=next(n for n in status['nodes'] if n['name']==target)
        assert node['measured'] and node['rate']==48000 and node['quantum']>0,node
        assert abs(node['cycle_ms']-1000*node['quantum']/node['rate'])<.00001
        assert node['busy_us']>=0 and node['wait_us']>=0 and node['xruns']>=0
        print('PASS analytic stereo peak/RMS and live profiler: peak %.6f RMS %.6f; quantum %d rate %d CPU %.2f%% RSS %d KiB' %
              (measured['channels'][0]['peak'],measured['channels'][0]['rms'],node['quantum'],node['rate'],status['audio_cpu_percent_one_core'],status['rss_kb']),flush=True)
        command('set-effect-param',FX,'gain1:Mult','4');command('set-effect-param',FX,'gain2:Mult','4')
        clipped=json.loads(command('--json','meter',target,'500').stdout)
        assert all(c['peak']>1.9 and c['clipped_samples']>1000 for c in clipped['channels']),clipped
        print('PASS measured overload and per-channel sample counters',flush=True)
        assert command('meter',target,'0',check=False).returncode==2
        assert command('--timeout','300','meter','pipemixer.no-such-node','100',check=False).returncode==3
        assert links<={l['id'] for l in graph.query()['links']},'Existing audio links changed'
        ui=Tui();ui.send(b'r')
        index=next(i for i,p in enumerate(sorted_ports('output')) if p['node_name']==target)
        ui.send(b'g'+b'j'*index)
        screen=ui.send(b'i');screen+=ui.drain(1)
        assert b'Diagnostics' in screen and b'RMS' in screen,screen
        ui.send(b'\n');ui.resize(8,30);ui.resize(24,100);ui.send(b'\x1b')
        assert b'Diagnostics' in ui.send(b'i')
        ui.send(b'\x1b');ui.send(b'\x1b');ui.close();ui=None
        print('PASS diagnostics TUI, meter reset, resize and routing-menu isolation',flush=True)
    finally:
        if ui:ui.close()
        if tone:tone.terminate();tone.wait(timeout=3)
        command('delete-effect',FX,check=False)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'],ignore_errors=True)
    after=graph.query()
    assert names=={n['name'] for n in after['nodes']}
    assert links=={l['id'] for l in after['links']}
    graph.reference_check()
    print('PROFESSIONAL MONITORING PASSED; user audio paths preserved',flush=True)

if __name__=='__main__':main()
