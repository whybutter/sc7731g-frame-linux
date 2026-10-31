# Firmware: extracted from your frame, never shipped

The frame runs four pieces of vendor firmware under Linux (NOTICE says
whose they are and why none is in this repository). This is how you get
them from your own frame, how they are checked, and how a patch to one is
applied. The approach is Nexmon's: the repository holds the tools, the
checksums and the patches; the binaries come from the device you own.

| file | used by | made from | made by |
|---|---|---|---|
| `firmware/silead/gsl1680.fw` | touch (mainline `silead`), in the initramfs | the Android kernel in `boot.img` | `tools/extract-gsl-fw.py` |
| `firmware/wcnmodem.bin` | Wi-Fi and Bluetooth (cp2), in the kernel image | the `wcnmodem` partition, plus `firmware/patches/` | copied, then patched |
| `firmware/wifi-nvm.bin` | Wi-Fi calibration, in the kernel image | `/etc/connectivity_*.ini` in `system.img` | `tools/wcn-nvm.py` |
| PSKEY (144 bytes) | Bluetooth, written to `bt-sc7731g` at boot | `/etc/connectivity_configure.ini` + the frame's serial | `rootfs/usr/lib/sc7731g-bt/pskey.sh` on the frame |

The kernel build refuses to run without `gsl1680.fw` (a kernel without it
boots with no touch), and builds the Wi-Fi blobs into the kernel image when
they are there (`CONFIG_EXTRA_FIRMWARE`), so the A/B root slots can't lose
them.

## 1. Dump three partitions

You need root on the stock Android. On the VT10101 that means the UART
(docs/bring-up.md, "Getting in"); other SC7731G frames may allow adb. As
root, from a computer with adb:

```sh
mkdir backup
for p in boot wcnmodem system; do
  adb shell su -c "cat /dev/block/platform/*/by-name/$p" > backup/$p.img
done
```

(`adb pull /dev/block/platform/*/by-name/<name>` works too, and is
binary-safe on adb versions where `exec-out` isn't. The old toolbox `dd` on
this Android rejects `bs=1M` and large `skip=`.)

Take **a full backup** while you are there, every partition plus the GPT
(docs/install-plan.md): installing changes `boot`, `recovery` and
`userdata`, and the recovery image is your way back to Android. After the
install, Android's original `boot.img` lives in the `recovery` partition.

## 2. Extract

```sh
./build.sh firmware backup/
```

It runs `firmware/extract.sh` in the build container. Every input and
output is compared with `firmware/SHA256SUMS`, which holds what firmware
build `VTimes_VT10101_V01_20250617` gives:

- an input with a known checksum must give the known output, or the step
  fails;
- an unknown input (another firmware build, another frame model) is a
  warning: the outputs are made the same way but are unverified. Test them,
  and if they work, an issue with your build's fingerprint and checksums
  helps others.

## 3. Patches

A patch to `wcnmodem.bin` is a Python file in `firmware/patches/`, applied
in name order (`firmware/patches/README.md` has the format). Each one
checks the SHA-256 of what it is given and of what it makes, and refuses an
input it wasn't written for. None exists yet: the planned one is the Wi-Fi /
Bluetooth coexistence fix (docs/bluetooth.md, "Coexistence").

## Why not just ship them

Spreadtrum's WCN firmware and Silead's touch firmware are not
redistributable, and the calibration and PSKEY data are per model or per
unit (the PSKEY carries a Bluetooth address). Extracting from the frame you
own keeps all of that where it belongs.
