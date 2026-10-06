"""Actual OSC UDP and MIDI byte-stream integration, using a virtual MIDI tty."""
import json,os,pty,shutil,socket,struct,subprocess,time,tty
import board_graph as graph

graph.ENV['XDG_CONFIG_HOME']='/tmp/board/external-control-config'
NAME='test_external';TARGET='pipemixer.bus.'+NAME+'.output';DEVICE='/tmp/board/test-midi-device'
FX=NAME+'_fx';SCENE='test_external_scene'

def command(*args,check=True):
    r=subprocess.run([graph.BINARY,*args],env=graph.ENV,text=True,capture_output=True,timeout=35)
    if check and r.returncode:raise AssertionError((args,r.returncode,r.stderr))
    return r

def state():return json.loads(command('--json','automation-status').stdout)
def rule(name):return next((r for r in state().get('rules',[]) if r['name']==name),None)
def volume():return json.loads(command('--json','get-volume',TARGET).stdout)['channels'][0]['percent']
def string(text):
    data=text.encode()+b'\0';return data+b'\0'*((-len(data))%4)
def message(address,tags,*args):
    out=string(address)+string(','+tags)
    for tag,arg in zip(tags,args):out+=string(arg) if tag=='s' else struct.pack('>i' if tag=='i' else '>f',arg)
    return out

def bundle(*packets):return string('#bundle')+struct.pack('>Q',1)+b''.join(struct.pack('>I',len(p))+p for p in packets)
def send(sock,port,packet):
    sock.sendto(packet,('127.0.0.1',port));data=sock.recv(2048)
    assert data.startswith(string('/pipemixer/reply')+string(',sis')),data
    at=len(string('/pipemixer/reply'))+len(string(',sis'));nul=data.index(0,at);at+=(nul-at+4)&~3
    code=struct.unpack_from('>i',data,at)[0];return code,data

