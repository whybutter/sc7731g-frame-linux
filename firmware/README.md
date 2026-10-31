# firmware/

Vendor firmware for the frame, extracted from YOUR frame by
`./build.sh firmware DIR` (docs/firmware.md). Everything this step writes
here is gitignored; only these files are tracked:

- `extract.sh`: the extraction, run in the build container;
- `SHA256SUMS`: the known inputs (`in/...`) and outputs (`out/...`);
- `patches/`: binary patches to the WCN firmware, each checking its own
  input and output.
