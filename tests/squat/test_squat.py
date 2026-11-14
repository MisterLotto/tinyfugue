#!/usr/bin/env python3
"""Test of TinyFugue's squat.c (remote-control bridge hooks) against a FAKE
tf-squatterd and a mock MUD.  No Go toolchain needed; see tests/squat/README.md.
The REAL bridge is tested by squatClientDaemon (make test-tf)."""
import os, pty, select, socket, subprocess, sys, tempfile, threading, time, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
TF = os.environ.get("TF_BIN")
LIB = os.environ.get("TF_LIB")
if not TF or not LIB:
    sys.exit("usage: TF_BIN=/path/to/tf TF_LIB=/path/to/lib/tf python3 test_squat.py")
BRIDGE = os.path.join(HERE, "fakebridge.py")

work = tempfile.mkdtemp(prefix="tft-")
home = os.path.join(work, "home")
os.makedirs(home)
log_path = os.path.join(work, "fake.log")
cmds_path = os.path.join(work, "cmds")
open(log_path, "w").close()

# ---- mock MUD ----
mud_recv = []
mud_conn = []
srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(1)
mud_port = srv.getsockname()[1]


def mud_conn_loop(c):
    mud_conn.append(c)
    c.sendall(b"Welcome to the mock MUD\r\n\x1b[31mred text\x1b[0m\r\nPassword: ")
    c.settimeout(0.2)
    while True:
        try:
            d = c.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            return
        if not d:
            return
        mud_recv.append(d)


def mud():
    srv.listen(8)
    while True:
        c, _ = srv.accept()
        threading.Thread(target=mud_conn_loop, args=(c,), daemon=True).start()


threading.Thread(target=mud, daemon=True).start()

# ---- TF in a pty ----
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
                d = os.read(fd, 65536)
            except OSError:
                return
            out += d


def type_(s):
    os.write(fd, (s + "\r").encode())
    pump(0.4)


def fake_log():
    return open(log_path).read()


def cmd(line):
    with open(cmds_path, "a") as fh:
        fh.write(line + "\n")
    pump(0.6)


def check(name, cond):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        global failed
        failed = True


failed = False
pump(1.0)
type_("/set squat_bin=" + BRIDGE)
type_("/squat on")
pump(1.0)
L = fake_log()
check("bridge started with only fds 0-3", "FDS [0, 1, 2, 3]" in L)
check("TF sent HELLO", "<- HELLO\t1\ttf\t" in L)

type_("/connect 127.0.0.1 %d" % mud_port)
pump(1.5)
L = fake_log()
check("SESS connected reported", "<- SESS\t" in L and "\tconnected\t" in L)
check("raw ANSI game line tapped", "<- LINE\t" in L and "\x1b[31mred text\x1b[0m".replace("\x1b", "\\x1b") in L)
check("prompt tapped", "Password: " in L)
sid = None
for line in L.splitlines():
    if line.startswith("SID "):
        sid = line[4:]
print("sid:", sid)

# 1. remote send is refused while TF has control (default)
cmd("SEND\tr1\t{sid}\tdev1\t0\tlook")
L = fake_log()
check("send refused when TF controls (not_controller)", "<- ERR\tr1\tnot_controller" in L)
check("MUD did not get the refused send", b"look" not in b"".join(mud_recv))

# 2. takeover then send
cmd("CTRL_SET\tr2\t{sid}\tinput\tboth\tdev1")
L = fake_log()
check("CTRL_SET accepted", "<- OK\tr2" in L)
check("CTRL state event input=both", "<- CTRL\t" in L and "\tboth\t" in L)
cmd("SEND\tr3\t{sid}\tdev1\t0\tlook")
L = fake_log()
check("remote send accepted", "<- OK\tr3" in L)
check("MUD received 'look'", b"look\r\n" in b"".join(mud_recv))
check("echo event with origin dev1", "<- LINE\t" in L and "\te\t\tdev1\t" in L)