def main():
    before=graph.query();master=slave=sock=None
    try:
        command('create-bus',NAME);command('set-volume',TARGET,'100');command('set-mute',TARGET,'off')
        master,slave=pty.openpty();tty.setraw(slave);os.symlink(os.ttyname(slave),DEVICE)
        sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sock.bind(('127.0.0.1',0));sock.settimeout(3)
        spare=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);spare.bind(('127.0.0.1',0));port=spare.getsockname()[1];spare.close()
        rules=[
            {'name':'cc','trigger':{'midi':{'type':'cc','channel':1,'number':7}},'actions':[{'type':'fade-volume','target':TARGET,'value':'input','min':0,'max':100,'duration_ms':80}]},
            {'name':'press','trigger':{'midi':{'type':'note','channel':2,'number':60,'edge':'press'}},'actions':[{'type':'mute','target':TARGET,'value':True}]},
            {'name':'release','trigger':{'midi':{'type':'note','channel':2,'number':60,'edge':'release'}},'actions':[{'type':'mute','target':TARGET,'value':False}]},
            {'name':'p_param','trigger':{'midi':{'type':'note','channel':3,'number':61,'edge':'press'}},'actions':[{'type':'parameter','target':FX,'parameter':'compressor1:Ratio','value':4}]},
            {'name':'r_param','trigger':{'midi':{'type':'note','channel':3,'number':61,'edge':'release'}},'actions':[{'type':'parameter','target':FX,'parameter':'compressor1:Ratio','value':1}]},
            {'name':'program','trigger':{'midi':{'type':'program','channel':1,'number':3}},'actions':[{'type':'volume','target':TARGET,'value':65}]},
            {'name':'mapped','trigger':{'osc':'/controller/gain'},'when':{'type':'mute','target':TARGET,'value':False},'hold_ms':80,'actions':[{'type':'fade-volume','target':TARGET,'value':'input','min':10,'max':90,'duration_ms':100}]},
            {'name':'periodic','trigger':{'interval_ms':250},'enabled':False,'actions':[{'type':'volume','target':TARGET,'value':45}]}
        ]
        doc={'format':'pipemixer.automation','version':1,'osc':{'bind':'127.0.0.1','port':port,'direct':True},'midi':{'device':DEVICE},'rules':rules}
        path='/tmp/board/external-import.json';json.dump(doc,open(path,'w'));command('import-automation',path);command('start-automation')
        graph.wait_for(lambda:state().get('external',{}).get('midi_connected') and state()['external']['osc_listening'])
        assert send(sock,port,message('/pipemixer/volume','sfi',TARGET,35.,140))[0]==0
        graph.wait_for(lambda:abs(volume()-35)<.01)
        assert send(sock,port,bundle(message('/pipemixer/volume','sf',TARGET,70.),message('/pipemixer/mute','si',TARGET,1)))[0]==0
        graph.wait_for(lambda:abs(volume()-70)<.01 and command('get-mute',TARGET).stdout.strip()=='on')
        assert send(sock,port,message('/controller/gain','f',.5))[0]<0
        assert send(sock,port,message('/pipemixer/mute','si',TARGET,0))[0]==0
        time.sleep(.2);assert send(sock,port,message('/controller/gain','f',.5))[0]==0
        graph.wait_for(lambda:abs(volume()-50)<.01);assert rule('mapped')['fired']==1
        for packet in [message('/pipemixer/volume','sf',TARGET,float('nan')),bundle(message('/pipemixer/volume','sf',TARGET,20.),b'bad!'),message('/pipemixer/volume','sf',TARGET,151.),string('/x')+string(',z'),string('#bundle')+struct.pack('>Q',99)]:
            assert send(sock,port,packet)[0]<0
        time.sleep(.15);assert abs(volume()-50)<.01
        print('PASS actual UDP OSC direct controls, normalized mapping, condition gates, immediate bundles and malformed packet rejection',flush=True)
        command('create-effect',FX,'empty');command('add-effect-stage',FX,'compressor')
        def ratio():return next(p['value'] for p in json.loads(command('--json','effect-params',FX).stdout) if p['name']=='compressor1:Ratio')
        accepted=send(sock,port,message('/pipemixer/parameter','ssf',FX,'compressor1:Ratio',4.));assert accepted[0]==0,accepted
        graph.wait_for(lambda:abs(ratio()-4)<.001)
        assert send(sock,port,message('/pipemixer/parameter','ssfi',FX,'compressor1:Ratio',1.,150))[0]==0
        graph.wait_for(lambda:abs(ratio()-1)<.001)
        command('save-scene',SCENE)
        assert send(sock,port,message('/pipemixer/parameter','ssf',FX,'compressor1:Ratio',3.))[0]==0
        graph.wait_for(lambda:abs(ratio()-3)<.001)
        assert send(sock,port,message('/pipemixer/scene','s',SCENE))[0]==0
        graph.wait_for(lambda:not state()['queued_batches'] and abs(ratio()-1)<.001)
        assert send(sock,port,message('/pipemixer/rule','s','mapped'))[0]==0
        graph.wait_for(lambda:abs(volume()-10)<.01)
        print('PASS OSC immediate/faded effect parameters including exact lower bound, scene load and rule trigger',flush=True)
        os.write(master,b'\xb0\x07\x00');graph.wait_for(lambda:abs(volume())<.01)
        os.write(master,b'\x07\xf8\x7f');graph.wait_for(lambda:abs(volume()-100)<.01)
        os.write(master,b'\xb1\x07\x30');time.sleep(.2);assert abs(volume()-100)<.01
        os.write(master,b'\x91\x3c\x7f');graph.wait_for(lambda:command('get-mute',TARGET).stdout.strip()=='on')
        os.write(master,b'\x3c\x00');graph.wait_for(lambda:command('get-mute',TARGET).stdout.strip()=='off')
        os.write(master,b'\x91\x3c\x7f\x81\x3c\x20');graph.wait_for(lambda:rule('release')['fired']==2 and command('get-mute',TARGET).stdout.strip()=='off')
        os.write(master,b'\xf0\x01\x02\xf8\x03\xf7\x07\x00');time.sleep(.2);assert abs(volume()-100)<.01
        os.write(master,b'\xc0\x03');graph.wait_for(lambda:abs(volume()-65)<.01)
        assert rule('cc')['fired']==2 and rule('press')['fired']==2 and rule('program')['fired']==1
        print('PASS MIDI CC, channel filtering, running status, interleaved realtime, note on/off and velocity zero, program change and SysEx isolation',flush=True)
        os.write(master,b'\x91\x3c\x7f\x81\x3c\x00'*8+b'\x92\x3d\x7f\x82\x3d\x00'*8)
        graph.wait_for(lambda:rule('release')['fired']==10 and rule('r_param')['fired']==8 and not state()['queued_batches'])
        time.sleep(.15);assert command('get-mute',TARGET).stdout.strip()=='off' and abs(ratio()-1)<.001
        print('PASS burst note press/release keeps the newest mute and immediate parameter values after acknowledged writes',flush=True)
        os.unlink(DEVICE);os.close(master);os.close(slave);master=slave=None
        graph.wait_for(lambda:not state()['external']['midi_connected'])
        master,slave=pty.openpty();tty.setraw(slave);os.symlink(os.ttyname(slave),DEVICE)
        graph.wait_for(lambda:state()['external']['midi_connected']);os.write(master,b'\xb0\x07\x40');graph.wait_for(lambda:abs(volume()-100*64/127)<.02)
        print('PASS configured MIDI device disconnect/reconnect resumes mappings with parser state cleared',flush=True)
        command('enable-automation','periodic','on');graph.wait_for(lambda:rule('periodic')['fired']>=2)
        command('enable-automation','periodic','off');graph.wait_for(lambda:not rule('periodic')['enabled'])
        doc['osc']['direct']=False;json.dump(doc,open(path,'w'));command('import-automation',path);time.sleep(.4)
        assert send(sock,port,message('/pipemixer/volume','sf',TARGET,30.))[0]<0
        accepted=send(sock,port,message('/controller/gain','f',.25));assert accepted[0]==0,(accepted,state(),command('get-mute',TARGET).stdout);graph.wait_for(lambda:abs(volume()-30)<.01)
        print('PASS monotonic periodic triggers, persistent disable and mapping-only OSC mode',flush=True)
    finally:
        command('stop-automation',check=False)
        if sock:sock.close()
        if master is not None:os.close(master)
        if slave is not None:os.close(slave)
        if os.path.lexists(DEVICE):os.unlink(DEVICE)
        command('clear-recovery',check=False);command('delete-scene',SCENE,check=False);command('delete-effect',FX,check=False)
        command('delete-bus',NAME,check=False);shutil.rmtree(graph.ENV['XDG_CONFIG_HOME'],ignore_errors=True)
    after=graph.query();assert {n['name'] for n in before['nodes']}=={n['name'] for n in after['nodes']};assert {l['id'] for l in before['links']}=={l['id'] for l in after['links']}
    graph.reference_check();print('EXTERNAL CONTROL PASSED (MIDI transport is a virtual tty, not attached hardware)',flush=True)
if __name__=='__main__':main()
