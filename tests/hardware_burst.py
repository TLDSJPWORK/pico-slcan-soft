"""Current-firmware probe. Uses only stdlib; never flashes firmware."""
import collections
import json
import os
from pathlib import Path
import select
import shlex
import subprocess
import sys
import termios
import time

REMOTE = r'''
import socket,select,sys,json,struct,time,subprocess
s=socket.socket(socket.PF_CAN,socket.SOCK_RAW,socket.CAN_RAW)
s.setsockopt(socket.SOL_CAN_RAW,socket.CAN_RAW_FD_FRAMES,1)
s.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1048576)
s.bind(('can0',)); s.setblocking(False)
frames=[]
print(json.dumps({'ready':True}),flush=True)
while True:
 for f in select.select([s,sys.stdin],[],[],1)[0]:
  if f is s:
   while True:
    try: b=s.recv(72)
    except BlockingIOError: break
    except OSError as e:
     if e.errno==100: break  # Expected SocketCAN notification during deliberate link-down.
     raise
    ident,n=struct.unpack_from('=IB',b)
    frames.append([time.monotonic(),ident,b[8:8+n].hex(),len(b),b[5]])
  else:
   cmd=sys.stdin.readline().strip()
   if not cmd or cmd=='quit': sys.exit(0)
   if cmd in ('linkdown','linkup'):
    args=['sudo','-n','ip','link','set','can0','down'] if cmd=='linkdown' else ['sudo','-n','ip','link','set','can0','up','type','can','bitrate','125000','sample-point','0.8','sjw','8','dbitrate','2000000','dsample-point','0.8','dsjw','2','fd','on']
    subprocess.run(args,check=True);print(json.dumps({'link':cmd}),flush=True);continue
   print(json.dumps({'frames':frames}),flush=True); frames=[]
'''

