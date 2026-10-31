#!/usr/bin/env python3
"""Wi-Fi user space for the bring-up initramfs, from Alpine armv7 packages.

  wifi-userland.py DEST       (in the build container; WIFI=1 in kernel.sh)

The initramfs is a static busybox and nothing else, and busybox has no
802.11 tools. wpa_supplicant and iw come from the same pinned Alpine branch
as that busybox (versions.env), with their shared libraries and musl's
loader, so they run as they would in the Alpine rootfs -- which is where
they end up anyway. Dependencies are resolved from APKINDEX ("so:" names to
the packages that provide them). Downloads are cached in /cache/dl/apk.

Firmware: only the files the chipset drivers ask for (kernel/
wifi.config), kept as Alpine ships them (.zst; the kernel decompresses),
plus wireless-regdb's signed regulatory.db, which cfg80211 verifies against
its built-in keys.
"""
import os
import shutil
import subprocess
import sys
import tarfile
import io
import urllib.request

MIRROR = os.environ.get("ALPINE_MIRROR", "https://dl-cdn.alpinelinux.org/alpine")
BRANCH = os.environ.get("ALPINE_BRANCH", "v3.22")
REPO = f"{MIRROR}/{BRANCH}/main/armv7"
CACHE = "/cache/dl/apk"

WANT = ["musl", "wpa_supplicant", "iw"]
FIRMWARE_PKGS = ["linux-firmware-rtlwifi", "linux-firmware-mediatek",
                 "linux-firmware-other", "linux-firmware-ath9k_htc",
                 "wireless-regdb"]
# (path in the package, path under DEST/lib/firmware); symlinks in the
# packages are taken as their targets
FIRMWARE = [
    # our TL-WN821N, an RTL8192CU (rtl8xxxu picks one by chip cut)
    ("lib/firmware/rtlwifi/rtl8192cufw.bin.zst", "rtlwifi/rtl8192cufw.bin.zst"),
    ("lib/firmware/rtlwifi/rtl8192cufw_A.bin.zst", "rtlwifi/rtl8192cufw_A.bin.zst"),
    ("lib/firmware/rtlwifi/rtl8192cufw_B.bin.zst", "rtlwifi/rtl8192cufw_B.bin.zst"),
    ("lib/firmware/rtlwifi/rtl8192cufw_TMSC.bin.zst", "rtlwifi/rtl8192cufw_TMSC.bin.zst"),
    ("lib/firmware/mediatek/mt7601u.bin.zst", "mt7601u.bin.zst"),
    ("lib/firmware/rt2870.bin.zst", "rt2870.bin.zst"),
    # ath9k_htc: the AR7010 the TL-WN821N v4 label promised, and the AR9271
    ("lib/firmware/ath9k_htc/htc_7010-1.4.0.fw.zst", "ath9k_htc/htc_7010-1.4.0.fw.zst"),
    ("lib/firmware/ath9k_htc/htc_9271-1.4.0.fw.zst", "ath9k_htc/htc_9271-1.4.0.fw.zst"),
    ("lib/firmware/regulatory.db", "regulatory.db"),
    ("lib/firmware/regulatory.db.p7s", "regulatory.db.p7s"),
]
# what of the user-space packages goes in: programs and libraries only
KEEP = ("lib/", "usr/lib/", "sbin/", "usr/sbin/", "bin/", "usr/bin/")
SKIP = ("usr/share/", "usr/lib/pkgconfig/", "lib/apk/", "etc/")


def index():
    raw = urllib.request.urlopen(f"{REPO}/APKINDEX.tar.gz").read()
    with tarfile.open(fileobj=io.BytesIO(raw)) as t:
        text = t.extractfile("APKINDEX").read().decode()
    pkgs, provides = {}, {}
    for rec in text.split("\n\n"):
        f = {}
        for line in rec.splitlines():
            if len(line) > 2 and line[1] == ":":
                f.setdefault(line[0], []).append(line[2:])
        if "P" not in f:
            continue
        name = f["P"][0]
        pkgs[name] = f
        for p in [name] + " ".join(f.get("p", [])).split():
            provides.setdefault(p.split("=")[0], name)
    return pkgs, provides


def resolve(pkgs, provides, want):
    out, todo = [], list(want)
    while todo:
        name = provides.get(todo.pop(0).split("=")[0].lstrip("!"))
        if name is None or name in out:
            continue
        out.append(name)
        todo += " ".join(pkgs[name].get("D", [])).split()
    return out


def fetch(pkgs, name):
    apk = f"{CACHE}/{name}-{pkgs[name]['V'][0]}.apk"
    if not os.path.exists(apk):
        os.makedirs(CACHE, exist_ok=True)
        urllib.request.urlretrieve(f"{REPO}/{os.path.basename(apk)}", apk + ".part")
        os.rename(apk + ".part", apk)
    return apk


def unpack(apk, into):
    os.makedirs(into, exist_ok=True)
    # an apk is three concatenated gzip streams; GNU tar reads them as one
    subprocess.run(["tar", "-xzf", apk, "-C", into, "--warning=no-unknown-keyword",
                    "--exclude=.PKGINFO", "--exclude=.SIGN.*", "--exclude=.pre-*",
                    "--exclude=.post-*", "--exclude=.trigger"],
                   check=True, stderr=subprocess.DEVNULL)


def main():
    dest = sys.argv[1]
    pkgs, provides = index()
    stage = "/tmp/wifi-userland"
    shutil.rmtree(stage, ignore_errors=True)
    names = resolve(pkgs, provides, WANT)
    for n in names:
        unpack(fetch(pkgs, n), f"{stage}/user")
    for n in FIRMWARE_PKGS:
        unpack(fetch(pkgs, n), f"{stage}/fw")
    root = f"{stage}/user"
    for dirpath, _, files in os.walk(root):
        for fn in files:
            src = os.path.join(dirpath, fn)
            rel = os.path.relpath(src, root)
            if not rel.startswith(KEEP) or rel.startswith(SKIP):
                continue
            dst = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            # links become copies: kernel.sh's initramfs list has only
            # files and dirs, and an absolute link would be read on the
            # build host
            while os.path.islink(src):
                t = os.readlink(src)
                src = os.path.join(root, t.lstrip("/")) if t.startswith("/") \
                    else os.path.join(os.path.dirname(src), t)
            if os.path.exists(src):
                shutil.copy2(src, dst)
    for src, dst in FIRMWARE:
        d = os.path.join(dest, "lib/firmware", dst)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(os.path.realpath(os.path.join(stage, "fw", src)), d)
    print("wifi userland: " + " ".join(f"{n}-{pkgs[n]['V'][0]}" for n in names))


if __name__ == "__main__":
    main()
