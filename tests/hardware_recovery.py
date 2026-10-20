"""Dedicated-bus hardware recovery test; requires diagnostic firmware.

SERIAL_PORT and CAN_IF select the only devices operated. SSH_TARGET optionally
runs SocketCAN on another Linux host; SSH_CONTROL_PATH reuses an existing login.
The Linux host needs passwordless sudo for ip and the socket receiver (SUDO='' for
an already privileged shell). This test deliberately downs CAN_IF. REPEAT=10
runs each fault/wait condition ten times. No reboot or other CAN port is used.
"""
import sys,os,time,json,subprocess,shlex,select,struct,termios
from pathlib import Path
class BasePort:
    def __init__(self):
        self.fd=os.open(os.environ.get('SERIAL_PORT','/dev/ttyACM0'),os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
        a=termios.tcgetattr(self.fd);a[0]=a[1]=a[3]=0;a[2]=termios.CS8|termios.CREAD|termios.CLOCAL
        a[4]=a[5]=termios.B115200;a[6][termios.VMIN]=a[6][termios.VTIME]=0
        termios.tcsetattr(self.fd,termios.TCSANOW,a)
        self.raw=bytearray()
    def drain(self,seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if select.select([self.fd],[],[],min(.01,max(0,end-time.monotonic())))[0]:
                try:self.raw.extend(os.read(self.fd,65536))
                except BlockingIOError:pass
    def write(self,data):
        end=time.monotonic()+20
        while data:
            assert time.monotonic()<end,'USB write timeout'
            r,w,_=select.select([self.fd],[self.fd],[],.1)
            if r:
                try:self.raw.extend(os.read(self.fd,65536))
                except BlockingIOError:pass
            if w:
                try:data=data[os.write(self.fd,data):]
                except BlockingIOError:pass
    def cmd(self,c):
        self.write(c.encode()+b'\r');self.drain(.15)

OUT=Path(sys.argv[1]);OUT.mkdir(parents=True,exist_ok=True)
TARGET=os.environ.get('SSH_TARGET');IFACE=os.environ.get('CAN_IF','can0')
SUDO=os.environ.get('SUDO','sudo -n')
def linux(command):
 if not TARGET:return ['sh','-c',command]
 args=['ssh','-o','BatchMode=yes']
 if os.environ.get('SSH_CONTROL_PATH'):args+=['-S',os.environ['SSH_CONTROL_PATH']]
 return args+[TARGET,command]
class Port(BasePort):
 def write(self,data):
  end=time.monotonic()+10
  while data:
   if time.monotonic()>end:raise TimeoutError('serial write')
   r,_,_=select.select([self.fd],[],[],.0002)
   if r:
    try:self.raw.extend(os.read(self.fd,65536))
    except BlockingIOError:pass
   try:data=data[os.write(self.fd,data[:1024]):]
   except BlockingIOError:pass
code=Path(__file__).with_name('recovery_receiver.py').read_text().replace("('can0',)",'('+repr(IFACE)+',)')
code=code.replace('else ident==0x621','else ident in (0x621,0x5b0) or 0x680<=ident<0x694')
code=code.replace('drops,hw,user])','drops,hw,user,ident])')
p=subprocess.Popen(linux((SUDO+' ' if SUDO else '')+'python3 -u -c '+shlex.quote(code)),stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
assert p.stdout.readline().strip()=='ready'
def remote(c):
 p.stdin.write(c+'\n');p.stdin.flush()
 if not select.select([p.stdout],[],[],5)[0]:raise TimeoutError('receiver')
 return json.loads(p.stdout.readline())
def config(rate=500000,sp=.8,up=True):
 args=f'{SUDO} ip link set {shlex.quote(IFACE)} down'
 if up:args+=f' && {SUDO} ip link set {shlex.quote(IFACE)} up type can bitrate {rate} sample-point {sp} sjw 8 dbitrate 2000000 dsample-point 0.8 dsjw 2 fd on'
 subprocess.run(linux(args),check=True)
port=Port();rows=[]
def cmd(c,wait=.05):
 port.raw.clear();port.write(c.encode()+b'\r');port.drain(wait);return bytes(port.raw).decode(errors='replace')
def status():
 raw=cmd('w');line=next(x for x in raw.split('\r') if x.startswith('W '));return {k:int(v) for k,v in (x.split('=')for x in line[2:].split())}
def diag():return cmd('x').strip()
def setup(speed='S6',fd=False):
 for c in ['C',speed,'Y2' if fd else 'Y0','Z0','O']:
  assert cmd(c)=='\r',(c,bytes(port.raw))
 assert diag().startswith('X accepted='),'Diagnostic firmware required'
 assert 'recoveries' in status(),'TX recovery firmware required'
 remote('snapshot')
def send(tag,n=100):
 port.raw.clear();port.write(''.join(f't6218{tag:08X}{i:08X}\r' for i in range(n)).encode());port.drain(.15)
 raw=bytes(port.raw);return {'cr':raw.count(13),'bel':raw.count(7)}
def wait():
 end=time.monotonic()+5
 while True:
  w=status()
  if not w['queued'] and not w['pending']:return w
  if time.monotonic()>end:raise RuntimeError('stuck '+json.dumps(w)+' '+diag())
def verify(frames,tag,n,duplicates=False):
 relevant=[f for f in frames if f[8]==0x621 and f[1].startswith(f'{tag:08x}')]
 vals=[int(f[1][8:],16)for f in relevant]
 format_ok=all(f[2]==8 and f[3]==16 and f[5]==0 and f[6] for f in relevant)
 return dict(received=len(vals),unique=len(set(vals)),duplicates=len(vals)-len(set(vals)),sequence=vals,format_ok=format_ok,ok=format_ok and set(vals)==set(range(n)) and vals==sorted(vals) and (duplicates or len(vals)==n))
def save(row):
 rows.append(row);(OUT/os.environ.get('RESULT_FILE','recovery-results.json')).write_text(json.dumps(rows,indent=2));print(json.dumps(row),flush=True)
try:
 for run,mode in enumerate(['s','f','v','noack','busoff','o']*int(os.environ.get('REPEAT','1')),1):
  config();setup();before=status()
  if mode=='busoff':
   config(125000,.8);setup('S4',True);before=status();cmd('b5B00');acks=send(run,20);port.drain(.15);fault=status();faultdiag=diag();config(125000,.75);after=wait();frames=remote('snapshot')['frames'];check=verify(frames,run,20)
   ok=check['ok'] and after['recoveries']>before['recoveries'] and after['uncertain']>before['uncertain']
  elif mode=='o':
   injection=cmd('jo',.3);fault=status();faultdiag=diag();direct=remote('snapshot')['frames'];acks=send(run);after=wait();frames=remote('snapshot')['frames'];check=verify(frames,run,100)
   exact=[(f[8],f[1])for f in direct]==[(0x680+i,f'{i:02x}')for i in range(20)]
   ok=check['ok'] and exact and after['recoveries']==before['recoveries']+1
   faultdiag=injection+' '+faultdiag
  else:
   if mode in ['v','noack']:config(up=False)
   if mode in ['s','f']:cmd('j'+mode)
   acks=send(run)
   if mode=='v':cmd('jv')
   port.drain(.3);fault=status();faultdiag=diag()
   if mode in ['v','noack']:config()
   after=wait();frames=remote('snapshot')['frames'];check=verify(frames,run,100,mode=='s')
   ok=check['ok'] and (after['recoveries']==before['recoveries'] if mode=='noack' else after['recoveries']==before['recoveries']+1)
  enddiag=diag();ok=ok and acks['bel']==0 and acks['cr']==(20 if mode=='busoff'else 100) and 'free=20 valid=0 ' in enddiag and 'tefsta=00000000' in enddiag
  row=dict(mode=mode,before=before,fault=fault,fault_diag=faultdiag,after=after,final_diag=enddiag,acks=acks,check=check,pass_result=ok)
  save(row)
  assert ok,row
finally:
 cmd('C');os.close(port.fd);remote('quit') if False else None
 p.stdin.write('quit\n');p.stdin.flush();p.wait(timeout=5)
