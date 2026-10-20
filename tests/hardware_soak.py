"""Dedicated test bus only. Requires SERIAL_PORT, CAN_IF, SSH_TARGET, SSH_CONTROL_PATH.
OUTPUT_DIR selects result directory. Does not flash firmware or reboot Linux.
"""
import sys,os,time,json,subprocess,shlex,select,struct
from pathlib import Path
from hardware_rx_duplex import Port
OUT=os.environ.get('OUTPUT_DIR','output/performance-V102')
Path(OUT).mkdir(parents=True,exist_ok=True)
DURATION=int(os.environ.get('SOAK_SECONDS','86400'));RATE=int(os.environ.get('SOAK_RATE','200'))
RATE_RX=int(os.environ.get('SOAK_RX_RATE',str(RATE)))
assert 0<RATE<=10000 and 0<RATE_RX<=10000 and 0<DURATION<=604800
REMOTE=r'''
import socket,time,select,sys,json,struct
from pathlib import Path
def kernel_rx_stats():
 return {name:int((Path('/sys/class/net/rcan2/statistics')/name).read_text()) for name in ['rx_errors','rx_dropped','rx_fifo_errors','rx_missed_errors','rx_over_errors']}
kernel_baseline=kernel_rx_stats()
def kernel_rx_delta():return {name:value-kernel_baseline[name] for name,value in kernel_rx_stats().items()}
s=socket.socket(socket.PF_CAN,socket.SOCK_RAW,socket.CAN_RAW);s.setsockopt(socket.SOL_CAN_RAW,socket.CAN_RAW_FD_FRAMES,1);s.setsockopt(socket.SOL_SOCKET,33,8*1024*1024);s.setsockopt(socket.SOL_SOCKET,40,1);s.bind(('rcan2',));s.setblocking(False)
n=0;rx=0;bad=0;prev=-1;start=None;end=None;send_errors=0;drops=0
print('ready',flush=True)
def payload(i):return struct.pack('>I',i)+bytes((i+j)%256 for j in range(60))
while True:
 now=time.monotonic()
 if start is not None and n< int(DURATION*RATE) and now>=start+n/RATE:
  for k in range(min(10,int((now-start)*RATE)+1-n,int(DURATION*RATE)-n)):
   try:s.send(struct.pack('=IBBBB64s',0x622,64,1,0,0,payload(n)));n+=1
   except OSError:send_errors+=1;break
 for f in select.select([s,sys.stdin],[],[],.001)[0]:
  if f is s:
   while True:
    try:
     b,anc,flags,addr=s.recvmsg(72,128)
     for level,typ,data in anc:
      if level==socket.SOL_SOCKET and typ==40:drops=max(drops,struct.unpack_from("=I",data)[0])
    except BlockingIOError:break
    ident=struct.unpack_from('=I',b)[0]
    if ident!=0x621:continue
    data=b[8:72];i=struct.unpack_from('>I',data)[0]
    if i!=prev+1 or b[4]!=64 or not b[5]&1 or data!=payload(i):bad+=1
    prev=i;rx+=1
  else:
   c=sys.stdin.readline().strip()
   if not c or c=='quit':sys.exit()
   if c=='start':start=time.monotonic()
   print(json.dumps(dict(sent=n,received=rx,bad=bad,last=prev,send_errors=send_errors,drops=drops,elapsed=0 if start is None else time.monotonic()-start,kernel_rx_errors=kernel_rx_delta())),flush=True)
'''.replace('DURATION',str(DURATION)).replace('RATE',str(RATE_RX))
REMOTE=REMOTE.replace("'rcan2'",repr(os.environ['CAN_IF'])).replace("/sys/class/net/rcan2/", "/sys/class/net/"+os.environ['CAN_IF']+"/")
p=subprocess.Popen(['ssh','-S',os.environ['SSH_CONTROL_PATH'],os.environ['SSH_TARGET'],'sudo -n python3 -u -c '+shlex.quote(REMOTE)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
assert p.stdout.readline().strip()=='ready'
def remote(c):
 p.stdin.write(c+'\n');p.stdin.flush()
 if not select.select([p.stdout],[],[],10)[0]:raise TimeoutError('peer response')
 return json.loads(p.stdout.readline())
port=Port();pending=b'';received=0;bad=0;acks=0;bels=0;last=-1;statuses=[]
def consume():
 global pending,received,bad,acks,bels,last
 pending+=bytes(port.raw);port.raw.clear();parts=pending.split(b'\r');pending=parts.pop()
 for line in parts:
  if not line:acks+=1;continue
  if b'\x07' in line:bels+=line.count(7);line=line.replace(b'\x07',b'')
  if line.startswith(b'b622F'):
   try:
    data=bytes.fromhex(line[5:].decode());i=struct.unpack_from('>I',data)[0]
    if i!=last+1 or data!=payload(i):bad+=1
    last=i;received+=1
   except Exception:bad+=1
  elif line.startswith(b'W '):statuses.append(line.decode())
  elif line:bad+=1

def payload(i):return struct.pack('>I',i)+bytes((i+j)%256 for j in range(60))
def command(c):port.write(c.encode()+b'\r');port.drain(.15);port.raw.clear()
rows=[]
Path(OUT+'/soak-run.json').write_text(json.dumps(dict(pid=os.getpid(),started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),duration_seconds=DURATION,rate_per_direction=RATE if RATE==RATE_RX else None,rate_tx_fps=RATE,rate_rx_fps=RATE_RX,firmware='V0103')))
def stats(line):return {k:int(v) for k,v in (x.split('=') for x in line[2:].split())}
try:
 for c in ['C','S6','Y2','Z0','O']:command(c)
 port.write(b'V\r');port.drain(.2);assert b'V0103\r' in port.raw;port.raw.clear()
 port.write(b'w\r');port.drain(.2);consume();baseline=stats(statuses[-1]);acks=0
 remote('start');start=time.monotonic();sent=0;checkpoint=start;health=start
 while time.monotonic()-start<DURATION:
  now=time.monotonic();target=min(DURATION*RATE,int((now-start)*RATE)+1)
  if target>sent:
   end=min(target,sent+10)
   port.write(b''.join(b'b621F'+payload(i).hex().encode()+b'\r' for i in range(sent,end)));sent=end
  port.drain(.001);consume()
  if now-health>=5:
   assert sent>=target-2*RATE,('TX producer cannot sustain requested rate',sent,target)
   port.write(b'w\r');health=now
  if now-checkpoint>=30:
   peer=remote('snapshot');row=dict(elapsed=now-start,sent=sent,received=received,bad=bad,acks=acks,bels=bels,peer=peer)
   row['status']=statuses[-1] if statuses else None
   assert bad==bels==peer['bad']==peer['send_errors']==peer['drops']==0 and all(value==0 for value in peer['kernel_rx_errors'].values()),row
   assert peer['sent']>=min(DURATION*RATE_RX,int(peer['elapsed']*RATE_RX))-2*RATE_RX,('Peer producer behind',row)
   assert stats(row['status'])['recoveries']==baseline['recoveries'],row
   rows.append(row);statuses[:]=statuses[-1:];open(OUT+'/soak-progress.json','w').write(json.dumps(rows,indent=2));print(json.dumps(row),flush=True);checkpoint=now
 while sent<DURATION*RATE:
  port.write(b'b621F'+payload(sent).hex().encode()+b'\r');sent+=1;port.drain(.001);consume()
 port.drain(3);consume();port.write(b'w\r');port.drain(.2);consume();peer=remote('snapshot')
 result=dict(duration_seconds=DURATION,rate_per_direction=RATE if RATE==RATE_RX else None,rate_tx_fps=RATE,rate_rx_fps=RATE_RX,sent=sent,received=received,bad=bad,acks=acks,bels=bels,peer=peer,statuses=statuses,baseline=baseline,elapsed=time.monotonic()-start)
 result['pass']=received==peer['sent']==DURATION*RATE_RX and sent==peer['received']==DURATION*RATE and bad==bels==peer['bad']==peer['send_errors']==0 and acks==sent and peer['drops']==0 and all(value==0 for value in peer['kernel_rx_errors'].values()) and stats(statuses[-1])['queued']==0 and stats(statuses[-1])['pending']==0 and stats(statuses[-1])['recoveries']==baseline['recoveries']
 open(OUT+'/soak.json','w').write(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
finally:
 command('C');os.close(port.fd);p.stdin.write('quit\n');p.stdin.flush();p.wait(timeout=5)
 subprocess.run(['ssh','-S',os.environ['SSH_CONTROL_PATH'],os.environ['SSH_TARGET'],f"sudo -n ip link set {os.environ['CAN_IF']} down"],check=True)
