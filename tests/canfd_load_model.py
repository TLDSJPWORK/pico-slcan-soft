"""ISO CAN FD standard-ID/BRS wire-time estimate, including intermission.
Field sizes: https://raw.githubusercontent.com/torvalds/linux/master/include/linux/can/length.h
Not an oscilloscope measurement. No error/overload frames assumed.
"""
import struct,json

def bits(value,n):return [(value>>i)&1 for i in range(n-1,-1,-1)]
def payload(i):return struct.pack('>I',i)+bytes((i+j)%256 for j in range(60))
def frame_us(ident,data,nominal=500000,fast=2000000,nom_sp=.75,data_sp=.8):
 # SOF + ID + RRS + IDE + FDF + res + BRS + ESI + DLC + data.
 stream=[0]+bits(ident,11)+[0,0,1,0,1,0]+bits(15,4)
 for byte in data:stream.extend(bits(byte,8))
 assert len(stream)==534
 previous=None;run=0;nom_stuff=0;fast_stuff=0
 for index,bit in enumerate(stream):
  run=run+1 if bit==previous else 1;previous=bit
  if run==5:
   if index<16:nom_stuff+=1
   else:fast_stuff+=1
   previous=1-bit;run=1
 # Nominal: 16 pre-BRS, fractional BRS/CRC-delimiter, 9 footer + 3 IFS.
 # Fast: ESI+DLC+64B + SBC/CRC21/fixed stuffing (4+21+7).
 nom_bits=16+nom_sp+(1-data_sp)+12+nom_stuff
 fast_bits=(1-nom_sp)+1+4+512+32+data_sp+fast_stuff
 return 1e6*(nom_bits/nominal+fast_bits/fast)
if __name__=='__main__':
 rows=[]
 for ident in [0x621,0x622]:
  times=[frame_us(ident,payload(i)) for i in range(65536)]
  rows.append(dict(id=hex(ident),samples=len(times),mean_us=sum(times)/len(times),min_us=min(times),max_us=max(times)))
 mean=sum(x['mean_us'] for x in rows)
 print(json.dumps(dict(method='protocol model; not measured waveform',frames=rows,bus_percent_at_200_each=mean*200/10000,bus_percent_at_750_each=mean*750/10000),indent=2))
