"""Sustained diagnostics/history menu refresh must keep bounded process memory."""
import json
import os
import shutil
import time
import board_graph as graph
import board_history as history
from board_routing import Tui, sorted_ports

graph.ENV['XDG_CONFIG_HOME']='/tmp/board/monitoring-resource-config'

def rss(pid):
    return int(open('/proc/%d/statm'%pid).read().split()[1])*os.sysconf('SC_PAGESIZE')//1024

def plateau(ui, rounds):
    ui.drain(1);values=[]
    for _ in range(rounds):
        ui.drain(5);values.append(rss(ui.process.pid))
    assert max(values)-min(values)<1024,values
    return min(values),max(values)

def main():
    before=graph.query();names={n['name'] for n in before['nodes']};links={l['id'] for l in before['links']};ui=tone=None
    try:
        history.command('create-bus',history.NAMES[0]);tone=history.tone(0,440)
        history.command('start-history','uimemory','5',history.SOURCES[0])
        ui=Tui();ui.send(b'r')
        index=next(i for i,p in enumerate(sorted_ports('output')) if p['node_name']==history.SOURCES[0])
        ui.send(b'g'+b'j'*index);assert b'Diagnostics' in ui.send(b'i')
        low,high=plateau(ui,6);print('PASS 30 s diagnostics refresh RSS %d..%d KiB'%(low,high),flush=True)
        ui.send(b'\x1b');assert b'Audio history' in ui.send(b'R');ui.send(b'\n')
        low,high=plateau(ui,4);print('PASS 20 s history detail refresh RSS %d..%d KiB'%(low,high),flush=True)
        ui.send(b'\x1b');ui.send(b'\x1b');ui.close();ui=None
        state=json.loads(history.command('--json','history-status','uimemory').stdout)
        assert state['ready'] and all(t['dropped_frames']==0 for t in state['tracks'])
    finally:
        if ui:ui.send(b'\x1b');ui.send(b'\x1b');ui.close()
        history.command('stop-history','uimemory',check=False)
        if tone:tone.terminate();tone.wait(timeout=3)
        history.command('delete-bus',history.NAMES[0],check=False);shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'],ignore_errors=True)
    after=graph.query();assert names=={n['name'] for n in after['nodes']};assert links=={l['id'] for l in after['links']}
    graph.reference_check();print('MONITORING UI RESOURCES PASSED',flush=True)

if __name__=='__main__':main()
