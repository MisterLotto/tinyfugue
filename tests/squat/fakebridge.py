#!/usr/bin/env python3
"""Fake tf-squatterd for testing TinyFugue's squat.c.

Speaks TFCTL on fd 3.  Logs every received line to $FAKE_LOG.  Commands to send
to TF are appended (one TFCTL line each, "{sid}" replaced by the first session
id seen) to $FAKE_CMDS by the test."""
import os, sys, socket, time, select

open_fds = []
for n in range(0, 64):
    try:
        os.fstat(n)
        open_fds.append(n)
    except OSError:
        pass
log = open(os.environ["FAKE_LOG"], "a", buffering=1)
cmds_path = os.environ["FAKE_CMDS"]
log.write("ARGV %r\n" % (sys.argv,))
# which fds did we inherit?  (must be only 0,1,2,3)
log.write("FDS %r\n" % (open_fds,))
s = socket.socket(fileno=3)
s.setblocking(False)
buf = b""
sid = None
cmd_off = 0


def send(line):
    s.sendall((line + "\n").encode())


while True:
    r, _, _ = select.select([s], [], [], 0.1)
    if r:
        try:
            d = s.recv(65536)
        except BlockingIOError:
            d = None
        if d == b"":
            log.write("EOF\n")
            break
        if d:
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace")
                log.write("<- " + text + "\n")
                f = text.split("\t")
                if f[0] == "HELLO":
                    send("HELLO\t1\tbridge\tfake")
                if f[0] == "SESS" and sid is None:
                    sid = f[1]
                    log.write("SID %s\n" % sid)
    if os.path.exists(cmds_path):
        with open(cmds_path) as fh:
            fh.seek(cmd_off)
            data = fh.read()
            cmd_off = fh.tell()
        for line in data.splitlines():
            if not line.strip():
                continue
            line = line.replace("{sid}", sid or "NONE").replace("\\t", "\t")
            log.write("-> " + line + "\n")
            send(line)
