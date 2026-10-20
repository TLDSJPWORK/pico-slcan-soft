# Executed on Linux as a socket-local privileged receiver. No sysctl changes.
import socket,time,select,sys,json,struct
s=socket.socket(socket.PF_CAN,socket.SOCK_RAW,socket.CAN_RAW)
s.setsockopt(socket.SOL_CAN_RAW,socket.CAN_RAW_FD_FRAMES,1)
s.setsockopt(socket.SOL_SOCKET,33,8*1024*1024) # SO_RCVBUFFORCE, this socket only
s.setsockopt(socket.SOL_SOCKET,37,4|8|16|64) # RX hardware/software + SW/raw HW reporting
s.setsockopt(socket.SOL_SOCKET,40,1) # SO_RXQ_OVFL
s.bind(('can0',));s.setblocking(False)
frames=[];echo=False;drops=0;hw_count=0;fallback_count=0
layout='payload'
print('ready',flush=True)
def info():return dict(rcvbuf=s.getsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF),drops=drops,hardware_timestamps=hw_count,fallback_timestamps=fallback_count)
while True:
 for f in select.select([s,sys.stdin],[],[],1)[0]:
  if f is s:
   while True:
    try:b,anc,flags,addr=s.recvmsg(72,256)
    except BlockingIOError:break
    except OSError as e:
     if e.errno==100:break
     raise
    ident=struct.unpack_from('=I',b)[0]
    if not ((0x400<=ident<0x480) if layout=='minimum' else ident==0x621):continue
    stamp=None;hw=False;user=time.time()
    for level,typ,data in anc:
     if level!=socket.SOL_SOCKET:continue
     if typ==40:drops=max(drops,struct.unpack_from('=I',data)[0])
     if typ==37 and len(data)>=24:
      sec,nsec,_,__,rawsec,rawnsec=struct.unpack_from('=6q' if len(data)>=48 else '=6i',data)
      if rawsec or rawnsec:stamp=rawsec+rawnsec/1e9;hw=True
      elif sec or nsec:stamp=sec+nsec/1e9
    if stamp is None:stamp=user
    if hw:hw_count+=1
    else:fallback_count+=1
    value=ident if layout=='minimum' else b[8:8+b[4]].hex()
    frames.append([stamp,value,b[4],len(b),b[5],drops,hw,user])
    if echo:
     a=bytearray(b);struct.pack_into('=I',a,0,0x622);s.send(a)
  else:
   c=sys.stdin.readline().strip()
   if not c or c=='quit':sys.exit()
   if c=='echo':echo=True
   if c=='noecho':echo=False
   if c=='snapshot':print(json.dumps({'frames':frames,'receiver':info()}),flush=True);frames=[]
   else:print(json.dumps({'clock':time.time(),'receiver':info()}),flush=True)
