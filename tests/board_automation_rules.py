"""Stateful condition logic, debounce, hysteresis, scenes and bounded config."""
import fcntl, json, os, shutil, subprocess, time
import board_graph as graph
from board_audio import PROBE

graph.ENV['XDG_CONFIG_HOME']='/tmp/board/automation-rules-config'
BASE='/tmp/board/automation-rules-config/pipemixer'
NAMES=['test_auto_control','test_auto_result','test_auto_presence']
A='pipemixer.bus.'+NAMES[0]+'.output';B='pipemixer.bus.'+NAMES[1]+'.output';P='pipemixer.bus.'+NAMES[2]+'.output'

def command(*args,check=True):
    r=subprocess.run([graph.BINARY,*args],env=graph.ENV,text=True,capture_output=True,timeout=35)
    if check and r.returncode:raise AssertionError((args,r.returncode,r.stderr))
    return r

def status():return json.loads(command('--json','automation-status').stdout)
def rule(name='duck'):return next((r for r in status().get('rules',[]) if r['name']==name),None)
def volume(target=B):return json.loads(command('--json','get-volume',target).stdout)['channels'][0]['percent']
def config(rules):
    path='/tmp/board/automation-import.json';json.dump({'format':'pipemixer.automation','version':1,'rules':rules},open(path,'w'))
    command('check-automation',path);command('import-automation',path)
    graph.wait_for(lambda:len(status().get('rules',[]))==len(rules))
    time.sleep(.35)