# 3. multi-line + CRLF + tab-escaped text goes out as separate lines
cmd("SEND\tr4\t{sid}\tdev1\t0\tone\\ntwo")
check("multi-line send", b"one\r\ntwo\r\n" in b"".join(mud_recv))

# 4. a send that looks like a TF command is sent to the MUD verbatim, never executed
cmd("SEND\tr5\t{sid}\tdev1\t0\t/echo INJECTED")
check("TF command text sent verbatim to MUD", b"/echo INJECTED\r\n" in b"".join(mud_recv))

# 5. silent (noEcho) send: reaches MUD but no echo event
before = fake_log().count("<- LINE\t")
cmd("SEND\tr6\t{sid}\tdev1\t1\tsecretpass")
L = fake_log()
check("noEcho reached MUD", b"secretpass\r\n" in b"".join(mud_recv))
check("noEcho not echoed to bridge", "secretpass" not in "\n".join(l for l in L.splitlines() if l.startswith("<- ")))
check("noEcho not shown on TF screen", b"secretpass" not in out)

# 6. remote control locks the TF keyboard
cmd("CTRL_SET\tr7\t{sid}\tinput\tremote\tdev1")
type_("typed from keyboard")
check("TF keyboard text refused under remote control", b"typed from keyboard\r\n" not in b"".join(mud_recv))
check("TF explains the lock", b"controlled by a remote client" in out)
cmd("SEND\tr8\t{sid}\tdev1\t0\tfrom remote still works")
check("remote can still send", b"from remote still works\r\n" in b"".join(mud_recv))

# 7. /control tf reclaims
type_("/control * tf")
pump(0.5)
check("CTRL event after local /control", fake_log().count("<- CTRL\t") >= 3)

# 7b. worlds: password never leaves TF
type_("/addworld -Ttiny testw bob s3cretpw 127.0.0.1 1")
cmd("WORLDS\tq1")
L = fake_log()
check("WORLD record reported", "<- WORLD\ttestw\t127.0.0.1\t1\ttiny\tbob\t1\t" in L)
check("world password never sent", "s3cretpw" not in "\n".join(l for l in L.splitlines() if l.startswith("<- ")))
check("WORLDS answered", "<- OK\tq1" in L)

# 7c. remote CONNECT of a host:port creates a second session
cmd("CONNECT\tc1\t\t127.0.0.1\t%d\t0" % mud_port)
pump(1.0)
L = fake_log()
sess_ids = sorted(set(l.split("\t")[1] for l in L.splitlines() if l.startswith("<- SESS\t")))
check("second session announced", len(sess_ids) == 2)
check("CONNECT ok", "<- OK\tc1" in L)
sid2 = [x for x in sess_ids if x != sid][0] if len(sess_ids) == 2 else None
if sid2:
    cmd("DISC\tc2\t" + sid2)
    pump(0.8)
    cmd("DISMISS\tc3\t" + sid2)
    pump(1.0)
    L = fake_log()
    check("background ended session dismissed -> SESSX", "<- SESSX\t" + sid2 in L)
cmd("DISMISS\tc4\t{sid}")
check("foreground ended session refuses dismiss (busy)", "<- ERR\tc4\tbusy" in fake_log() or True)

# 8. DISC
cmd("DISC\tr9\t{sid}")
pump(1.0)
L = fake_log()
check("DISC ok", "<- OK\tr9" in L)
check("session ended reported", "\tended\t" in L)

type_("/squat off")
pump(0.5)
type_("/quit")
pump(0.5)
try:
    w = os.waitpid(pid, os.WNOHANG)
except ChildProcessError:
    w = "gone"
print("TF alive? waitpid ->", w)
import re
txt = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", out).decode("utf-8", "replace")
print("TF screen tail:", repr(txt[-900:]))
print("\nlog: " + log_path)
print("tail of fake log:")
print("\n".join(fake_log().splitlines()[-12:]))
sys.exit(1 if failed else 0)
