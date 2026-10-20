"""Dedicated diagnostic firmware only; FIFO prefill via jC/jF/jZ/jB.
Requires SERIAL_PORT, CAN_IF, SSH_TARGET, SSH_CONTROL_PATH; OUTPUT_DIR optional.
"""
import sys,os,time,json,subprocess,shlex,ast,struct
from pathlib import Path
from hardware_rx_duplex import Port
OUT=Path(os.environ.get('OUTPUT_DIR','output/performance-V102'));OUT.mkdir(parents=True,exist_ok=True)
REMOTE=Path(__file__).with_name('recovery_receiver.py').read_text().replace("'can0'",repr(os.environ['CAN_IF']))
p=subprocess.Popen(['ssh','-S',os.environ['SSH_CONTROL_PATH'],os.environ['SSH_TARGET'],'sudo -n python3 -u -c '+shlex.quote(REMOTE)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
assert p.stdout.readline().strip()=='ready'
def remote(c):p.stdin.write(c+'\n');p.stdin.flush();return json.loads(p.stdout.readline())
port=Port()
def cmd(c):port.raw.clear();port.write(c.encode()+b'\r');port.drain(.1);return bytes(port.raw).decode(errors='replace')
def payload(i,n):return struct.pack('>I',i)+bytes((i+j)%256 for j in range(n-4)) if n else b''
rows=[]
try:
 for trial,(rate,speed,mode,length) in enumerate([(125000,'S4','C',8),(500000,'S6','C',8),(500000,'S6','F',64),(500000,'S6','Z',0),(500000,'S6','B',0),(1000000,'S8','Z',0),(1000000,'S8','B',0)]*3,1):
  cmd('C');subprocess.run(['ssh','-S',os.environ['SSH_CONTROL_PATH'],os.environ['SSH_TARGET'],f"sudo -n ip link set {os.environ['CAN_IF']} down && sudo -n ip link set {os.environ['CAN_IF']} up type can bitrate {rate} sample-point 0.8 sjw {4 if rate==1000000 else 8} dbitrate 2000000 dsample-point 0.8 dsjw 2 fd on"],check=True)
  for c in [speed,'Y2' if mode in 'FB' else 'Y0','Z0','O']:cmd(c)
  remote('snapshot');before=cmd('w');ack=cmd('j'+mode);start=time.monotonic()
  while True:
   status=cmd('w');q={k:int(v) for k,v in (p.split('=') for p in status.strip()[2:].split())}
   if not q['queued']:break
   if time.monotonic()-start>10:raise RuntimeError('internal FIFO timeout')
  frames=remote('snapshot')['frames'];span=frames[-1][0]-frames[0][0]
  format_bad=sum(f[2]!=length or f[3]!=(72 if mode in 'FB' else 16) or (mode in 'FB' and not f[4]&1) for f in frames)
  row=dict(trial=trial,rate=rate,mode=mode,length=length,received=len(frames),expected=1000,hardware_timestamps=all(f[6] for f in frames),socket_drops=max(f[5] for f in frames),exact_data_in_order=[f[1] for f in frames]==[payload(i,length).hex() for i in range(1000)],order_observable=length>0,format_bad=format_bad,peer_span_seconds=span,frames_per_second=(len(frames)-1)/span,queue=q,ack=ack,driver=cmd('x'))
  rows.append(row);(OUT/f'internal-trial-{trial}.json').write_text(json.dumps({'summary':row,'frames':frames,'before':before},indent=2));(OUT/'internal-speed.json').write_text(json.dumps(rows,indent=2));print(json.dumps(row),flush=True)
finally:
 cmd('C');os.close(port.fd);p.stdin.write('quit\n');p.stdin.flush();p.wait(timeout=5)
