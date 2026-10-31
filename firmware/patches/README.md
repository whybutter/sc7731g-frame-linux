# firmware/patches/

Patches to the WCN co-processor firmware (`wcnmodem.bin`), applied by
`firmware/extract.sh` in name order (`10-name.py`, `20-name.py`, ...).

A patch is a Python 3 script run as `patch.py IN OUT`. It must:

1. refuse unless `sha256(IN)` is the input it was written for (the output
   of the previous patch, or `wcnmodem.bin` as extracted);
2. change only what it documents: each site with its address in cp2's
   address space (the image is linked at 0x8a808000), the bytes expected
   there before, and the bytes written;
3. refuse unless `sha256(OUT)` is the output it promises.

Nothing vendor goes in the script beyond the few bytes it replaces and
expects. None exists yet; docs/bluetooth.md ("Coexistence") describes the
first one planned.
