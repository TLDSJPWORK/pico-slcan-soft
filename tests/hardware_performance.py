"""Dedicated test bus only. Requires SERIAL_PORT, CAN_IF, SSH_TARGET, SSH_CONTROL_PATH.
OUTPUT_DIR selects result directory. Does not flash firmware or reboot Linux.
"""
import sys,os,time,json,subprocess,shlex,select,struct,statistics
from pathlib import Path
from hardware_rx_duplex import Port
OUT=os.environ.get('OUTPUT_DIR','output/performance-V102')
Path(OUT).mkdir(parents=True,exist_ok=True)
BasePort=Port
class Port(BasePort):
 def write(self,data):
  end=time.monotonic()+120
  while data:
   if time.monotonic()>end:raise TimeoutError('USB write >120 seconds')
   r,w,_=select.select([self.fd],[],[],.0002)
   if r:
    try:self.raw.extend(os.read(self.fd,65536))
    except BlockingIOError:pass
   try:data=data[os.write(self.fd,data[:1024]):]
   except BlockingIOError:pass

REMOTE=Path(__file__).with_name('recovery_receiver.py').read_text()
REMOTE=REMOTE.replace("'can0'",repr(os.environ['CAN_IF']))
p=subprocess.Popen(['ssh','-S',os.environ['SSH_CONTROL_PATH'],os.environ['SSH_TARGET'],'sudo -n python3 -u -c '+shlex.quote(REMOTE)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
assert p.stdout.readline().strip()=='ready'
def remote(c):
 p.stdin.write(c+'\n');p.stdin.flush();return json.loads(p.stdout.readline())
port=Port()
def cmd(c):
 port.raw.clear();port.write(c.encode()+b'\r');port.drain(.1);return bytes(port.raw).decode(errors='replace')
def queue():
 text=cmd('w');return {k:int(v) for k,v in (x.split('=') for x in text.strip()[2:].split())}
def setup(speed,fd):
 for c in ['C',speed,'Y2' if fd else 'Y0','Z0','O']:cmd(c)
def config(rate):
 subprocess.run(['ssh','-S',os.environ['SSH_CONTROL_PATH'],os.environ['SSH_TARGET'],f"sudo -n ip link set {os.environ['CAN_IF']} down && sudo -n ip link set {os.environ['CAN_IF']} up type can bitrate {rate} sample-point 0.8 sjw {4 if rate==1000000 else 8} dbitrate 2000000 dsample-point 0.8 dsjw 2 fd on"],check=True)
def payload(i,n):return struct.pack('>I',i)+bytes((i+j)%256 for j in range(n-4))
def percentile(a,q):return sorted(a)[round((len(a)-1)*q)]
version=cmd('V');assert 'V0103' in version,version
(new_version_path:=Path(OUT+'/version.txt')).write_text(version)
rows=json.loads(Path(OUT+'/performance.json').read_text()) if Path(OUT+'/performance.json').exists() else []
try:
 for trial,(rate,speed,length,kind) in enumerate([(125000,'S4',8,'t'),(500000,'S6',8,'t'),(500000,'S6',64,'b'),(1000000,'S8',8,'t'),(1000000,'S8',64,'b')]*3,1):
  if any(r['trial']==trial for r in rows):continue
  config(rate);setup(speed,kind=='b');remote('noecho');remote('snapshot')
  sync=[]
  for _ in range(20):
   a=time.monotonic();r=remote('clock')['clock'];b=time.monotonic();sync.append((b-a,r-b,r-a))
  uncertainty,off_low,off_high=min(sync)
  expected=[payload(i,length).hex() for i in range(10000)]
  dlc='F' if length==64 else '8'
  commands=[(kind+'621'+dlc+d+'\r').encode() for d in expected]
  port.raw.clear();start=time.monotonic();port.write(b''.join(commands));write_end=time.monotonic();port.drain(.1)
  while bytes(port.raw).count(13)+bytes(port.raw).count(7)<10000:
   if time.monotonic()-start>150:raise RuntimeError('missing command replies')
   port.drain(.05)
  responses=bytes(port.raw);port.raw.clear()
  polls=[]
  while True:
   q=queue();polls.append(q)
   if not q['queued']:break
   if time.monotonic()-start>150:raise RuntimeError('FIFO stuck')
  frames=remote('snapshot')['frames'];reply=[c for c in responses if c in (13,7)]
  accepted=[d for d,c in zip(expected,reply) if c==13];data=[f[1] for f in frames]
  elapsed_bus=frames[-1][0]-frames[0][0] if len(frames)>1 else 0
  burst_start=time.monotonic();port.raw.clear();port.write(b''.join(commands[:1000]));port.drain(.1)
  while bytes(port.raw).count(13)+bytes(port.raw).count(7)<1000:
   if time.monotonic()-burst_start>15:raise RuntimeError('missing burst replies')
   port.drain(.05)
  burst_resp=bytes(port.raw)
  while queue()['queued']:pass
  burst=remote('snapshot')['frames'];last=burst[-1][0] if burst else 0
  remote('echo');latencies=[];latency_bad=0
  for i in range(100):
   packet=commands[i];expected_echo=(kind+'622'+dlc+expected[i]).upper().encode();port.raw.clear();t=time.monotonic();port.write(packet)
   end=t+2
   while expected_echo not in bytes(port.raw).upper() and time.monotonic()<end:
    if select.select([port.fd],[],[],.01)[0]:
     try:port.raw.extend(os.read(port.fd,65536))
     except BlockingIOError:pass
   if expected_echo in bytes(port.raw).upper():latencies.append((time.monotonic()-t)*1000)
   else:latency_bad+=1
  remote('noecho');remote('snapshot')
  row=dict(trial=trial,rate=rate,length=length,kind=kind,attempted=10000,ack_cr=responses.count(13),ack_bel=responses.count(7),response_count=len(reply),received=len(frames),accepted_data_exact=data==accepted,hardware_timestamps=all(f[6] for f in frames+burst),socket_drops=max(f[5] for f in frames+burst),write_seconds=write_end-start,peer_span_seconds=elapsed_bus,saturated_frames_per_second=(len(frames)-1)/elapsed_bus,peer_clock_uncertainty_ms=uncertainty*1000,burst1000_received=len(burst),burst1000_cr=burst_resp.count(13),burst1000_bel=burst_resp.count(7),burst1000_data_exact=[f[1] for f in burst]==expected[:1000],burst1000_last_delivery_ms_bounds=[(last-off_high-burst_start)*1000,(last-off_low-burst_start)*1000],rtt_samples=len(latencies),rtt_bad=latency_bad,rtt_ms=dict(min=min(latencies),median=statistics.median(latencies),p95=percentile(latencies,.95),p99=percentile(latencies,.99),max=max(latencies)),final_queue=q)
  open(OUT+f'/performance-trial-{trial}.json','w').write(json.dumps({'summary':row,'clock_sync':sync,'received_frames':frames,'response_hex':responses.hex(),'burst1000_frames':burst,'burst1000_response_hex':burst_resp.hex(),'roundtrip_ms':latencies},indent=2))
  rows.append(row);open(OUT+'/performance.json','w').write(json.dumps(rows,indent=2));print(json.dumps(row),flush=True)
finally:
 cmd('C');os.close(port.fd);p.stdin.write('quit\n');p.stdin.flush();p.wait(timeout=5)
