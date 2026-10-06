"""Multitrack PCM, segment continuity, shutdown, disk guards, UI and sustained costs."""
import json
import os
import shutil
import signal
import time
import board_graph as graph
import board_history as history
from board_routing import Tui, sorted_ports

graph.ENV['XDG_CONFIG_HOME']='/tmp/board/recording-test-config'
graph.ENV['XDG_STATE_HOME']='/tmp/board/recording-ui-state'
ROOT='/tmp/board/recording-takes'
command=history.command

def status(name='rectest'):
    return json.loads(command('--json','history-status',name).stdout)

def check_take(directory, tracks, frequencies=None, expected_error=False):
    meta=json.load(open(directory+'/session.json'))
    assert meta['mode']=='recording' and meta['complete'] and (bool(meta['error'])==expected_error),meta
    assert len(meta['tracks'])==tracks
    lengths=[]
    for index in range(tracks):
        frames=0
        for part in range(1,meta['parts']+1):
            pcm=history.wav(directory+'/track%02d-part%04d.wav'%(index+1,part))
            frames+=len(pcm)//2
            if part<meta['parts']:assert len(pcm)//2==meta['segment_frames']
            if frequencies and len(pcm)>48000:history.check_tone(pcm,frequencies[index])
        lengths.append(frames)
    assert lengths==[meta['frames_per_track']]*tracks,(lengths,meta)
    assert meta['bytes_written']==sum(lengths)*8
    return meta

