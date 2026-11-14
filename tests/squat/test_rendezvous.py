#!/usr/bin/env python3
"""Test of the rendezvous in TinyFugue's squat.c: a restarted bridge knocks, TinyFugue swaps channels,
the game connection never drops, a dead channel is survived.  Uses the FAKE bridge for the first
channel and this script as the knocking bridge.  See tests/squat/README.md."""
import os, pty, select, socket, subprocess, sys, tempfile, threading, time, stat

HERE = os.path.dirname(os.path.abspath(__file__))
TF = os.environ.get("TF_BIN")
LIB = os.environ.get("TF_LIB")
if not TF or not LIB:
    sys.exit("usage: TF_BIN=/path/to/tf TF_LIB=/path/to/lib/tf python3 test_rendezvous.py")
BRIDGE = os.path.join(HERE, "fakebridge.py")

work = tempfile.mkdtemp(prefix="tfr-", dir="/tmp")
home = os.path.join(work, "home"); os.makedirs(home)
sq = os.path.join(work, "sq")
log_path = os.path.join(work, "fake.log"); open(log_path, "w").close()
cmds_path = os.path.join(work, "cmds")

# mock MUD: remembers whether its connection was closed
mud_closed = []
mud_conns = []
srv = socket.socket(); srv.bind(("127.0.0.1", 0)); srv.listen(4)
mud_port = srv.getsockname()[1]


def mud_loop(c):
    mud_conns.append(c)
    c.sendall(b"Welcome\r\n")
    while True:
        try:
            d = c.recv(4096)
        except OSError:
            d = b""
        if not d:
            mud_closed.append(1)
            return


def mud():
    while True:
        c, _ = srv.accept()
        threading.Thread(target=mud_loop, args=(c,), daemon=True).start()


threading.Thread(target=mud, daemon=True).start()

env = dict(os.environ, HOME=home, TERM="xterm", FAKE_LOG=log_path, FAKE_CMDS=cmds_path)
pid, fd = pty.fork()
if pid == 0:
    os.execve(TF, [TF, "-n", "-L" + LIB], env)
out = b""


def pump(t=0.5):
    global out
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                out += os.read(fd, 65536)
            except OSError:
                return


def type_(s):
    os.write(fd, (s + "\r").encode()); pump(0.4)


failed = False


def check(name, cond):
    global failed
    print(("PASS " if cond else "FAIL ") + name)
    failed = failed or not cond


def esc(f):
    return f.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")


class Chan:
    def __init__(self, s):
        self.s = s; self.buf = b""; self.lines = []; self.eof = False

    def send(self, *f):
        self.s.sendall(("\t".join(esc(x) for x in f) + "\n").encode())

    def pump(self, t=0.5):
        end = time.time() + t
        self.s.settimeout(0.05)
        while time.time() < end:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                self.eof = True; return
            if not d:
                self.eof = True; return
            self.buf += d
            while b"\n" in self.buf:
                l, self.buf = self.buf.split(b"\n", 1)
                self.lines.append(l.decode("utf-8", "replace"))

    def has(self, prefix):
        return any(l.startswith(prefix) for l in self.lines)


def knock(token, pid_=4242):
    c = socket.socket(socket.AF_UNIX)
    c.connect(open(os.path.join(sq, "ctl.token")).read().split("\n")[1])
    ch = Chan(c)
    ch.send("KNOCK", token, str(pid_))
    ch.pump(0.6)
    return ch


pump(1.0)
type_("/set squat_bin=" + BRIDGE)
type_("/set squat_dir=" + sq)
type_("/squat on")
pump(1.0)
tok_path = os.path.join(sq, "ctl.token")
check("token file exists, mode 0600", os.path.exists(tok_path) and stat.S_IMODE(os.stat(tok_path).st_mode) == 0o600)
token, sock = open(tok_path).read().split("\n")[:2]
check("token is 64 hex chars", len(token) == 64 and all(c in "0123456789abcdef" for c in token))
check("socket exists, mode 0600", os.path.exists(sock) and stat.S_IMODE(os.stat(sock).st_mode) == 0o600)

type_("/connect 127.0.0.1 %d" % mud_port)
pump(1.0)
check("first bridge saw the session", "<- SESS\t" in open(log_path).read())

# wrong token: closed, nothing changes
bad = knock("0" * 64)
check("wrong token gets no HELLO", not bad.has("HELLO"))
check("wrong token connection is closed", bad.eof)
pump(0.3)
check("TF noted the rejected knock", b"rejected a knock" in out)

# a good knock is HELD: it gets HELLO, the first bridge is undisturbed
b2 = knock(token)
check("good knock answered with HELLO (instance, features)", b2.has("HELLO\t1\ttf\t"))
cmds = open(cmds_path, "a"); cmds.write("NOTICE\tstill on the first bridge\n"); cmds.close()
pump(0.8); b2.pump(0.3)
check("TF still talks to the first bridge while the candidate is held", b"still on the first bridge" in out)
check("the candidate got nothing yet", not any(l.startswith(("SESS", "LINE")) for l in b2.lines))

# ATTACH: TF switches; the first bridge sees EOF; the game connection is untouched
b2.send("ATTACH")
b2.pump(0.8); pump(0.5)
check("TF reports the attach", b"a bridge (pid 4242) attached" in out)
check("first bridge saw EOF", "EOF" in open(log_path).read())
check("MUD connection still open", not mud_closed)
mud_conns[0].sendall(b"after attach\r\n")
b2.pump(1.0)
check("lines now go to the new bridge", any(l.startswith("LINE") and "after attach" in l for l in b2.lines))
type_("/echo TF alive")
check("TF still responsive", b"TF alive" in out)

# the new bridge dies: TF detaches, keeps the connection and the rendezvous
b2.s.close()
pump(1.0)
check("TF says no bridge is attached", b"no bridge attached" in out)
check("MUD connection still open after the bridge died", not mud_closed)
check("rendezvous still open", os.path.exists(sock))

# a fresh bridge knocks and gets everything announced again, with history
b3 = knock(token, 777)
b3.send("ATTACH")
b3.pump(1.5)
check("detached TF re-announces its sessions", b3.has("SESS\t"))
check("...with history", any(l.startswith("LINE") and "after attach" in l for l in b3.lines))

# RESYNC from a bridge that lost its state
n = len(b3.lines)
b3.send("RESYNC")
b3.pump(1.5)
check("RESYNC makes TF announce again", sum(1 for l in b3.lines[n:] if l.startswith("SESS\t")) >= 1)

# /squat off: rendezvous gone, bridge channel closed
type_("/squat off")
pump(0.8); b3.pump(0.5)
check("rendezvous files removed", not os.path.exists(tok_path) and not os.path.exists(sock))
check("bridge saw EOF on /squat off", b3.eof)

try:
    os.kill(pid, 15)
except OSError:
    pass
print("FAILED" if failed else "ALL PASSED")
sys.exit(1 if failed else 0)
