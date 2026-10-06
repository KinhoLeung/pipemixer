"""Parallel batches, bounded engine memory and CPU with eight simultaneous fades."""
import json,os,resource,shutil,subprocess,time
import board_graph as graph

graph.ENV['XDG_CONFIG_HOME']='/tmp/board/automation-resource-config'
NAMES=['test_auto_load%d'%i for i in range(8)]
TARGETS=['pipemixer.bus.'+n+'.output' for n in NAMES]
def command(*args,check=True):
    r=subprocess.run([graph.BINARY,*args],env=graph.ENV,text=True,capture_output=True,timeout=15)
    if check and r.returncode:raise AssertionError((args,r.returncode,r.stderr))
    return r

def status():return json.loads(command('--json','automation-status').stdout)
def volume(target):return json.loads(command('--json','get-volume',target).stdout)['channels'][0]['percent']
def proc(pid):
    stats=open('/proc/%d/stat'%pid).read().rsplit(')',1)[1].split();ticks=int(stats[11])+int(stats[12]);rss=int(open('/proc/%d/statm'%pid).read().split()[1])*os.sysconf('SC_PAGESIZE')//1024;return ticks,rss

def main():
    before=graph.query()
    try:
        for name,target in zip(NAMES,TARGETS):command('create-bus',name);command('set-volume',target,'100')
        rules=[{'name':'long','on_start':True,'when':{'elapsed_ms':0},'actions':[{'type':'fade-volume','target':TARGETS[0],'value':0,'duration_ms':1800},{'type':'wait','duration_ms':800}]},
               {'name':'short','on_start':True,'when':{'elapsed_ms':0},'actions':[{'type':'fade-volume','target':TARGETS[1],'value':40,'duration_ms':100}]}]
        path='/tmp/board/auto-load.json';json.dump({'format':'pipemixer.automation','version':1,'rules':rules},open(path,'w'));command('import-automation',path);command('start-automation')
        graph.wait_for(lambda:len(status()['rules'])==2 and next(r for r in status()['rules'] if r['name']=='short')['state']=='active')
        assert status()['active_fades']>=1 and abs(volume(TARGETS[1])-40)<.01,status()
        graph.wait_for(lambda:not status()['queued_batches']);print('PASS short action completes while unrelated long fade/wait is still running',flush=True)
        json.dump({'format':'pipemixer.automation','version':1,'rules':[]},open(path,'w'));command('import-automation',path);graph.wait_for(lambda:not status()['rules'])
        for target in TARGETS:command('set-volume',target,'100');command('fade-volume',target,'0','12000','smooth')
        pid=status()['pid'];start=time.monotonic();ticks,rss=proc(pid);samples=[rss]
        services={name:int(subprocess.check_output(['pidof',name],text=True).strip()) for name in ['pipewire','wireplumber']}
        service_ticks={name:proc(service)[0] for name,service in services.items()}
        for step in range(5):
            time.sleep(2);samples.append(proc(pid)[1])
            if step==1:
                values=[volume(target) for target in TARGETS]
                assert all(5<value<99 for value in values),values
                assert status()['active_fades']==8,status()
                print('PASS every concurrent fade advances and live volume queries remain responsive',flush=True)
        end_ticks,end_rss=proc(pid);cpu=(end_ticks-ticks)/os.sysconf('SC_CLK_TCK')/(time.monotonic()-start)*100
        service_cpu={name:(proc(service)[0]-service_ticks[name])/os.sysconf('SC_CLK_TCK')/(time.monotonic()-start)*100 for name,service in services.items()}
        graph.wait_for(lambda:not status()['active_fades']);assert all(j['reason']=='completed' for j in status()['jobs']),status()
        # Quiet clients used to stall in the second registry round trip after
        # this property traffic. Repeat fresh connections without debug logs.
        for _ in range(3):assert all(abs(volume(t))<.01 for t in TARGETS)
        assert max(samples)-min(samples)<1024,samples
        print('PASS eight simultaneous 12-second fades: engine RSS %d..%d KiB, CPU %.2f%% of one core'%(min(samples),max(samples),cpu),flush=True)
        print('CONTROL LOAD: PipeWire CPU %.2f%%, WirePlumber CPU %.2f%% of one core'%(service_cpu['pipewire'],service_cpu['wireplumber']),flush=True)
        assert cpu<20,cpu
        assert service_cpu['pipewire']<50,service_cpu
        assert service_cpu['wireplumber']<85,service_cpu
        print('PASS 24 fresh quiet volume queries after multi-node property traffic',flush=True)
        command('stop-automation');graph.wait_for(lambda:not status()['running'])
        starters=[subprocess.Popen([graph.BINARY,'fade-volume',target,'50','600'],env=graph.ENV,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) for target in TARGETS]
        for p in starters:
            out,err=p.communicate(timeout=15);assert p.returncode==0,(p.returncode,err)
        graph.wait_for(lambda:not status()['active_fades'])
        assert len(status()['jobs'])==8 and all(j['reason']=='completed' for j in status()['jobs']),status()
        assert all(abs(volume(t)-50)<.01 for t in TARGETS)
        print('PASS eight concurrent starters share one ready worker and all controls finish',flush=True)
    finally:
        command('stop-automation',check=False)
        for name in NAMES:command('delete-bus',name,check=False)
        shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'],ignore_errors=True)
    after=graph.query();assert {n['name'] for n in before['nodes']}=={n['name'] for n in after['nodes']};assert {l['id'] for l in before['links']}=={l['id'] for l in after['links']}
    graph.reference_check();print('AUTOMATION RESOURCES PASSED',flush=True)
if __name__=='__main__':main()