def main():
    before=graph.query();names={n['name'] for n in before['nodes']};links={l['id'] for l in before['links']}
    buses=['test_history_a','test_history_b'];tones=[];ui=None;sessions=['rectest','signaltest','diskguard','spacewatch','eighttracks']
    shutil.rmtree(ROOT,ignore_errors=True);os.mkdir(ROOT)
    try:
        for name in buses:command('create-bus',name)
        tones=[history.tone(0,440),history.tone(1,880)]
        command('start-history','rectest','4',*history.SOURCES);time.sleep(2)
        command('record-history','rectest',ROOT+'/sustained','1')
        assert command('record-history','rectest',ROOT+'/duplicate',check=False).returncode==1
        busy=command('export-history','rectest',ROOT+'/during-recording',check=False)
        assert busy.returncode==1 and 'Stop continuous recording' in busy.stderr
        assert not os.path.exists(ROOT+'/during-recording')
        samples=[]
        for _ in range(12):
            time.sleep(5);samples.append(status())
        assert all(s['recording'] and all(t['dropped_frames']==0 for t in s['tracks']) for s in samples),samples[-1]
        rss=[s['rss_kb'] for s in samples];cpu=[s['cpu_percent_one_core'] for s in samples]
        assert max(rss)-min(rss)<1024,rss
        command('stop-recording','rectest');meta=check_take(ROOT+'/sustained',2,[440,880])
        assert meta['parts']>=2 and meta['frames_per_track']>60*48000,meta
        assert not status()['recording'] and status()['buffered_seconds']==4
        print('PASS 60 s two-track recording, 1 s preroll and default 60 s segments: %.2f s, %.2f MiB, RSS %d..%d KiB, CPU %.2f..%.2f%%, drops 0'%(meta['frames_per_track']/48000,meta['bytes_written']/1048576,min(rss),max(rss),min(cpu),max(cpu)),flush=True)
        assert command('record-history','rectest',ROOT+'/sustained',check=False).returncode==1
        assert command('record-history','rectest',ROOT+'/bad-preroll','5',check=False).returncode==1
        command('record-history','rectest',ROOT+'/reconnect')
        time.sleep(1.3);tones[1].terminate();tones[1].wait(timeout=3);tones[1]=None
        command('delete-bus',buses[1]);time.sleep(1.5)
        assert not status()['tracks'][1]['available']
        command('create-bus',buses[1]);tones[1]=history.tone(1,880);time.sleep(1.5)
        assert status()['tracks'][1]['available']
        command('stop-recording','rectest');meta=check_take(ROOT+'/reconnect',2)
        assert meta['tracks'][1]['gap_frames']>48000 and meta['tracks'][0]['gap_frames']<4800,meta
        pcm=history.wav(ROOT+'/reconnect/track02-part0001.wav');assert any(all(x==0 for x in pcm[start:start+48000]) for start in range(0,len(pcm)-48000,12000))
        print('PASS live source loss fills silence and reconnect resumes on the shared recording timeline',flush=True)
        command('stop-history','rectest')
        segmented=dict(graph.ENV,PIPEMIXER_RECORD_SEGMENT_SECONDS='2')
        command('start-history','signaltest','2',history.SOURCES[0],env=segmented)
        command('record-history','signaltest',ROOT+'/signal')
        time.sleep(5.3);pid=status('signaltest')['pid'];os.kill(pid,signal.SIGTERM)
        graph.wait_for(lambda:command('history-status','signaltest',check=False).returncode==3)
        meta=check_take(ROOT+'/signal',1,[440]);assert meta['parts']>=3
        print('PASS configurable segment boundaries and SIGTERM finalize valid WAVs and metadata',flush=True)
        guarded=dict(graph.ENV,PIPEMIXER_RECORD_RESERVE_MB='1024')
        command('start-history','diskguard','1',history.SOURCES[0],env=guarded)
        assert command('record-history','diskguard',ROOT+'/guarded',check=False).returncode==1
        assert not os.path.exists(ROOT+'/guarded') and not status('diskguard')['recording']
        assert 'space' in status('diskguard')['last_error'].lower()
        command('stop-history','diskguard')
        free=os.statvfs(ROOT);reserve=max(16,int(free.f_bavail*free.f_frsize/1048576)-4)
        watch=dict(graph.ENV,PIPEMIXER_RECORD_RESERVE_MB=str(reserve))
        command('start-history','spacewatch','1',*history.SOURCES,env=watch);time.sleep(.5)
        command('record-history','spacewatch',ROOT+'/spacewatch')
        deadline=time.monotonic()+15
        while status('spacewatch')['recording'] and time.monotonic()<deadline:time.sleep(.25)
        state=status('spacewatch');assert not state['recording'] and 'space' in state['last_error'].lower(),state
        assert state['disk_free_bytes']>=reserve*1048576
        check_take(ROOT+'/spacewatch',2,[440,880],expected_error=True)
        assert all(t['available'] for t in state['tracks']);command('stop-history','spacewatch')
        print('PASS runtime disk guard finalizes valid files and retains the configured reserve/cache',flush=True)
        print('PASS existing-directory protection, disk reserve rejection and continued cache operation',flush=True)
        # New TUI-created cache, nonblocking export, recording after TUI exit, explicit stop.
        ui=Tui();ui.send(b'r')
        index=next(i for i,p in enumerate(sorted_ports('output')) if p['node_name']==history.SOURCES[0])
        ui.send(b'g'+b'j'*index);assert b'Audio history' in ui.send(b'R')
        screen=ui.send(b'\n');assert b'Start 10-second cache' in screen or b'Select sources' in screen
        ui.send(b'g\n')
        graph.wait_for(lambda:any(s['name']=='history1' for s in json.loads(command('--json','list-history').stdout)))
        ui.drain(1);time.sleep(1)
        screen=ui.send(b'g\n');ui.resize(8,30);ui.resize(24,100);ui.drain(1)
        ui.send(b'gjj\n');graph.wait_for(lambda:status('history1')['recording']);ui.send(b'\x1b');ui.send(b'\x1b');ui.close();ui=None
        assert status('history1')['recording'];time.sleep(1)
        ui=Tui();ui.send(b'R\n');ui.send(b'gjj\n');graph.wait_for(lambda:not status('history1')['recording']);ui.send(b'\x1b');ui.close();ui=None
        take=status('history1')['recording_directory'];check_take(take,1,[440]);command('stop-history','history1')
        print('PASS TUI source selection, resize, async export/start/stop and recording survives closing TUI',flush=True)
        # Largest supported source count, measured separately from the two-track run.
        for index in range(2,8):
            name='test_record_%d'%index;buses.append(name);command('create-bus',name)
            for role in ['input','output']:command('set-volume','pipemixer.bus.'+name+'.'+role,'100');command('set-mute','pipemixer.bus.'+name+'.'+role,'off')
            import subprocess
            tones.append(subprocess.Popen([history.PROBE,'play',graph.PREFIX+'record-tone'+str(index),'pipemixer.bus.'+name+'.input',str(440+110*index)],env=graph.ENV,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL))
        sources=['pipemixer.bus.'+name+'.output' for name in buses]
        command('start-history','eighttracks','1',*sources);time.sleep(.5);command('record-history','eighttracks',ROOT+'/eight')
        eight=[]
        for _ in range(4):time.sleep(5);eight.append(status('eighttracks'))
        command('stop-recording','eighttracks');meta=check_take(ROOT+'/eight',8,[440,880]+[440+110*i for i in range(2,8)])
        assert all(t['dropped_frames']==0 for t in eight[-1]['tracks'])
        assert max(s['rss_kb'] for s in eight)-min(s['rss_kb'] for s in eight)<1024
        print('PASS 8-track sustained PCM: %.2f s, %.2f MiB, %.3f MB/s, RSS %d KiB, CPU %.2f..%.2f%%, drops 0'%(meta['frames_per_track']/48000,meta['bytes_written']/1048576,8*384000/1000000,eight[-1]['rss_kb'],min(s['cpu_percent_one_core'] for s in eight),max(s['cpu_percent_one_core'] for s in eight)),flush=True)
        command('stop-history','eighttracks')
    finally:
        if ui:ui.send(b'\x1b');ui.close()
        for name in sessions+['history1']:command('stop-history',name,check=False)
        for p in tones:
            if p:p.terminate();p.wait(timeout=3)
        for name in buses:command('delete-bus',name,check=False)
        shutil.rmtree(ROOT,ignore_errors=True);shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'],ignore_errors=True);shutil.rmtree(graph.ENV['XDG_STATE_HOME'],ignore_errors=True)
    after=graph.query();assert names=={n['name'] for n in after['nodes']};assert links=={l['id'] for l in after['links']}
    graph.reference_check();print('MULTITRACK RECORDING PASSED; user audio paths preserved',flush=True)

if __name__=='__main__':main()
