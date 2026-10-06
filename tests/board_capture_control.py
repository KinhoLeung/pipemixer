"""Concurrent session ownership, text control and actual TUI multi-selection."""
import json
import os
import shutil
import subprocess
import time
import board_graph as graph
import board_history as history
from board_terminal import ViewTui
from board_routing import sorted_ports

graph.ENV['XDG_CONFIG_HOME']='/tmp/board/capture-control-config'
graph.ENV['XDG_STATE_HOME']='/tmp/board/capture-control-state'
command=history.command

def main():
    before=graph.query();names={n['name'] for n in before['nodes']};links={l['id'] for l in before['links']};tones=[];ui=None
    try:
        for name in history.NAMES:command('create-bus',name)
        tones=[history.tone(0,440),history.tone(1,880)]
        racers=[subprocess.Popen([graph.BINARY,'start-history','race','1',history.SOURCES[0]],env=graph.ENV,stdout=subprocess.PIPE,stderr=subprocess.PIPE) for _ in range(2)]
        results=[p.communicate(timeout=15) for p in racers]
        assert sum(p.returncode==0 for p in racers)==1,[(p.returncode,r) for p,r in zip(racers,results)]
        assert len(json.loads(command('--json','list-history').stdout))==1
        text=command('history-status','race').stdout
        assert 'cache' in text and history.SOURCES[0] in text and not text.startswith('{'),text
        command('stop-history','race');graph.wait_for(lambda:not json.loads(command('--json','list-history').stdout))
        print('PASS concurrent same-name creation has one owner and text/JSON control agree',flush=True)
        ui=ViewTui();ui.send(b'r')
        index=next(i for i,p in enumerate(sorted_ports('output')) if p['node_name']==history.SOURCES[0])
        ui.send(b'g'+b'j'*index);ui.send(b'R\n')
        lines=ui.screen.text.splitlines();first=next(i for i,line in enumerate(lines) if 'Start 10-second cache' in line)
        other=next(i for i,line in enumerate(lines) if history.SOURCES[1] in line and '[ ]' in line)
        ui.send(b'g'+b'j'*(other-first)+b' ')
        assert '(2/8)' in ui.screen.text and '[x] '+history.SOURCES[1] in ui.screen.text,ui.screen.text
        ui.send(b'g\n')
        graph.wait_for(lambda:any(s['name']=='history1' for s in json.loads(command('--json','list-history').stdout)))
        state=json.loads(command('--json','history-status','history1').stdout)
        assert {t['source'] for t in state['tracks']}==set(history.SOURCES),state
        ui.drain(1);ui.send(b'gjj\n')
        graph.wait_for(lambda:json.loads(command('--json','history-status','history1').stdout)['recording'])
        ui.send(b'\x1b');ui.send(b'\x1b');ui.close();ui=None;time.sleep(1.5)
        state=json.loads(command('--json','history-status','history1').stdout);directory=state['recording_directory']
        command('stop-history','history1');graph.wait_for(lambda:command('history-status','history1',check=False).returncode==3)
        meta=json.load(open(directory+'/session.json'));assert meta['complete'] and not meta['error'] and len(meta['tracks'])==2
        lengths=[]
        for index,track in enumerate(meta['tracks']):
            pcm=history.wav(directory+'/track%02d-part0001.wav'%(index+1))
            history.check_tone(pcm,440 if track['source']==history.SOURCES[0] else 880);lengths.append(len(pcm)//2)
        assert lengths==[meta['frames_per_track']]*2
        print('PASS TUI Space selects two distinct sources; stop-cache finalizes the active two-track take',flush=True)
    finally:
        if ui:ui.send(b'\x1b');ui.send(b'\x1b');ui.close()
        for name in ['race','history1']:command('stop-history',name,check=False)
        for p in tones:p.terminate();p.wait(timeout=3)
        for name in history.NAMES:command('delete-bus',name,check=False)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'],ignore_errors=True);shutil.rmtree(graph.ENV['XDG_STATE_HOME'],ignore_errors=True)
    after=graph.query();assert names=={n['name'] for n in after['nodes']};assert links=={l['id'] for l in after['links']}
    graph.reference_check();print('CAPTURE CONTROL PASSED',flush=True)

if __name__=='__main__':main()
