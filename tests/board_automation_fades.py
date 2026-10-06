"""Monotonic fades, independent controls, takeover and disappearing targets."""
import json
import os
import shutil
import subprocess
import time
import board_graph as graph
from board_audio import capture, PROBE

graph.ENV['XDG_CONFIG_HOME']='/tmp/board/automation-fades-config'
NAME='test_auto_fade'
TARGET='pipemixer.bus.'+NAME+'.output'
EFFECT='test_auto_effect'

def command(*args, check=True):
    r=subprocess.run([graph.BINARY,*args],env=graph.ENV,text=True,capture_output=True,timeout=15)
    if check and r.returncode: raise AssertionError((args,r.returncode,r.stderr))
    return r

def state(): return json.loads(command('--json','automation-status').stdout)
def volume(target=TARGET): return json.loads(command('--json','get-volume',target).stdout)
def parameters():return {p['name']:p['value'] for p in json.loads(command('--json','effect-params',EFFECT).stdout)}
def done(target=TARGET, parameter=''):
    return next((j for j in state()['jobs'] if j['target']==target and j['parameter']==parameter and not j['active']),None)

def main():
    before=graph.query();tone=None
    try:
        command('create-bus',NAME)
        for role in ['input','output']:
            command('set-volume','pipemixer.bus.'+NAME+'.'+role,'100');command('set-mute','pipemixer.bus.'+NAME+'.'+role,'off')
        tone=subprocess.Popen([PROBE,'play',graph.PREFIX+'auto-tone','pipemixer.bus.'+NAME+'.input','440'],env=graph.ENV,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        graph.wait_for(lambda:any(n['name']==graph.PREFIX+'auto-tone' for n in graph.query()['nodes']))
        baseline=capture(TARGET)[0]
        command('fade-volume',TARGET,'0','900')
        samples=[]
        for _ in range(5):
            samples.append(volume());time.sleep(.08)
        graph.wait_for(lambda:done())
        assert done()['reason']=='completed',done()
        assert capture(TARGET)[0]<.0001
        levels=[s['channels'][0]['percent'] for s in samples];assert all(a>b for a,b in zip(levels,levels[1:])),levels
        print('PASS detached fade to silence and real PCM silence; samples',samples,flush=True)
        command('fade-volume',TARGET,'100','350','smooth');graph.wait_for(lambda:done())
        assert done()['reason']=='completed' and abs(capture(TARGET)[0]-baseline)<.002
        command('set-volume',TARGET,'50','FR')
        command('fade-volume',TARGET,'80','300');graph.wait_for(lambda:done())
        values=volume();assert abs(values['channels'][0]['percent']-80)<.01 and abs(values['channels'][1]['percent']-40)<.01,values
        print('PASS amplitude interpolation, smooth fade and preserved channel balance',values,flush=True)
        command('fade-volume',TARGET,'0','1800');time.sleep(.12);command('set-volume',TARGET,'65')
        graph.wait_for(lambda:done());assert done()['reason']=='manual volume change',done()
        time.sleep(.2);values=volume()
        command('fade-volume',TARGET,'100','1000');time.sleep(.1);command('cancel-fade',TARGET)
        assert done()['reason']=='cancelled'
        print('PASS manual takeover and cancellation stop further writes',values,flush=True)
        command('create-effect',EFFECT,'empty');command('add-effect-stage',EFFECT,'compressor')
        command('fade-effect-param',EFFECT,'compressor1:Threshold dB','-36','400','smooth')
        graph.wait_for(lambda:done(EFFECT,'compressor1:Threshold dB'))
        assert done(EFFECT,'compressor1:Threshold dB')['reason']=='completed' and abs(parameters()['compressor1:Threshold dB']+36)<.001
        for desired in [-22,-21,-20,-19,-18]:
            command('set-effect-param',EFFECT,'compressor1:Threshold dB','-36')
            command('fade-effect-param',EFFECT,'compressor1:Threshold dB','-5','1000');time.sleep(.12)
            assert state()['active_fades']==1,state()
            command('set-effect-param',EFFECT,'compressor1:Threshold dB',str(desired))
            graph.wait_for(lambda:done(EFFECT,'compressor1:Threshold dB'))
            assert done(EFFECT,'compressor1:Threshold dB')['reason']=='manual parameter change',done(EFFECT,'compressor1:Threshold dB')
            time.sleep(.2);assert abs(parameters()['compressor1:Threshold dB']-desired)<.001
        print('PASS effect parameter fades and manual parameter takeover without graph recreation',flush=True)
        command('fade-volume',TARGET,'0','1000');command('delete-bus',NAME)
        graph.wait_for(lambda:done());assert done()['reason']=='target disappeared',done()
        command('create-bus',NAME);time.sleep(.2);assert not state()['active_fades']
        for args in [('fade-volume',TARGET,'151','100'),('fade-volume',TARGET,'10','0'),('fade-volume',TARGET,'nan','100')]:assert command(*args,check=False).returncode==2
        assert command('fade-effect-param',EFFECT,'compressor1:Ratio','.5','100',check=False).returncode==3
        print('PASS disappearance cancels fade, replacement does not inherit stale writes, invalid values rejected',flush=True)
    finally:
        command('stop-automation',check=False)
        if tone:tone.terminate();tone.wait(timeout=3)
        command('delete-effect',EFFECT,check=False);command('delete-bus',NAME,check=False)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'],ignore_errors=True)
    after=graph.query();assert {n['name'] for n in before['nodes']}=={n['name'] for n in after['nodes']}
    assert {l['id'] for l in before['links']}=={l['id'] for l in after['links']}
    graph.reference_check();print('AUTOMATION FADES PASSED',flush=True)

if __name__=='__main__':main()