def fade(value):return {'type':'fade-volume','target':B,'value':value,'duration_ms':120,'curve':'smooth'}
def main():
    before=graph.query();tone=None
    try:
        for name in NAMES:command('create-bus',name)
        command('set-volume',A,'20');command('set-volume',B,'100');command('set-mute',A,'off')
        command('start-automation')
        when={'all':[{'type':'volume','target':A,'above':60,'hysteresis':10},{'not':{'type':'mute','target':A,'value':True}},{'any':[{'type':'present','target':P,'value':True},{'elapsed_ms':60000}]}]}
        config([{'name':'duck','hold_ms':220,'release_ms':120,'cooldown_ms':2000,'when':when,'actions':[fade(35),{'type':'wait','duration_ms':80},{'type':'mute','target':B,'value':True}], 'otherwise':[{'type':'mute','target':B,'value':False},fade(85)]}])
        command('set-volume',A,'100');time.sleep(.08);command('set-volume',A,'20');time.sleep(.3);assert rule()['fired']==0,rule()
        command('set-volume',A,'100');graph.wait_for(lambda:rule()['state']=='active' and rule()['fired']==1)
        assert abs(volume()-35)<.01 and command('get-mute',B).stdout.strip()=='on'
        command('set-volume',A,'55');time.sleep(.3);assert rule()['fired']==1
        command('set-volume',A,'45');graph.wait_for(lambda:rule()['fired']==2 and rule()['state']=='idle')
        assert abs(volume()-85)<.01 and command('get-mute',B).stdout.strip()=='off'
        released=time.monotonic();command('set-volume',A,'100');time.sleep(.35)
        assert rule()['fired']==2 and rule()['state']=='cooldown',rule()
        graph.wait_for(lambda:rule()['state']=='active' and rule()['fired']==3)
        assert time.monotonic()-released>=1.0
        print('PASS all/any/not, short transient rejection, hysteresis, rising/falling actions and cooldown',flush=True)
        old=open(BASE+'/automation.json').read();bad='/tmp/board/auto-bad.json'
        for content in ['{"format":"pipemixer.automation","version":1,"rules":[],"rules":[]}',old+' trailing','{"format":"pipemixer.automation","version":1,"rules":[],}',old.replace('220','-1',1)]:
            open(bad,'w').write(content);assert command('import-automation',bad,check=False).returncode==3
            assert open(BASE+'/automation.json').read()==old
        assert oct(os.stat(BASE+'/automation.json').st_mode&0o777)=='0o600'
        command('enable-automation','duck','off');graph.wait_for(lambda:not rule()['enabled'])
        command('set-volume',A,'20');time.sleep(.3);assert rule()['fired']==0
        print('PASS strict bounded JSON rejects duplicate keys/trailing garbage/range errors; atomic import and persistent enable',flush=True)
        high={'name':'high','priority':10,'on_start':True,'when':{'type':'present','target':P,'value':True},'actions':[fade(25)]}
        low={'name':'low','priority':0,'on_start':True,'when':{'elapsed_ms':0},'actions':[fade(70)]}
        config([low,high]);graph.wait_for(lambda:rule('high')['state']=='active' and rule('low')['state']=='suppressed')
        assert abs(volume()-25)<.01
        command('delete-bus',NAMES[2]);graph.wait_for(lambda:rule('low')['state']=='active');assert abs(volume()-70)<.01
        print('PASS deterministic priority suppresses competing controls and releases them when higher condition clears',flush=True)
        command('set-volume',A,'100');command('set-mute',B,'off')
        tone=subprocess.Popen([PROBE,'play',graph.PREFIX+'auto-rules-tone','pipemixer.bus.'+NAMES[0]+'.input','440'],env=graph.ENV,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        command('set-volume','pipemixer.bus.'+NAMES[0]+'.input','100');command('set-mute','pipemixer.bus.'+NAMES[0]+'.input','off')
        config([{'name':'level','on_start':True,'hold_ms':150,'release_ms':150,'when':{'type':'level','target':A,'above':-30,'hysteresis':3},'actions':[fade(25)],'otherwise':[fade(80)]}])
        graph.wait_for(lambda:rule('level')['state']=='active');assert abs(volume()-25)<.01
        command('set-volume',A,'50');graph.wait_for(lambda:rule('level')['state']=='idle' and rule('level')['fired']==2);assert abs(volume()-80)<.01
        tone.terminate();tone.wait(timeout=3);tone=None
        print('PASS real RMS level triggers ducking and release, metering streams remain internal',flush=True)
        command('set-volume',A,'50');command('set-volume',B,'80');command('save-scene','auto_saved')
        config([{'name':'scene','when':{'elapsed_ms':86400000},'actions':[{'type':'scene','target':'auto_saved'},{'type':'wait','duration_ms':100},fade(60)]}])
        command('set-volume',A,'20');command('trigger-automation','scene');graph.wait_for(lambda:rule('scene')['state']=='idle' and rule('scene')['fired']==1)
        assert abs(volume(A)-50)<.01 and abs(volume()-60)<.01,(volume(A),volume(),status())
        command('fade-volume',B,'0','1500')
        with open(graph.ENV['XDG_RUNTIME_DIR']+'/pipemixer-scene.lock','a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            graph.wait_for(lambda:next((j for j in status()['jobs'] if j['target']==B and not j['active'] and j['reason']=='scene operation in progress'),None))
        command('load-scene','auto_saved');time.sleep(.15);assert abs(volume()-80)<.01
        print('PASS exclusive scene operation stops in-flight automation writes; loaded volume stays stable',flush=True)
        command('stop-automation');graph.wait_for(lambda:not status()['running']);command('start-automation');graph.wait_for(lambda:rule('scene') is not None)
        assert rule('scene')['fired']==0 and not status()['config_error']
        print('PASS sequenced scene loading, wait and fade; persisted definitions reload after worker restart without replaying transient jobs',flush=True)
    finally:
        command('stop-automation',check=False)
        if tone:tone.terminate();tone.wait(timeout=3)
        for name in NAMES:command('delete-bus',name,check=False)
        command('clear-recovery',check=False);command('delete-scene','auto_saved',check=False)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'],ignore_errors=True)
    after=graph.query();assert {n['name'] for n in before['nodes']}=={n['name'] for n in after['nodes']};assert {l['id'] for l in before['links']}=={l['id'] for l in after['links']}
    graph.reference_check();print('AUTOMATION RULES PASSED',flush=True)
if __name__=='__main__':main()
