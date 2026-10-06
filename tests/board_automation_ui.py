"""Real curses automation menu: fade editing, rules, resize and idle RSS."""
import json,os,shutil,subprocess,time
import board_graph as graph
from board_terminal import ViewTui
from board_routing import sorted_ports

graph.ENV['XDG_CONFIG_HOME']='/tmp/board/automation-ui-config'
NAME='test_ui_auto';TARGET='pipemixer.bus.'+NAME+'.output'
def command(*args,check=True):
    r=subprocess.run([graph.BINARY,*args],env=graph.ENV,text=True,capture_output=True,timeout=35)
    if check and r.returncode:raise AssertionError((args,r.returncode,r.stderr))
    return r

def status():return json.loads(command('--json','automation-status').stdout)
def volume():return json.loads(command('--json','get-volume',TARGET).stdout)['channels'][0]['percent']
def main():
    before=graph.query();ui=None
    try:
        command('create-bus',NAME);command('set-volume',TARGET,'100')
        path='/tmp/board/auto-ui.json';json.dump({'format':'pipemixer.automation','version':1,'rules':[{'name':'ui_rule','when':{'elapsed_ms':86400000},'actions':[{'type':'fade-volume','target':TARGET,'value':40,'duration_ms':100}]}]},open(path,'w'));command('import-automation',path)
        ui=ViewTui();ui.send(b'r');index=next(i for i,p in enumerate(sorted_ports('output')) if p['node_name']==TARGET);ui.send(b'g'+b'j'*index);ui.send(b'o')
        assert 'Automation / MIDI / OSC' in ui.screen.text and 'Start automation engine' in ui.screen.text,ui.screen.text
        ui.send(b'\n');graph.wait_for(lambda:status()['running']);ui.drain(.4)
        ui.send(b'g' + b'j\n');assert 'Fade editor' in ui.screen.text,ui.screen.text
        ui.send(b'gjj'+b'h'*4);assert 'Value: 80.000' in ui.screen.text,ui.screen.text
        ui.send(b'gjjjl');assert '1250 ms' in ui.screen.text,ui.screen.text
        ui.send(b'g\n');time.sleep(.2);ui.send(b'\x1b');ui.close();ui=None
        graph.wait_for(lambda:abs(volume()-80)<.01)
        assert not status()['active_fades']
        print('PASS TUI opens from matrix, starts engine, edits fade value/duration and fades continue after TUI exit',flush=True)
        ui=ViewTui();ui.send(b'o');ui.send(b'g'+b'j'*10+b'\n');assert 'Automation rule ui_rule' in ui.screen.text,ui.screen.text
        ui.send(b'\n');graph.wait_for(lambda:not status()['rules'][0]['enabled']);ui.drain(.4);assert 'Enable rule' in ui.screen.text,ui.screen.text
        ui.send(b'\n');graph.wait_for(lambda:status()['rules'][0]['enabled']);ui.send(b'gj\n');graph.wait_for(lambda:abs(volume()-40)<.01)
        ui.resize(12,55);ui.resize(24,100)
        rss=[]
        for _ in range(6):
            ui.drain(.6);rss.append(next(int(line.split()[1]) for line in open('/proc/%d/status'%ui.process.pid) if line.startswith('VmRSS:')))
        assert max(rss)-min(rss)<1024,rss
        ui.send(b'\x1b');ui.send(b'o');ui.send(b'g'+b'j'*5+b'\n');assert 'Active / recent fades' in ui.screen.text and 'completed' in ui.screen.text,ui.screen.text
        ui.send(b'\x1b');ui.close();ui=None
        assert command('automation-status').stdout.startswith('Automation'),command('automation-status').stdout
        print('PASS TUI persistent enable/disable, manual trigger, live status, resize, job view and idle RSS',rss,flush=True)
    finally:
        if ui:ui.send(b'\x1b');ui.close()
        command('stop-automation',check=False);command('delete-bus',NAME,check=False);shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'],ignore_errors=True)
    after=graph.query();assert {n['name'] for n in before['nodes']}=={n['name'] for n in after['nodes']};assert {l['id'] for l in before['links']}=={l['id'] for l in after['links']}
    graph.reference_check();print('AUTOMATION UI PASSED',flush=True)
if __name__=='__main__':main()
