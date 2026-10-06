"""Supervisor recovers the engine and respects persistent start/stop preference."""
import json,os,shutil,signal,subprocess,time
import board_graph as graph

graph.ENV['XDG_CONFIG_HOME']='/tmp/board/automation-supervisor-config'
ENV=dict(graph.ENV,PIPEMIXER_BINARY=graph.BINARY,PIPEMIXER_WAIT_ROUTING='0')

def command(*args,check=True):
    r=subprocess.run([graph.BINARY,*args],env=graph.ENV,text=True,capture_output=True,timeout=15)
    if check and r.returncode:raise AssertionError((args,r.returncode,r.stderr))
    return r

def status():return json.loads(command('--json','automation-status').stdout)
def supervisor():return subprocess.Popen(['sh','/tmp/board/pipemixer-automation-session'],env=ENV,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
def main():
    before=graph.query();session=None
    try:
        path='/tmp/board/auto-supervisor.json';json.dump({'format':'pipemixer.automation','version':1,'rules':[{'name':'persisted','when':{'elapsed_ms':86400000},'actions':[{'type':'volume','target':'test_absent','value':50}]}]},open(path,'w'))
        command('import-automation',path);session=supervisor();graph.wait_for(lambda:status()['running']);graph.wait_for(lambda:len(status()['rules'])==1)
        first=status()['pid'];os.kill(first,signal.SIGKILL);graph.wait_for(lambda:status()['running'] and status()['pid']!=first)
        graph.wait_for(lambda:len(status()['rules'])==1);assert status()['rules'][0]['fired']==0 and status()['active_fades']==0
        command('stop-automation');graph.wait_for(lambda:not status()['running']);time.sleep(2);assert not status()['running']
        assert open(graph.ENV['XDG_CONFIG_HOME']+'/pipemixer/automation-enabled').read()=='off\n'
        command('start-automation');graph.wait_for(lambda:status()['running']);session.terminate();session.wait(timeout=5);session=None
        graph.wait_for(lambda:not status()['running']);assert open(graph.ENV['XDG_CONFIG_HOME']+'/pipemixer/automation-enabled').read()=='on\n'
        session=supervisor();graph.wait_for(lambda:status()['running']);graph.wait_for(lambda:len(status()['rules'])==1)
        assert status()['rules'][0]['fired']==0
        print('PASS engine SIGKILL recovery, stable definitions, persistent stop, explicit restart and supervisor recreation',flush=True)
    finally:
        command('stop-automation',check=False)
        if session:session.terminate();session.wait(timeout=5)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'],ignore_errors=True)
    after=graph.query();assert before==after
    graph.reference_check();print('AUTOMATION SUPERVISOR PASSED',flush=True)
if __name__=='__main__':main()
