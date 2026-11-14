# squat: remote control of a running TinyFugue

This branch (`squatterbridge`) lets a running TinyFugue be paired with a remote
client (SquatClient), the way `squatterd` pairs with one: one-time code, pinned
TLS fingerprint, device keys. The remote client sees all of TF's connections
(history, then live), can type into them, list, create, edit and delete worlds,
exchange GMCP and MSDP with the game, and can take control from, share control with, or
hand control back to the TF terminal.

Design and protocol: `squatClientDaemon/docs/TF-SQUATTER.md`.

## How it works

`/squat on` starts a helper, `tf-squatterd`, as TF's child process over a
private socketpair (no port or path for other users to reach). The helper does
all networking, TLS and authentication; TF only emits events and applies a few
commands (`src/squat.c`). Nothing in TF listens on the network.

## Build

```sh
./configure --enable-gmcp --enable-msdp && make   # needed to forward GMCP / MSDP (MSDP needs flex)
# the bridge (Go 1.24+):
cd ../squat/squatClientDaemon && make build      # produces ./tf-squatterd
```

Put `tf-squatterd` on your `PATH`, or `/set squat_bin=/path/to/tf-squatterd`.
On macOS, if linking fails with missing `utext_*`/`ucnv_*` symbols, add
`-licuuc` to `LIBS` in `src/Makefile` (configure only adds `-licui18n`).

## Use

```
/squat on                  start the bridge (listens on 127.0.0.1:10328, TLS)
/squat pair [viewer|operator|admin]
                           print a one-time pairing code and the TLS fingerprint
/squat devices | /squat revoke <id>
/squat status | /squat off
/control                   show who controls each world (and waiting requests)
/control <world>|* tf|remote|both
/control <world>|* automation tf|client
/control allow|deny [n]    answer a takeover request   /control pending
```

Settings are ordinary variables read when `/squat on` runs (and some when used):

| variable | default | meaning |
|---|---|---|
| `squat_bin` | `tf-squatterd` | helper program |
| `squat_port` | `10328` | listen port |
| `squat_bind` | `127.0.0.1` | listen address (loopback unless you change it) |
| `squat_dir` | `~/.tf-squatter` | keys, certificate, paired devices, log |
| `squat_plain` | `0` | non-zero: plain `ws://` (loopback only; e.g. behind an ssh tunnel) |
| `squat_log` | `info` | `debug` logs every message between TF and the helper |
| `squat_takeover` | `auto` | `auto` lets a paired operator take over, `ask` asks you first, `off` refuses |
| `squat_ask_timeout` | `30` | seconds before an unanswered `ask` is refused |
| `squat_exec` | off | remote TF commands for admin devices (`tf.exec`): `off`, `allow` (only the commands in `squat_exec_allow`) or `on` (any command: this equals a shell on the machine, see below) |
| `squat_exec_allow` | `version listworlds listsockets listdef listvar recall help` | space-separated command names permitted when `squat_exec=allow` |
| `squat_scripts` | off | uploaded scripts (`tf.script.put`, admin devices): `off`, `safe` (sandboxed, see below) or `on` (TinyFugue's own rights) |
| `squat_disp` | off | also send the post-trigger display text of each line |
| `squat_mtts` | off | `on`: answer the terminal-type request with a standard `MTTS` capability reply (UTF-8, colours TF supports); `client`: also claim 256 colours and truecolor for the remote client. Read when a connection starts. Games that read only the first reply are unaffected |

Needs `/set background=on` (the default) so worlds other than the foreground
one are read.

The status variable `squat_status` (shown in a status field that `/squat on`
adds) reads like `sq:3 R1 A1 ?1`: sessions, worlds a remote may type into,
worlds whose triggers a client runs, takeover requests waiting for you.

## Control

Each world has two independent switches:

* **input**: `tf` (only the TF keyboard), `remote` (TF keyboard locked for that
  world), `both`. Remote text is sent to the game verbatim and is never parsed
  as a TF command. The window size the game is told follows the controller.
* **automation**: `tf` (TF's triggers run) or `client` (TF skips trigger matching
  for that world while a client owns it). Both revert to `tf` when the client
  disconnects or the bridge stops.

`/control <world> tf` always takes a world back.

## Remote editing and commands

* Worlds can be created, edited and deleted from the client. The password is
  write-only; a macro file (code TF loads on connect) can only be set by an
  admin device and only while `squat_exec` is on.
* `tf.exec` runs a TF command for an admin device and returns its output. It is
  off unless you set `squat_exec`:
  * `allow`: only the commands named in `squat_exec_allow` (default: read-only
    ones). A line is refused if it chains commands (`%;`), holds control
    characters, or starts with any other command. Setting a world's macro file
    is refused too.
  * `on`: any command. Treat it as shell access to the account TinyFugue runs
    as: TinyFugue can run programs (`/sh`, `/quote !`), load files and define
    macros, and a command can reach all of that indirectly (`/eval`, `/def`).
    **A deny-list cannot make `on` safe; use `allow`.**
  Only an admin device can use it at all, and the enforcement is in TinyFugue,
  not in the bridge or the client. What a remote device *types into a game* is
  never run as a TinyFugue command: it goes to the game verbatim. There is no way
  to send TinyFugue code or files through the bridge other than `tf.exec`.

## Uploaded scripts

An admin device can send a macro file (`tf.script.put`): the bridge stores it in
`<squat_dir>/scripts/` and TinyFugue loads it, when `squat_scripts` allows.

* `safe` runs the script **sandboxed**: while it loads, and every time a macro it
  defined runs later (a trigger, a hook, a timer), TinyFugue behaves as under
  `/restrict world`: no `/sh`, `/quote !` or pipes, no file access (`/log`, `/save`,
  `/lcd`, `/load`), no connections to hosts that are not defined worlds, and no Lua
  (`/calllua`, `/loadlua`). The script cannot run `/restrict`, `/squat`, `/control`, or
  change any `squat_*` variable, so it cannot loosen these settings. TinyFugue's own
  restriction level is unchanged and its keyboard is not affected.
* `on` loads it with TinyFugue's own rights: treat it like a file you wrote.
* Only files directly inside the scripts directory are ever loaded, whatever a client sends.
* Removing a script removes the file; the macros it defined remain until `/undef`.

## Tests

```sh
TF_BIN=src/tf TF_LIB=lib/tf python3 tests/squat/test_squat.py     # TF against a fake bridge
cd ../squat/squatClientDaemon
make test-tf  TF_BIN=... TF_LIB=...                               # TF against the real bridge
make harness  TF_BIN=... TF_LIB=...                               # everything, against real games
```