def main():
    outdir=Path(sys.argv[1]); outdir.mkdir(parents=True,exist_ok=True)
    code=REMOTE.replace("'can0'",repr(os.environ.get('CAN_IF','can0'))).replace("'125000'",repr(os.environ.get('PEER_BITRATE','500000')))
    target=os.environ.get('SSH_TARGET')
    ssh_args=['ssh','-o','BatchMode=yes']
    if os.environ.get('SSH_CONTROL_PATH'): ssh_args+=['-S',os.environ['SSH_CONTROL_PATH']]
    args=ssh_args+[target,'python3 -u -c '+shlex.quote(code)] if target else [sys.executable,'-u','-c',code]
    p=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    def remote(cmd):
        p.stdin.write(cmd+'\n');p.stdin.flush()
        if not select.select([p.stdout],[],[],8)[0]: raise TimeoutError('remote receiver')
        line=p.stdout.readline()
        if not line: raise RuntimeError('remote receiver exited')
        return json.loads(line)
    if not select.select([p.stdout],[],[],8)[0]: raise TimeoutError('receiver start')
    assert json.loads(p.stdout.readline())['ready']
    fd=os.open(os.environ.get('SERIAL_PORT','/dev/ttyACM0'),os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
    a=termios.tcgetattr(fd);a[0]=a[1]=a[3]=0;a[2]=termios.CS8|termios.CREAD|termios.CLOCAL
    a[4]=a[5]=termios.B115200;a[6][termios.VMIN]=a[6][termios.VTIME]=0
    termios.tcsetattr(fd,termios.TCSANOW,a)
    def drain(seconds):
        end=time.monotonic()+seconds;data=bytearray()
        while time.monotonic()<end:
            if select.select([fd],[],[],min(.01,max(0,end-time.monotonic())))[0]:
                try:data.extend(os.read(fd,65536))
                except BlockingIOError:pass
        return bytes(data)
    def write(data):
        got=bytearray();end=time.monotonic()+15
        while data:
            if time.monotonic()>end: raise TimeoutError('serial write')
            r,w,_=select.select([fd],[fd],[],.1)
            if r:
                try:got.extend(os.read(fd,65536))
                except BlockingIOError:pass
            if w:
                try:n=os.write(fd,data);data=data[n:]
                except BlockingIOError:pass
        return bytes(got)
    try:
        setup=[]
        frame_cmd=os.environ.get('FRAME_CMD','t')
        length=int(os.environ.get('FRAME_LEN','8'))
        dlc=[0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64].index(length)
        for cmd in (b'C\r',os.environ.get('SLCAN_SPEED','S6').encode()+b'\r',b'Y2\r' if frame_cmd.lower() in 'db' else b'Y0\r',b'O\r',b'V\r',b'N\r'):
            raw=write(cmd)+drain(.3);setup.append([cmd.hex(),raw.hex()])
        (outdir/'setup.json').write_text(json.dumps(setup,indent=2))
        remote('snapshot')
        phases=json.loads(os.environ['PHASES']) if 'PHASES' in os.environ else [('baseline',20,.01),('paced',100,.001),('burst100',100,0),
                ('recovery1',20,.01),('burst1000',1000,0),('recovery2',20,.01)]
        def diagnostics():
            if not os.environ.get('DIAGNOSTICS'): return None
            response=write(b'x\r')+drain(.1)
            text=response.decode(errors='replace').strip()
            if not text.startswith('X accepted='): raise RuntimeError('diagnostics unavailable: '+repr(text))
            fields=dict(s.split('=') for s in text[2:].split())
            return {k:int(v,16 if k in ['txqsta','tefsta','txreq','trec','bdiag'] else 10) for k,v in fields.items()}
        def buffer_stats():
            if not os.environ.get('BUFFER_STATS'): return None
            response=write(b'w\r')+drain(.1)
            text=response.decode(errors='replace').strip()
            if not text.startswith('W accepted='): raise RuntimeError('buffer statistics unavailable: '+repr(text))
            return {k:int(v) for k,v in (s.split('=') for s in text[2:].split())}
        for run,(name,n,gap) in enumerate(phases,1):
            before=diagnostics()
            queue_before=buffer_stats()
            if name in ('noack','noack_close'): remote('linkdown')
            expected=[('da7a'+f'{run:04x}{i:08x}').ljust(length*2,'a') for i in range(n)]
            commands=[(frame_cmd+('000005A5' if frame_cmd.isupper() else '5A5')+f'{dlc:X}'+d+'\r').encode() for d in expected]
            start=time.monotonic(); raw=bytearray()
            if gap:
                for c in commands: raw.extend(write(c));raw.extend(drain(gap))
            else: raw.extend(write(b''.join(commands)))
            send_time=time.monotonic()-start
            raw.extend(drain(float(os.environ.get('DRAIN_SECONDS','2'))))
            frames=remote('snapshot')['frames']
            after=diagnostics()
            queue_after=buffer_stats()
            target_id=0x5a5 | (0x80000000 if frame_cmd.isupper() else 0)
            relevant=[f for f in frames if f[1]==target_id and f[2].startswith('da7a'+f'{run:04x}')]
            got=[f[2] for f in relevant]
            counts=collections.Counter(got); ex=set(expected)
            result={'phase':name,'sent':n,'received':len(got),'unique':len(counts),
                    'missing':[i for i,d in enumerate(expected) if d not in counts],
                    'unexpected':[d for d in got if d not in ex],
                    'duplicates':sum(v-1 for v in counts.values()),
                    'in_order':got==[d for d in expected if d in counts],
                    'send_seconds':send_time,'cr':raw.count(13),'bel':raw.count(7)}
            result.update(diag_before=before,diag_after=after)
            result.update(queue_before=queue_before,queue_after=queue_after)
            result['wire_format_errors']=sum(f[3]!=(72 if frame_cmd.lower() in 'db' else 16) or
                (frame_cmd.lower() in 'db' and bool(f[4]&1)!=(frame_cmd.lower()=='b')) for f in relevant)
            if before is not None:
                result['diag_delta']={k:after[k]-before[k] for k in ['accepted','rejected','completed']}
            if name in ('noack','noack_close'):
                if name=='noack_close':
                    raw.extend(write(b'C\r'));raw.extend(drain(.1))
                    raw.extend(write(b'O\r'));raw.extend(drain(.2))
                    result['after_close_open']=diagnostics()
                remote('linkup');raw.extend(drain(float(os.environ.get('DRAIN_SECONDS','2'))))
                resumed=remote('snapshot')['frames']
                result['after_ack_restored']={'diag':diagnostics(),'queue':buffer_stats(),'frames':resumed}
            (outdir/(name+'.json')).write_text(json.dumps({'result':result,'frames':frames,'serial_hex':raw.hex()},indent=2))
            print(json.dumps({k:v for k,v in result.items() if k!='missing'}|{'missing_count':len(result['missing'])}),flush=True)
            if name=='baseline' and len(got)!=n: raise RuntimeError('baseline failed; stop before bursts')
    finally:
        write(b'C\r');drain(.2);os.close(fd)
        p.stdin.write('quit\n');p.stdin.flush();p.wait(timeout=5)

if __name__=='__main__':main()
