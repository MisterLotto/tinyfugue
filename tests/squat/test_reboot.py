#!/usr/bin/env python3
"""/reboot -y keeps plain-TCP connections: the game must see ONE connection that carries text sent
before and after the reboot, for a named world and for a connection made with /connect host port.

    TF_BIN=src/tf TF_LIB=lib/tf python3 tests/squat/test_reboot.py
"""
import os, pty, select, socket, sys, tempfile, threading, time

TF = os.path.abspath(os.environ["TF_BIN"])
LIB = os.path.abspath(os.environ["TF_LIB"])


def run(connect):
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(5)
    port = srv.getsockname()[1]
    seen, accepts = [], [0]

    def serve():
        while True:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            accepts[0] += 1

            def rd(c=c):
                buf = b""
                while True:
                    d = c.recv(4096)
                    if not d:
                        seen.append("CLOSED")
                        return
                    buf += d
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        seen.append(line.strip().decode())

            threading.Thread(target=rd, daemon=True).start()

    threading.Thread(target=serve, daemon=True).start()
    home = tempfile.mkdtemp()
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.update(HOME=home, TERM="xterm")
        os.execv(TF, [TF, "-n", "-L" + LIB])

    def pump(sec):
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    os.read(fd, 65536)
                except OSError:
                    return

    def send(line, wait=0.6):
        os.write(fd, (line + "\r").encode())
        pump(wait)

    pump(1.5)
    for cmd in connect(port):
        send(cmd, 0.8)
    send("before-reboot")
    send("/reboot -y", 3.0)
    ok = True
    try:
        send("after-reboot", 1.0)
    except OSError:
        ok = False
    result = list(seen)  # before TinyFugue is killed (that closes the connection)
    try:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
    except OSError:
        pass
    srv.close()
    return ok and accepts[0] == 1 and result == ["before-reboot", "after-reboot"], accepts[0], result


fails = 0
for name, connect in (
    ("named world", lambda p: ["/addworld rbw 127.0.0.1 %d" % p, "/connect rbw"]),
    ("connect host port (temporary world)", lambda p: ["/connect 127.0.0.1 %d" % p]),
):
    ok, accepts, seen = run(connect)
    print(("PASS" if ok else "FAIL"), "reboot keeps the connection:", name, "| connections:", accepts, "| game saw:", seen)
    fails += not ok
sys.exit(1 if fails else 0)
