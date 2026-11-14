# squat tests

`test_squat.py` drives a real TinyFugue (in a pseudo-terminal) against
`fakebridge.py`, a stand-in for `tf-squatterd` that speaks the TFCTL protocol,
and a mock MUD. It needs no Go toolchain and checks the basics of `src/squat.c`:

* the bridge child inherits only fds 0-3 (TF's game sockets are not leaked)
* sessions, raw ANSI lines, echo lines and prompts are reported
* a remote send is refused while TF has control and accepted after takeover
* text that looks like a TF command is sent to the MUD verbatim, never executed
* silent (noEcho) sends reach the MUD but not the screen or the bridge
* the TF keyboard is locked while a remote controls the session
* world records are reported without passwords
* remote connect, disconnect and dismiss

```sh
TF_BIN=/path/to/src/tf TF_LIB=/path/to/lib/tf python3 tests/squat/test_squat.py
```

`test_rendezvous.py` covers the rendezvous (a restarted bridge knocks: token check, held candidate, attach swaps the
channel and closes the old one, a dead channel detaches without touching game connections, resync, cleanup on
`/squat off`). The real bridge restarting under a real TinyFugue is `TestRealTinyFugueReload` in squatClientDaemon
(`internal/tfbridge`, needs TF_BIN/TF_LIB).

Everything else (remote world editing, GMCP, takeover requests, `tf.exec`, the
status indicator, `disp`, TLS, pairing, roles) is covered with the real bridge
and real games by the Go harness in the squatClientDaemon repository:
`make test-tf` and `make harness` (see `scripts/tfharness/README.md` there).
