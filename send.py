#!/usr/bin/env python3
"""Send a file to the Windows VM and prove it arrived.

scp reports success and the file is not there -- measured repeatedly, including
today. base64 in a single command line hits "The command line is too long" at
about 4 KB, and 30 KB chunks do not survive the shell either. What works is
6-8 chunked AppendAllText calls, then a decode script that reports the MD5, the
length and whether the result parses, so the delivery is verified instead of
assumed.

    ./send.py run_v6.ps1        -> C:\\tdvr\\run_v6.ps1
"""
import base64, hashlib, subprocess, sys

VM = "vm@192.168.1.6"
f = sys.argv[1]
src = open(f, "rb").read()
name = f.rsplit("/", 1)[-1]
want = hashlib.md5(src).hexdigest()
b = base64.b64encode(src).decode()
chunks = [b[i:i + 2000] for i in range(0, len(b), 2000)]

def sh(cmd):
    return subprocess.run(["ssh", "-o", "ConnectTimeout=15", VM, cmd],
                          capture_output=True, text=True)

sh('powershell -NoProfile -Command "Remove-Item C:\\\\tdvr\\\\b64in.txt -EA 0"')
for c in chunks:
    r = sh('powershell -NoProfile -Command "[IO.File]::AppendAllText(\'C:\\\\tdvr\\\\b64in.txt\',\'%s\')" ' % c)
    if r.returncode:
        print("CHUNK FAILED:", r.stderr[:200]); sys.exit(1)

script = (
    '$x = [IO.File]::ReadAllText("C:\\\\tdvr\\\\b64in.txt")\n'
    '[IO.File]::WriteAllBytes("C:\\\\tdvr\\\\%s", [Convert]::FromBase64String($x))\n'
    '"md5   " + (Get-FileHash C:\\\\tdvr\\\\%s -Algorithm MD5).Hash\n'
    '"bytes " + (Get-Item C:\\\\tdvr\\\\%s).Length\n'
    '$e = $null\n'
    '[void][System.Management.Automation.Language.Parser]::ParseFile("C:\\\\tdvr\\\\%s", [ref]$null, [ref]$e)\n'
    'if ($e) { "PARSE FAIL"; $e | Select-Object -First 3 | ForEach-Object { "  " + $_.Message } } else { "parse=OK" }\n'
    'Remove-Item C:\\\\tdvr\\\\b64in.txt -EA 0\n'
) % (name, name, name, name)

# the decode script is small, so it goes over the same channel as the payload
d = base64.b64encode(script.encode()).decode()
sh('powershell -NoProfile -Command "Remove-Item C:\\\\tdvr\\\\decode.tmp -EA 0"')
for c in [d[i:i + 2000] for i in range(0, len(d), 2000)]:
    sh('powershell -NoProfile -Command "[IO.File]::AppendAllText(\'C:\\\\tdvr\\\\decode.tmp\',\'%s\')" ' % c)
sh('powershell -NoProfile -Command "[IO.File]::WriteAllBytes(\'C:\\\\tdvr\\\\decode.ps1\',[Convert]::FromBase64String([IO.File]::ReadAllText(\'C:\\\\tdvr\\\\decode.tmp\'))); Remove-Item C:\\\\tdvr\\\\decode.tmp"')

r = sh("powershell -NoProfile -ExecutionPolicy Bypass -File C:\\\\tdvr\\\\decode.ps1")
print(r.stdout.strip())
got = ""
for line in r.stdout.splitlines():
    if line.strip().upper().startswith("MD5"):
        got = line.split()[-1].lower()
print("local md5  %s  %s" % (want, "MATCH" if got == want else "MISMATCH"))
sys.exit(0 if got == want else 1)
