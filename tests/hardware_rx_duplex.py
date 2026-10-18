"""Actual RX matrix and concurrent TX/RX verification via stdlib SocketCAN."""
import base64, json, os, select, shlex, struct, subprocess, sys, termios, time, zlib
from pathlib import Path

class Port:
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

def make_cases():
    cases=[]
    lengths=[0,1,8,12,16,20,24,32,48,64]
    for fd in [False,True]:
        for ext in [False,True]:
            for special in [False,True]:  # RTR for classic, BRS for FD
                for length in (lengths if fd else [0,1,8]):
                    i=len(cases);ident=(0x123000 if ext else 0x600)+i
                    data=bytes((i+j)%256 for j in range(length))
                    cmd=('b' if special else 'd') if fd else ('r' if special else 't')
                    if ext:cmd=cmd.upper()
                    # Numeric DLC mapping, including 8 -> 8 and 12 -> 9.
                    dlc=([0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64].index(length))
                    expected=cmd+format(ident,'08X' if ext else '03X')+format(dlc,'X')
                    if fd or not special:expected+=data.hex().upper()
                    canid=ident | (0x80000000 if ext else 0) | (0x40000000 if special and not fd else 0)
                    packet=struct.pack('=IBBBB64s',canid,length,int(special),0,0,data) if fd else struct.pack('=IB3x8s',canid,length,data)
                    cases.append({'packet':packet.hex(),'expected':expected})
    return cases

REMOTE=r'''
import socket,select,sys,json,threading,time,errno,base64,zlib
s=socket.socket(socket.PF_CAN,socket.SOCK_RAW,socket.CAN_RAW)
s.setsockopt(socket.SOL_CAN_RAW,socket.CAN_RAW_FD_FRAMES,1)
s.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1048576)
s.bind(('can0',));s.setblocking(False)
spec=SPEC
sent=[];received=[];errors=[]
def producer():
 try:
  for i,h in enumerate(spec['packets']):
   deadline=time.monotonic()+10
   while True:
    try:s.send(bytes.fromhex(h));break
    except OSError as e:
     if e.errno not in (errno.ENOBUFS,errno.EAGAIN):raise
     if time.monotonic()>deadline:raise TimeoutError('CAN TX stalled')
     time.sleep(.001)
   sent.append(i);time.sleep(spec['gap'])
 except Exception as e:errors.append(repr(e))
print(json.dumps({'ready':True}),flush=True)
t=threading.Thread(target=producer);t.start()
while True:
 for f in select.select([s,sys.stdin],[],[],.1)[0]:
  if f is s:
   while True:
    try:b=s.recv(72)
    except BlockingIOError:break
    received.append(b.hex())
  else:
   cmd=sys.stdin.readline()
   t.join(timeout=12)
   end=time.monotonic()+.3
   while time.monotonic()<end:
    if select.select([s],[],[],.01)[0]:received.append(s.recv(72).hex())
   print(json.dumps({'sent':sent,'received':received,'errors':errors,'producer_alive':t.is_alive()}),flush=True)
   sys.exit(0)
'''

def main():
    mode=sys.argv[1];out=Path(sys.argv[2]);out.parent.mkdir(parents=True,exist_ok=True)
    port=Port();p=None;result={}
    try:
        for c in ['C','S6','Y2','Z0','O','V','N']:port.cmd(c)
        setup=bytes(port.raw);port.raw.clear()
        if mode=='matrix':
            cases=make_cases();spec={'packets':[c['packet'] for c in cases],'gap':.01};expected=[c['expected'] for c in cases];tx=[]
        else:
            n=1000
            data=[('beef0001'+format(i,'08x')).ljust(128,'a') for i in range(n)]
            spec={'packets':[struct.pack('=IBBBB64s',0x6a6,64,1,0,0,bytes.fromhex(d)).hex() for d in data],'gap':.002}
            expected=['b6A6F'+d.upper() for d in data]
            txdata=[('cafe0001'+format(i,'08x')).ljust(128,'a') for i in range(n)]
            tx=[('b5A5F'+d+'\r').encode() for d in txdata]
        packed=base64.b64encode(zlib.compress(json.dumps(spec).encode())).decode()
        code=REMOTE.replace('SPEC',"json.loads(zlib.decompress(base64.b64decode("+repr(packed)+")))",1)
        code=code.replace("'can0'",repr(os.environ.get('CAN_IF','can0')))
        target=os.environ.get('SSH_TARGET')
        ssh_args=['ssh','-o','BatchMode=yes']
        if os.environ.get('SSH_CONTROL_PATH'): ssh_args+=['-S',os.environ['SSH_CONTROL_PATH']]
        args=ssh_args+[target,'python3 -u -c '+shlex.quote(code)] if target else [sys.executable,'-u','-c',code]
        p=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
        assert select.select([p.stdout],[],[],8)[0] and json.loads(p.stdout.readline())['ready']
        if tx:port.write(b''.join(tx))
        port.drain(2 if mode=='matrix' else 6)
        p.stdin.write('finish\n');p.stdin.flush()
        end=time.monotonic()+15
        while not select.select([p.stdout],[],[],0)[0]:
            assert time.monotonic()<end,'Remote deadline'
            port.drain(.05)
        remote=json.loads(p.stdout.readline());port.drain(.5)
        raw=bytes(port.raw);lines=[s.decode() for s in raw.split(b'\r') if s]
        empty_cr=sum(not s for s in raw.split(b'\r')[:-1])
        rx_ok=lines==expected
        peer=[]
        for h in remote['received']:
            b=bytes.fromhex(h);ident=struct.unpack_from('=I',b)[0]
            if ident==0x5a5:peer.append(b[8:8+b[4]].hex())
        tx_ok=peer==txdata if mode!='matrix' else not peer
        result={'mode':mode,'pass':rx_ok and tx_ok and not remote['errors'] and not remote['producer_alive'] and len(remote['sent'])==len(expected) and empty_cr==len(tx),
                'expected_rx':len(expected),'received_usb':len(lines),'received_pi':len(peer),'acks':empty_cr,'rx_exact_order':rx_ok,'tx_exact_order':tx_ok,
                'setup_hex':setup.hex(),'usb_lines':lines,'remote':remote}
        out.write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps({k:v for k,v in result.items() if k not in ['setup_hex','usb_lines','remote']}),flush=True)
        assert result['pass'],'RX/duplex mismatch; inspect artifact'
    finally:
        port.cmd('C');os.close(port.fd)
        if p is not None:
            try:p.wait(timeout=3)
            except subprocess.TimeoutExpired:p.terminate()

if __name__=='__main__':main()
