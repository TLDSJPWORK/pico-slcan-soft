"""Create an isolated TX fault-injection build; never edit production sources.

Run: python3 tests/prepare_recovery_diagnostics.py /absolute/new/source-directory
Then build that directory with the Pico SDK. Its x/j commands deliberately
corrupt controller bookkeeping and must only be used on a dedicated test bus.
"""
import argparse,difflib,hashlib,json,shutil
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('destination',type=Path)
parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parent.parent)
a=parser.parse_args();src=a.source.resolve();dest=a.destination.resolve()
if dest.exists():raise FileExistsError('Choose a new diagnostic source directory')
dest.mkdir(parents=True)
for name in ['CMakeLists.txt','pico-slcan.c','pico-slcan.h','pico_sdk_import.cmake','slcan','mcp25xxfd','pico-ssd1306','LICENSE']:
 s=src/name;d=dest/name
 if s.is_dir():shutil.copytree(s,d)
 else:shutil.copy2(s,d)
patch=[]
def edit(name,fn):
 p=dest/name;before=p.read_text();after=fn(before)
 if before==after:raise RuntimeError('Injection anchor missing: '+name)
 p.write_text(after);patch.extend(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile='a/'+name,tofile='b/'+name))

DIAG = r'''

// Diagnostics only: snapshots run on core0; never print inside the ISR.
void can_diag_print(can_controller_t *controller)
{
    can_interface_t *spi = &controller->host_interface;
    mcp25xxfd_spi_gpio_disable_irq(spi);
    uint32_t accepted = diag_accepted, rejected = diag_rejected, completed = diag_completed;
    uint32_t free_slots = controller->tx_pri_queue.num_free_slots, valid = 0;
    for (uint32_t i = 0; i < UREF_HASH_TABLE_SIZE; ++i) valid += controller->tx_pri_queue.uref_valid[i] ? 1U : 0U;
    uint32_t txqsta = read_word_crc(spi, C1TXQSTA);
    uint32_t tefsta = read_word_crc(spi, C1TEFSTA);
    uint32_t txreq = read_word_crc(spi, C1TXREQ);
    uint32_t trec = read_word_crc(spi, C1TREC);
    uint32_t bdiag = read_word_crc(spi, C1BDIAG1);
    uint32_t seq_bad = controller->target_specific.seq_bad;
    uint32_t addr_bad = controller->target_specific.txqua_bad;
    uint32_t hwfull = controller->target_specific.txqsta_bad;
    uint32_t bus_off = controller->target_specific.bus_off;
    uint32_t crc = spi->crc_errors;
    mcp25xxfd_spi_gpio_enable_irq(spi);
    printf("X accepted=%lu rejected=%lu completed=%lu free=%lu valid=%lu txqsta=%08lx tefsta=%08lx txreq=%08lx trec=%08lx bdiag=%08lx seq_bad=%lu addr_bad=%lu hwfull=%lu bus_off=%lu crc=%lu\r",
        (unsigned long)accepted, (unsigned long)rejected, (unsigned long)completed,
        (unsigned long)free_slots, (unsigned long)valid, (unsigned long)txqsta,
        (unsigned long)tefsta, (unsigned long)txreq, (unsigned long)trec,
        (unsigned long)bdiag, (unsigned long)seq_bad, (unsigned long)addr_bad,
        (unsigned long)hwfull, (unsigned long)bus_off, (unsigned long)crc);
}
'''
FAULT = r'''void can_fault(can_controller_t *controller, char mode) {
 can_interface_t *spi=&controller->host_interface;
 mcp25xxfd_spi_gpio_disable_irq(spi);
 if (mode=='s') fault_seq=true;
 if (mode=='v') for (uint32_t i=0;i<UREF_HASH_TABLE_SIZE;i++) controller->tx_pri_queue.uref_valid[i]=false;
 if (mode=='f') controller->tx_pri_queue.num_free_slots=0;
 if (mode=='o') {
  // Test-only: retain the normal 12-entry TEF. Preload 20 unique TX slots
  // without TXREQ, then release them together while IRQ servicing is paused.
  write_word(spi,C1TEFCON,FSIZE(CAN_HW_TEF_QUEUE_SIZE-1U)|TEFTSEN|TEFNEIE);
  can_frame_t frame={0};frame.canid.id=0x6EE;frame.dlc=1;frame.fd_data[0]=0xA5;
  fault_hold_tx=true;
  uint32_t submitted=0;
  for (uint32_t i=0;i<20;i++) {frame.canid.id=0x680+i;frame.fd_data[0]=i;submitted+=send_frame(controller,&frame,false);}
  fault_hold_tx=false;
  write_word(spi,C1TXQCON,TXREQ);
  busy_wait_us_32(100000);
  printf("J submitted=%lu\r",(unsigned long)submitted);
  uint32_t status=read_word_crc(spi,C1TEFSTA);
  printf("J tefsta=%08lx overflow=%u\r",(unsigned long)status,!!(status&TEFOVIF));
 }
 mcp25xxfd_spi_gpio_enable_irq(spi);
}
'''

def driver(s):
 pos=s.index('static bool TIME_CRITICAL send_frame(')
 s=s[:pos]+'static uint32_t diag_accepted, diag_rejected, diag_completed;\nstatic bool fault_seq, fault_hold_tx;\n\n'+s[pos:]
 needle='controller->tx_pri_queue.num_free_slots++;'
 assert s.count(needle)==1
 s=s.replace(needle,needle+'\n        diag_completed++;')
 needle='return queued ? CAN_ERC_NO_ERROR : CAN_ERC_NO_ROOM;'
 assert s.count(needle)==1
 s=s.replace(needle,'if (queued) diag_accepted++; else diag_rejected++;\n    '+needle)
 s=s.replace('uint8_t seq = (uint8_t)(details[0] >> 9);','uint8_t seq = (uint8_t)(details[0] >> 9);\n    if (fault_seq) { seq = 255; fault_seq = false; }')
 s=s.replace('write_word(spi_interface, C1TXQCON, UINC | TXREQ);','write_word(spi_interface, C1TXQCON, UINC | (fault_hold_tx ? 0U : TXREQ));')
 return '#include <stdio.h>\n'+s+DIAG+'\n'+FAULT
edit('mcp25xxfd/mcp25xxfd.c',driver)
edit('pico-slcan.c',lambda s:s+'\nextern void can_diag_print(can_controller_t *);\nextern void can_fault(can_controller_t *, char);\nvoid pico_slcan_diag(void) { if (mController_started) can_diag_print(&mController); else printf("X stopped\\r"); }\nvoid pico_slcan_fault(char mode) { if(mController_started) can_fault(&mController,mode); }\n')
edit('slcan/slcan.c',lambda s:s.replace('void pars_slcancmd(char *buf)','extern void pico_slcan_diag(void);\nextern void pico_slcan_fault(char);\n\nvoid pars_slcancmd(char *buf)').replace('switch (buf[0]) {',"switch (buf[0]) {\n    case 'x': pico_slcan_diag(); break;\n    case 'j': pico_slcan_fault(buf[1]); slcan_ack(); break;",1))
(dest/'diagnostics.patch').write_text(''.join(patch))
manifest={str(p.relative_to(dest)):hashlib.sha256(p.read_bytes()).hexdigest()for p in dest.rglob('*')if p.is_file()}
(dest/'source-sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(dest)
