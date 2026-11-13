import serial,time,os,sys
lba=int(sys.argv[1]); out=sys.argv[2]
s=serial.Serial('/dev/cu.usbmodem1101',115200,timeout=0.3)
fd=os.open('/dev/rdisk4',os.O_RDONLY)
d=os.pread(fd,512,lba*512); print(lba,int.from_bytes(d[:4],'little'),flush=True)
time.sleep(0.3); s.reset_input_buffer(); s.write(b'R'); time.sleep(5)
open(out,'w').write(s.read(300000).decode(errors='replace'))
