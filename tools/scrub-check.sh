#!/bin/sh
# scrub-check.sh -- fails if the tree holds anything private: addresses,
# MACs, serial numbers, host and network names, credentials, personal paths,
# names of the people and tools behind the work, or vendor firmware. Run it
# before every commit that will be published:
#
#   tools/scrub-check.sh            the files git would publish (tracked + new)
#   tools/scrub-check.sh --history  also every commit's author and message
#
# A hit prints the rule, then file:line:text, and the run exits 1. The few
# deliberate exceptions are in ALLOW, each with why: an exception matches a
# piece of text, never a whole file. Python's re, so it runs the same on
# macOS and Linux (BSD grep has no -P).
cd "$(dirname "$0")/.." || exit 1
exec python3 - "$@" <<'PY'
import re, subprocess, sys

RULES = [
    ("IPv4: private, loopback or CGNAT/tailnet",
     r"\b(?:10|127|172\.(?:1[6-9]|2\d|3[01])|192\.168|100\.(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7]))\.\d{1,3}\.\d{1,3}\b"),
    ("IPv4 in a URL or ssh target", r"(?:https?://|@)\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}"),
    ("MAC / BD_ADDR", r"\b[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}\b"),
    ("IPv6 address derived from a MAC", r"fe80::[0-9a-f:]*ff:fe[0-9a-f:]*"),
    ("a home LAN's subnet size", r"(?i)\b(?:LAN|network|subnet)\b.{0,24}/(?:1\d|2[0-3])\b|\.0/1[6-9]\b"),
    ("the owner", r"(?i)\bthe owner\b|\bowner's\b|\(owner\b"),
    ("paths on a personal machine", r"/Users/|/home/[a-z]|/Volumes/|/private/tmp|~/Backups|\bBackups/"),
    ("serial numbers on a command line", r"serialno=[0-9A-Za-z]{6,}"),
    ("SSIDs, PSKs, tokens, keys",
     r"(?i)\bssid\s*=\s*\"(?!your network)[^\"<]|\bpsk\s*=\s*\"(?!your password)|BEGIN [A-Z ]*PRIVATE KEY|"
     r"ghp_[A-Za-z0-9]{20}|xox[bp]-|\btoken\s*[:=]\s*[\"'][A-Za-z0-9]{12}"),
]
# deliberate exceptions, matched against the hit's line
ALLOW = [
    r"192\.168\.7\.[12]\b",          # the USB gadget link, as the A33 repository publishes it
    r"192\.168\.7\.0/24",            # ... and its subnet, in the same breath
    r"02:fa:a3:00:08:0[12]",         # g_ether's fixed, locally administered MACs in the DT
    r"fe80::fa:a3ff:fe00:802",       # the link-local address of that fixed gadget MAC
    r"(?i)\b(?:aa:bb:cc:dd:ee:ff|00:11:22:33:44:55|xx:xx:xx:xx:xx:xx|ff:ff:ff:ff:ff:ff)\b",  # placeholders, a test page target, broadcast
    r"C3:BE:12:00:00:01",            # our own test beacon's made-up static random address
    r"\.owner(?:_check)?\s*=",       # struct fields in C
]
FIRMWARE = re.compile(r"\.(fw|bin|img|ko|so|elf|apk|nvm)$|wcnmodem|gsl1680|pskey\.bin|wifi-nvm")
FW_OK = {"tools/scrub-private.txt", "tools/bt-pskey.py", "tools/wcn-nvm.py", "tools/extract-gsl-fw.py",
         "firmware/extract.sh", "firmware/SHA256SUMS", "firmware/README.md", "firmware/patches/README.md"}
SELF = "tools/scrub-check.sh"

def git(*a):
    return subprocess.run(("git",) + a, capture_output=True, text=True, check=True).stdout

files = sorted(set(git("ls-files", "-co", "--exclude-standard").split("\n")) - {"", SELF, "tools/scrub-private.txt"})
# Names only the maintainers know -- people, hosts, networks, serials -- are
# not written here, where publishing them would be the leak: they go one
# regex per line in tools/scrub-private.txt (gitignored), with an optional
# "allow: REGEX" line for a deliberate exception.
try:
    for line in open("tools/scrub-private.txt"):
        line = line.rstrip("\n")
        if line.startswith("allow: "):
            ALLOW.append(line[7:])
        elif line and not line.startswith("#"):
            RULES.append(("private names (tools/scrub-private.txt)", line))
    private = True
except FileNotFoundError:
    private = False
LINE_ALLOW = [re.compile(a) for a in ALLOW]
rules = [(n, re.compile(p)) for n, p in RULES]
hits = {}
binary = []
for f in files:
    try:
        data = open(f, "rb").read()
    except (FileNotFoundError, IsADirectoryError):
        continue
    if re.search(rb"[\x00-\x08\x0e-\x1a\x1c-\x1f]", data):
        binary.append(f)
        continue
    for i, line in enumerate(data.decode("utf-8", "replace").split("\n"), 1):
        for n, r in rules:
            for m in r.finditer(line):
                if any(s.start() <= m.start() and m.end() <= s.end() or m.start() <= s.start() and s.end() <= m.end()
                       for a in LINE_ALLOW for s in a.finditer(line)):
                    continue
                hits.setdefault(n, []).append(f"{f}:{i}:{line.strip()[:160]}")
                break

fw = [f for f in files if FIRMWARE.search(f) and f not in FW_OK]
bad = False
for n in dict.fromkeys(n for n, _ in RULES):
    if n in hits:
        bad = True
        print("==", n)
        print("\n".join(hits[n]))
if binary:
    bad = True; print("== binary files"); print("\n".join(binary))
if fw:
    bad = True; print("== firmware-looking files"); print("\n".join(fw))
if "--history" in sys.argv[1:]:
    log = git("log", "--format=%H%n%an <%ae>%n%cn <%ce>%n%B") if git("rev-list", "--all") else ""
    h = []
    for i, line in enumerate(log.split("\n"), 1):
        for n, r in rules:
            if r.search(line) and not any(a.search(line) for a in LINE_ALLOW):
                h.append(f"{n}: {line[:160]}")
    if h:
        bad = True; print("== commit history"); print("\n".join(h))
if not private:
    print("note: no tools/scrub-private.txt, so only the generic rules ran")
if not bad:
    print(f"scrub-check: clean ({len(files)} files{', history too' if '--history' in sys.argv[1:] else ''})")
sys.exit(1 if bad else 0)
PY
