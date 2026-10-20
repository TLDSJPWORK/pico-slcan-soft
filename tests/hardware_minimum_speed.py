"""Dedicated test bus only. Requires SERIAL_PORT, CAN_IF, SSH_TARGET, SSH_CONTROL_PATH.
OUTPUT_DIR selects result directory. Does not flash firmware or reboot Linux.
"""
import sys,os,time,json,subprocess,shlex,random
from pathlib import Path
from hardware_rx_duplex import Port
OUT=os.environ.get('OUTPUT_DIR','output/performance-V102')
Path(OUT).mkdir(parents=True,exist_ok=True)
REMOTE=Path(__file__).with_name('recovery_receiver.py').read_text().replace("layout='payload'","layout='minimum'")
REMOTE=REMOTE.replace("'can0'",repr(os.environ['CAN_IF']))
p=subprocess.Popen(['ssh','-S',os.environ['SSH_CONTROL_PATH'],os.environ['SSH_TARGET'],'sudo -n python3 -u -c '+shlex.quote(REMOTE)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
assert p.stdout.readline().strip()=='ready'
def snapshot():p.stdin.write('snapshot\n');p.stdin.flush();return json.loads(p.stdout.readline())['frames']
port=Port()
def cmd(c):port.raw.clear();port.write(c.encode()+b'\r');port.drain(.1);return bytes(port.raw).decode(errors='replace')
rows=[]
try:
 for trial,(rate,speed,kind) in enumerate([(500000,'S6','t'),(500000,'S6','b'),(1000000,'S8','t'),(1000000,'S8','b')]*3,1):
  cmd('C');subprocess.run(['ssh','-S',os.environ['SSH_CONTROL_PATH'],os.environ['SSH_TARGET'],f"sudo -n ip link set {os.environ['CAN_IF']} down && sudo -n ip link set {os.environ['CAN_IF']} up type can bitrate {rate} sample-point 0.8 sjw {4 if rate==1000000 else 8} dbitrate 2000000 dsample-point 0.8 dsjw 2 fd on"],check=True)
  for c in [speed,'Y2' if kind=='b' else 'Y0','Z0','O']:cmd(c)
  snapshot();ids=random.Random(20261019).choices(range(0x400,0x480),k=10000)
  port.raw.clear();start=time.monotonic();port.write(''.join(kind+f'{i:03X}0\r' for i in ids).encode());write_end=time.monotonic();port.drain(.1)
  while port.raw.count(13)+port.raw.count(7)<10000:
   if time.monotonic()-start>30:raise RuntimeError('command reply timeout')
   port.drain(.05)
  responses=bytes(port.raw);polls=[]
  while True:
   text=cmd('w');q={k:int(v) for k,v in (p.split('=') for p in text.strip()[2:].split())};polls.append(q)
   if not q['queued']:break
   if time.monotonic()-start>30:raise RuntimeError('drain timeout')
  frames=snapshot();replies=[c for c in responses if c in (13,7)];accepted=[i for i,c in zip(ids,replies) if c==13]
  delta=frames[-1][0]-frames[0][0];format_bad=sum(f[2]!=0 or f[3]!=(72 if kind=='b' else 16) or (kind=='b' and not f[4]&1) for f in frames)
  row=dict(trial=trial,peer_nominal_sjw=4 if rate==1000000 else 8,rate=rate,kind=kind,length=0,attempted=10000,accepted=responses.count(13),rejected=responses.count(7),received=len(frames),exact_ids_in_order=[f[1] for f in frames]==accepted,hardware_timestamps=all(f[6] for f in frames),socket_drops=max(f[5] for f in frames),format_bad=format_bad,write_seconds=write_end-start,peer_span_seconds=delta,saturated_frames_per_second=(len(frames)-1)/delta,final_queue=q)
  open(OUT+f'/minimum-frame-trial-{trial}.json','w').write(json.dumps({'summary':row,'frames':frames,'response_hex':responses.hex()},indent=2));rows.append(row);open(OUT+'/minimum-frame-speed.json','w').write(json.dumps(rows,indent=2));print(json.dumps(row),flush=True)
finally:
 cmd('C');os.close(port.fd);p.stdin.write('quit\n');p.stdin.flush();p.wait(timeout=5)
