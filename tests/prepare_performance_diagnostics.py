"""Generate isolated FIFO-prefill diagnostics; never modify production sources."""
import difflib,hashlib,json,subprocess,sys
from pathlib import Path
if len(sys.argv)!=2:raise SystemExit('usage: prepare_performance_diagnostics.py NEW_DIRECTORY')
dest=Path(sys.argv[1]).resolve()
subprocess.run([sys.executable,str(Path(__file__).with_name('prepare_recovery_diagnostics.py')),str(dest)],check=True)
PREFILL = "// Test-only bulk producer: prepare the existing application FIFO without USB\n// parsing time, then return to the unmodified one-inflight TX service.\nvoid pico_slcan_prefill(char mode) {\n if (!mController_started || tx_count) return;\n uint32_t len=(mode=='F')?64U:((mode=='C')?8U:0U);\n for (uint32_t i=0;i<SLCAN_TX_BUFFER_SIZE;i++) {\n  can_frame_t f={0};f.canid.id=0x621;f.dlc=len==64?15:len;\n  if(mode=='F'||mode=='B')f.flags=CAN_FRAME_FLAG_FDF|CAN_FRAME_FLAG_BRS;\n  uint8_t *d=(uint8_t *)f.fd_data;\n  if(len>=4){d[0]=i>>24;d[1]=i>>16;d[2]=i>>8;d[3]=i;\n   for(uint32_t j=4;j<len;j++)d[j]=(i+j-4)&0xff;}\n  tx_buffer[i]=f;\n }\n tx_head=tx_tail=0;tx_count=SLCAN_TX_BUFFER_SIZE;\n tx_accepted+=SLCAN_TX_BUFFER_SIZE;\n if(tx_count>tx_high_water)tx_high_water=tx_count;\n}\n"
changes=[]
for name in ['pico-slcan.c','slcan/slcan.c']:
 p=dest/name;before=p.read_text()
 if name=='pico-slcan.c':after=before+'\n'+PREFILL
 else:
  after=before.replace('extern void pico_slcan_fault(char);','extern void pico_slcan_fault(char);\nextern void pico_slcan_prefill(char);')
  after=after.replace("case 'j': pico_slcan_fault(buf[1]);", "case 'j': if(buf[1]=='C'||buf[1]=='F'||buf[1]=='B'||buf[1]=='Z') pico_slcan_prefill(buf[1]); else pico_slcan_fault(buf[1]);")
  assert 'pico_slcan_prefill(buf[1])' in after
 p.write_text(after)
 changes.extend(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile='a/'+name,tofile='b/'+name))
(dest/'performance-injection.patch').write_text(''.join(changes))
files=['pico-slcan.c','pico-slcan.h','mcp25xxfd/mcp25xxfd.c','mcp25xxfd/mcp25xxfd-types.h','mcp25xxfd/canapi.h','slcan/slcan.c']
(dest/'performance-source-sha256.json').write_text(json.dumps({n:hashlib.sha256((dest/n).read_bytes()).hexdigest()for n in files},indent=2)+'\n')
