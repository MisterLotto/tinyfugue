/*************************************************************************
 *  TinyFugue - programmable mud client
 *
 *  squat: remote control of a running TinyFugue.
 *
 *  This module owns one end of a local socketpair whose other end is held by
 *  the tf-squatterd helper process.  It (a) reports sockets and text to the
 *  helper, and (b) applies the helper's commands (send text, connect,
 *  disconnect, change control mode).  The helper does all networking, TLS and
 *  authentication; nothing here listens on the network.
 *
 *  Wire format ("TFCTL", local only): one message per line, fields separated
 *  by TAB.  Inside a field, backslash, TAB, LF, CR and other control bytes are
 *  escaped as \\ \t \n \r \xHH.  See docs/TF-SQUATTER.md section 5.
 *
 *  TinyFugue (aka "tf") is protected under the terms of the GNU
 *  General Public License.  See the file "COPYING" for details.
 ************************************************************************/

#include "tfconfig.h"
#include "port.h"
#include "tf.h"
#include "util.h"
#include "pattern.h"
#include "search.h"
#include "tfio.h"
#include "history.h"
#include "world.h"
#include "socket.h"
#include "output.h"
#include "attr.h"
#include "variable.h"
#include "expand.h"
#include "world.h"
#include "squat.h"
#include "command.h"

#if ENABLE_SQUAT

#include <ctype.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define SQUAT_OUT_MAX   (4u * 1024 * 1024)	/* queued bytes before we drop the bridge */
#define SQUAT_LINE_MAX  (256u * 1024)		/* longest accepted TFCTL line */
#define SQUAT_TEXT_MAX  (64u * 1024)		/* longest text field we send */
#define SQUAT_REPLAY    500			/* lines of history sent per connection */

int squat_kbd_depth = 0;
int squat_in_remote_send = 0;
int squat_silent = 0;
int squat_skip_output = 0;
int squat_fd = -1;

static pid_t bridge_pid = -1;
static pid_t reap_pid = -1;		/* killed child still to be reaped */
static int bridge_ready = 0;		/* bridge sent HELLO */
static char instance[17];
static unsigned long boot_epoch;
static char origin_buf[64];		/* device that is sending right now */
static int atexit_set = 0;

/* ------------------------------------------------------------------ */
/* growable buffers                                                    */
/* ------------------------------------------------------------------ */

typedef struct { char *p; size_t len, cap; } Buf;

static int buf_reserve(Buf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    char *np;

    if (need <= b->cap) return 1;
    if (b->cap == 0) b->cap = 256;
    while (b->cap < need) b->cap *= 2;
    if (!(np = (char *)realloc(b->p, b->cap))) return 0;
    b->p = np;
    return 1;
}

static void buf_put(Buf *b, const char *s, size_t n)
{
    if (!buf_reserve(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_str(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

static void buf_esc(Buf *b, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
	unsigned char c = (unsigned char)s[i];
	if (c == '\\') buf_put(b, "\\\\", 2);
	else if (c == '\t') buf_put(b, "\\t", 2);
	else if (c == '\n') buf_put(b, "\\n", 2);
	else if (c == '\r') buf_put(b, "\\r", 2);
	else if (c < 0x20 || c == 0x7f) {
	    char e[4] = { '\\', 'x', hex[c >> 4], hex[c & 15] };
	    buf_put(b, e, 4);
	} else buf_put(b, (const char *)&c, 1);
    }
}

static void buf_field(Buf *b, const char *s)
{
    buf_put(b, "\t", 1);
    buf_esc(b, s ? s : "", s ? strlen(s) : 0);
}

static void buf_num(Buf *b, long long v)
{
    char t[32];
    snprintf(t, sizeof t, "\t%lld", v);
    buf_str(b, t);
}

static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

/* ------------------------------------------------------------------ */
/* switches (ordinary TF variables, read when needed)                  */
/* ------------------------------------------------------------------ */

static int var_on(const char *name)
{
    const char *v = getvar(name);
    return v && *v && strcmp(v, "0") && strcmp(v, "off") && strcmp(v, "no");
}

/* squat_exec: off (default) | allow (only the commands in squat_exec_allow) | on (any command). */
#define EXEC_OFF 0
#define EXEC_ALLOW 1
#define EXEC_ON 2
#define EXEC_ALLOW_DEFAULT "version listworlds listsockets listdef listvar recall help"

static int exec_mode(void)
{
    const char *v = getvar("squat_exec");
    if (!v || !*v || !strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "no")) return EXEC_OFF;
    if (!strcmp(v, "allow")) return EXEC_ALLOW;
    return EXEC_ON;
}

/* squat_scripts: off (default) | safe (sandboxed: no shell, files, new connections or Lua,
 * also later when its macros run) | on (runs with TinyFugue's own rights).
 * Uploaded scripts are files in <squat_dir>/scripts. */
#define SCRIPTS_OFF 0
#define SCRIPTS_SAFE 1
#define SCRIPTS_ON 2
static char squat_scripts_dir[1100];

static int scripts_mode(void)
{
    const char *v = getvar("squat_scripts");
    if (!v || !*v || !strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "no")) return SCRIPTS_OFF;
    if (!strcmp(v, "safe")) return SCRIPTS_SAFE;
    return SCRIPTS_ON;
}

/* Any exec at all (the features list, the macro-file check's second half). */
static int exec_on(void) { return exec_mode() != EXEC_OFF; }

/* Is this command line permitted in allow mode? Only a plain "/name args" whose
 * name is in squat_exec_allow: no command chaining (%;), no control characters,
 * no leading "/@"-style prefixes. */
static int exec_allowed(const char *line)
{
    const char *list = getvar("squat_exec_allow");
    const char *p, *e;
    size_t n;
    if (!list || !*list) list = EXEC_ALLOW_DEFAULT;
    if (strstr(line, "%;")) return 0;
    for (p = line; *p; p++) if ((unsigned char)*p < 0x20) return 0;
    p = line + 1;
    for (e = p; *e && *e != ' ' && *e != '\t'; e++) ;
    n = (size_t)(e - p);
    if (n == 0) return 0;
    for (p = list; *p; ) {
	while (*p == ' ' || *p == '\t') p++;
	for (e = p; *e && *e != ' ' && *e != '\t'; e++) ;
	if ((size_t)(e - p) == n && !strncasecmp(p, line + 1, n)) return 1;
	p = e;
    }
    return 0;
}

static const char *var_or_default(const char *name, const char *dflt)
{
    const char *v = getvar(name);
    return (v && *v) ? v : dflt;
}
int squat_want_disp(void) { return squat_fd >= 0 && var_on("squat_disp"); }

#define POLICY_AUTO 0
#define POLICY_ASK  1
#define POLICY_OFF  2

static int policy_mode(void)
{
    const char *v = getvar("squat_takeover");
    if (v && !strcmp(v, "off")) return POLICY_OFF;
    if (v && !strcmp(v, "ask")) return POLICY_ASK;
    return POLICY_AUTO;
}

/* ------------------------------------------------------------------ */
/* output queue                                                        */
/* ------------------------------------------------------------------ */

static Buf outq;
static size_t outoff;
static Buf inq;

static void squat_stop(const char *why);

static void out_flush(void)
{
    while (squat_fd >= 0 && outoff < outq.len) {
	ssize_t n = write(squat_fd, outq.p + outoff, outq.len - outoff);
	if (n > 0) { outoff += (size_t)n; continue; }
	if (n < 0 && errno == EINTR) continue;
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
	squat_stop("control channel write failed");
	return;
    }
    if (squat_fd < 0) return;
    if (outoff >= outq.len) {
	outq.len = outoff = 0;
	writers_clear(squat_fd);
    } else {
	writers_set(squat_fd);
    }
}

/* Queue one finished message (b holds "TYPE\tfield...", no newline). */
static void send_msg(Buf *b)
{
    if (squat_fd < 0) return;
    if (outq.len - outoff + b->len + 1 > SQUAT_OUT_MAX) {
	squat_stop("bridge too slow, remote access stopped");
	return;
    }
    if (outoff > 0 && outoff == outq.len) outq.len = outoff = 0;
    buf_put(&outq, b->p, b->len);
    buf_put(&outq, "\n", 1);
    out_flush();
}

static void msg0(const char *type)
{
    Buf b = { 0 };
    buf_str(&b, type);
    send_msg(&b);
    buf_free(&b);
}

static void reply_ok(const char *reqid)
{
    Buf b = { 0 };
    buf_str(&b, "OK");
    buf_field(&b, reqid);
    send_msg(&b);
    buf_free(&b);
}

static void reply_ok2(const char *reqid, const char *extra)
{
    Buf b = { 0 };
    buf_str(&b, "OK");
    buf_field(&b, reqid);
    buf_field(&b, extra);
    send_msg(&b);
    buf_free(&b);
}

static void reply_err(const char *reqid, const char *code, const char *msg)
{
    Buf b = { 0 };
    buf_str(&b, "ERR");
    buf_field(&b, reqid);
    buf_field(&b, code);
    buf_field(&b, msg);
    send_msg(&b);
    buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* session table (what the bridge already knows)                       */
/* ------------------------------------------------------------------ */

typedef struct Known {
    unsigned long serial;
    int state, remote_echo, fg, tls, input, automation;
    int was_connected, quit_requested, seen;
    char sid[160];
    char world[128];		/* the name last announced (a world can be renamed) */
} Known;

static Known *known;
static size_t nknown, capknown;

static Known *known_find(unsigned long serial)
{
    size_t i;
    for (i = 0; i < nknown; i++)
	if (known[i].serial == serial) return &known[i];
    return NULL;
}

static Known *known_add(unsigned long serial)
{
    Known *k;
    if (nknown == capknown) {
	size_t nc = capknown ? capknown * 2 : 16;
	Known *nk = (Known *)realloc(known, nc * sizeof *known);
	if (!nk) return NULL;
	known = nk;
	capknown = nc;
    }
    k = &known[nknown++];
    memset(k, 0, sizeof *k);
    k->serial = serial;
    return k;
}

static void known_del(Known *k)
{
    size_t i = (size_t)(k - known);
    memmove(&known[i], &known[i + 1], (nknown - i - 1) * sizeof *known);
    nknown--;
}

/* ------------------------------------------------------------------ */
/* session ids                                                         */
/* ------------------------------------------------------------------ */

/* A session keeps the id it was announced with, even when its world is renamed. */
static void make_sid_new(const SquatSock *s, char *out, size_t n);
static void make_sid(const SquatSock *s, char *out, size_t n)
{
    Known *k = known_find(s->serial);
    if (k && k->sid[0]) snprintf(out, n, "%s", k->sid);
    else make_sid_new(s, out, n);
}

static void make_sid_new(const SquatSock *s, char *out, size_t n)
{
    char w[48];
    size_t i, j = 0;

    for (i = 0; s->world[i] && j < sizeof w - 1; i++) {
	unsigned char c = (unsigned char)s->world[i];
	w[j++] = (isalnum(c) || c == '_' || c == '.') ? (char)c : '_';
    }
    w[j] = '\0';
    snprintf(out, n, "w-%s-%lx-%lu", w, boot_epoch, s->serial);
}

/* Resolve a session id to a live socket; 0 if it does not match. */
static int sid_lookup(const char *sid, SquatSock *out)
{
    const char *dash = strrchr(sid, '-');
    unsigned long serial;
    char again[160];

    if (!dash || !(serial = strtoul(dash + 1, NULL, 10))) return 0;
    if (!squat_sock_info(serial, out)) return 0;
    make_sid(out, again, sizeof again);
    return strcmp(again, sid) == 0;
}

/* ------------------------------------------------------------------ */
/* events to the bridge                                                */
/* ------------------------------------------------------------------ */

static const char *state_name(int st)
{
    return st == SQUAT_ST_CONNECTING ? "connecting" :
	   st == SQUAT_ST_CONNECTED ? "connected" : "ended";
}

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void emit_sess(const SquatSock *s, const Known *k)
{
    Buf b = { 0 };
    char sid[160];

    make_sid(s, sid, sizeof sid);
    buf_str(&b, "SESS");
    buf_field(&b, sid);
    buf_field(&b, s->world);
    buf_field(&b, s->host);
    buf_field(&b, s->port);
    buf_num(&b, s->tls);
    buf_field(&b, state_name(s->state));
    buf_num(&b, s->remote_echo);
    buf_num(&b, s->fg);
    buf_num(&b, s->started_ms);
    buf_field(&b, s->state == SQUAT_ST_ENDED ?
	(k && k->quit_requested ? "client_quit" :
	 k && k->was_connected ? "remote_closed" : "connect_failed") : "");
    send_msg(&b);
    buf_free(&b);
}

static const char *input_name(int m)
{
    return m == SQUAT_INPUT_REMOTE ? "remote" : m == SQUAT_INPUT_BOTH ? "both" : "tf";
}

static const char *auto_name(int m) { return m == SQUAT_AUTO_CLIENT ? "client" : "tf"; }

static void emit_ctrl(const SquatSock *s, const char *device)
{
    Buf b = { 0 };
    char sid[160];

    make_sid(s, sid, sizeof sid);
    buf_str(&b, "CTRL");
    buf_field(&b, sid);
    buf_field(&b, input_name(s->input));
    buf_field(&b, auto_name(s->automation));
    buf_field(&b, device);
    send_msg(&b);
    buf_free(&b);
}

/* Announce a socket the bridge has not seen: state, control, replay. */
static Known *announce(const SquatSock *s)
{
    Known *k = known_add(s->serial);

    if (!k) return NULL;
    k->state = s->state;
    k->remote_echo = s->remote_echo;
    k->fg = s->fg;
    k->tls = s->tls;
    k->input = s->input;
    k->automation = s->automation;
    k->was_connected = (s->state == SQUAT_ST_CONNECTED);
    k->seen = 1;
    snprintf(k->world, sizeof k->world, "%s", s->world);
    make_sid_new(s, k->sid, sizeof k->sid);
    emit_sess(s, k);
    emit_ctrl(s, "");
    if (s->state != SQUAT_ST_CONNECTING)
	squat_sock_replay(s->serial);
    return k;
}

void squat_line_disp(const SquatSock *s, int kind, int flags, const char *origin,
    const char *text, size_t len, const char *disp, size_t displen)
{
    Buf b = { 0 };
    char sid[160], fl[8];
    int n = 0;

    if (squat_fd < 0 || !bridge_ready || !s) return;
    if (!known_find(s->serial) && !announce(s)) return;
    if (len > SQUAT_TEXT_MAX) len = SQUAT_TEXT_MAX;
    if (displen > SQUAT_TEXT_MAX) displen = SQUAT_TEXT_MAX;
    if (flags & SQUAT_LF_GAG) fl[n++] = 'g';
    if (flags & SQUAT_LF_PROMPT) fl[n++] = 'p';
    if (flags & SQUAT_LF_REPLAY) fl[n++] = 'r';
    fl[n] = '\0';
    make_sid(s, sid, sizeof sid);
    buf_str(&b, "LINE");
    buf_field(&b, sid);
    { char k[2] = { (char)kind, 0 }; buf_field(&b, k); }
    buf_field(&b, fl);
    buf_field(&b, origin ? origin : "game");
    buf_num(&b, now_ms());
    buf_put(&b, "\t", 1);
    buf_esc(&b, text, len);
    if (disp) {
	buf_put(&b, "\t", 1);
	buf_esc(&b, disp, displen);
    }
    send_msg(&b);
    buf_free(&b);
}

void squat_line(const SquatSock *s, int kind, int flags, const char *origin,
    const char *text, size_t len)
{
    squat_line_disp(s, kind, flags, origin, text, len, NULL, 0);
}

/* An MSDP message from the game: the raw payload; the bridge decodes it. */
void squat_msdp(const SquatSock *s, const char *data, size_t len)
{
    Buf b = { 0 };
    char sid[160];

    if (squat_fd < 0 || !bridge_ready || !s) return;
    if (!known_find(s->serial) && !announce(s)) return;
    if (len > SQUAT_TEXT_MAX) return;	/* never send half a message */
    make_sid(s, sid, sizeof sid);
    buf_str(&b, "MSDP");
    buf_field(&b, sid);
    buf_put(&b, "\t", 1);
    buf_esc(&b, data, len);
    send_msg(&b);
    buf_free(&b);
}

/* A GMCP message from the game: "Package.Name {json}". */
void squat_gmcp(const SquatSock *s, const char *text)
{
    Buf b = { 0 };
    char sid[160];
    const char *sp;

    if (squat_fd < 0 || !bridge_ready || !s) return;
    if (!known_find(s->serial) && !announce(s)) return;
    sp = strchr(text, ' ');
    make_sid(s, sid, sizeof sid);
    buf_str(&b, "GMCP");
    buf_field(&b, sid);
    buf_put(&b, "\t", 1);
    buf_esc(&b, text, sp ? (size_t)(sp - text) : strlen(text));
    buf_field(&b, sp ? sp + 1 : "");
    send_msg(&b);
    buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* MTTS (opt-in)                                                       */
/* ------------------------------------------------------------------ */

/* Reply for TTYPE request number *step (0, 1, 2, ...) when /set squat_mtts is
 * "on" or "client"; returns 0 when it is off, which leaves TinyFugue's own
 * TINYFUGUE / ANSI-ATTR / ANSI / UNKNOWN list in charge (the default for every
 * user).  The MTTS convention: 1st reply the client name, 2nd the terminal
 * type, 3rd "MTTS <bit vector>", and the last reply repeats to say "that is
 * all", so *step is clamped to 2.
 *   on     what TinyFugue itself can display (ANSI, UTF-8 if built with it,
 *          256 colours if built with them)
 *   client additionally claims 256 colours and truecolor, for a remote client
 *          that renders them (TinyFugue passes the raw text through) */
int squat_mtts_reply(int *step, char *buf, size_t n)
{
    const char *m = getvar("squat_mtts");
    int client, bits = 1;	/* 1 = ANSI */

    if (!m || !*m || !strcmp(m, "off") || !strcmp(m, "0") || !strcmp(m, "no")) return 0;
    client = !strcmp(m, "client");
    if (*step > 2) *step = 2;
    if (*step == 0) {
	snprintf(buf, n, "TINYFUGUE");
    } else if (*step == 1) {
	snprintf(buf, n, "%s", (NCOLORS == 256 || client) ? "XTERM-256COLOR" : "ANSI");
    } else {
#if WIDECHAR
	bits |= 4;		/* UTF-8 */
#endif
	if (NCOLORS == 256 || client) bits |= 8;	/* 256 colours */
	if (client) bits |= 256;	/* truecolor */
	if (squat_fd >= 0) bits |= 128;	/* PROXY: a bridge is attached */
	snprintf(buf, n, "MTTS %d", bits);
    }
    return 1;
}

const char *squat_origin_name(void)
{
    return squat_in_remote_send && origin_buf[0] ? origin_buf : "tf";
}

/* ------------------------------------------------------------------ */
/* status indicator                                                    */
/* ------------------------------------------------------------------ */

static char last_status[64];
static int status_field_added = 0;
static int in_atexit_stop = 0;

static void run_status_command(const char *line)
{
    String *cmd = Stringnew(line, -1, 0);
    cmd->links++;
    macro_run(CS(cmd), 0, NULL, 0, sub, "\bSQUAT");
    Stringfree(cmd);
}

/* /set-like update of the squat_status variable, which a status field shows. */
static void set_status_var(const char *text)
{
    String *v;
    if (!strcmp(last_status, text)) return;
    snprintf(last_status, sizeof last_status, "%s", text);
    v = Stringnew(text, -1, 0);
    v->links++;
    setstrvar(findorcreateglobalvar("squat_status"), CS(v), FALSE);
    Stringfree(v);
}

static int ask_count(void);
static void ask_expire(void);

/* "sq:3 R1 A1 ?1": sessions, worlds a remote may type into, worlds whose
 * triggers a client runs, takeover requests waiting for you. */
static void update_status(void)
{
    char u[64];
    size_t i;
    int r = 0, a = 0, q = ask_count(), n;

    if (squat_fd < 0) { set_status_var(""); return; }
    for (i = 0; i < nknown; i++) {
	if (known[i].input != SQUAT_INPUT_TF) r++;
	if (known[i].automation == SQUAT_AUTO_CLIENT) a++;
    }
    n = snprintf(u, sizeof u, "sq:%ld", (long)nknown);
    if (r) n += snprintf(u + n, sizeof u - n, " R%d", r);
    if (a) n += snprintf(u + n, sizeof u - n, " A%d", a);
    if (q) snprintf(u + n, sizeof u - n, " ?%d", q);
    set_status_var(u);
}

static const char *hello_features(void)
{
    static char f[128];
    snprintf(f, sizeof f, "lines,control,worlds,worldedit%s%s%s%s%s",
#if ENABLE_GMCP
	",gmcp",
#else
	"",
#endif
#if ENABLE_MSDP
	",msdp",
#else
	"",
#endif
	exec_on() ? ",exec" : "", policy_mode() == POLICY_ASK ? ",ask" : "",
	scripts_mode() != SCRIPTS_OFF ? ",scripts" : "");
    return f;
}

/* ------------------------------------------------------------------ */
/* per-loop socket diff                                                */
/* ------------------------------------------------------------------ */

static void tick_cb(const SquatSock *s, void *u)
{
    Known *k = known_find(s->serial);
    int changed;

    (void)u;
    if (!k) { announce(s); return; }
    k->seen = 1;
    if (s->state == SQUAT_ST_CONNECTED) k->was_connected = 1;
    changed = k->state != s->state || k->remote_echo != s->remote_echo ||
	k->fg != s->fg || k->tls != s->tls || strcmp(k->world, s->world) != 0;
    if (changed) {
	snprintf(k->world, sizeof k->world, "%s", s->world);
	k->state = s->state;
	k->remote_echo = s->remote_echo;
	k->fg = s->fg;
	k->tls = s->tls;
	emit_sess(s, k);
    }
    if (k->input != s->input || k->automation != s->automation) {
	k->input = s->input;
	k->automation = s->automation;
	emit_ctrl(s, "");
    }
}

static void reap_children(void)
{
    int st;
    if (reap_pid > 0 && waitpid(reap_pid, &st, WNOHANG) != 0) reap_pid = -1;
    if (bridge_pid > 0) {
	pid_t r = waitpid(bridge_pid, &st, WNOHANG);
	if (r == bridge_pid || (r < 0 && errno == ECHILD)) {
	    bridge_pid = -1;
	    if (squat_fd >= 0) squat_stop("bridge exited");
	}
    }
}

static void check_worlds_changed(void);

void squat_tick(void)
{
    size_t i;

    if (reap_pid > 0 || bridge_pid > 0) reap_children();
    if (squat_fd < 0 || !bridge_ready) return;
    ask_expire();
    if (squat_fd < 0) return;
    for (i = 0; i < nknown; i++) known[i].seen = 0;
    squat_sock_each(tick_cb, NULL);
    squat_sock_scan_prompts();
    for (i = 0; i < nknown; ) {
	if (!known[i].seen) {
	    Buf b = { 0 };
	    buf_str(&b, "SESSX");
	    buf_field(&b, known[i].sid);
	    send_msg(&b);
	    buf_free(&b);
	    if (squat_fd < 0) return;	/* send_msg dropped the bridge */
	    known_del(&known[i]);
	} else i++;
    }
    check_worlds_changed();
    update_status();
    out_flush();
}

/* ------------------------------------------------------------------ */
/* worlds                                                              */
/* ------------------------------------------------------------------ */

/* A hash of everything about the named worlds that a client lists, to notice that the
 * list changed no matter who changed it (the TF keyboard, a macro file, a remote client). */
static unsigned long worlds_hash;
static int worlds_hash_init;

static void hash_str(unsigned long *h, const char *s)
{
    for (; s && *s; s++) *h = (*h * 16777619UL) ^ (unsigned char)*s;
    *h = (*h * 16777619UL) ^ 0xff;
}

static void worldhash_cb(World *w)
{
    if (w->flags & WORLD_TEMP) return;
    hash_str(&worlds_hash, w->name);
    hash_str(&worlds_hash, w->host);
    hash_str(&worlds_hash, w->port);
    hash_str(&worlds_hash, w->type);
    hash_str(&worlds_hash, w->character);
    hash_str(&worlds_hash, w->mfile);
    hash_str(&worlds_hash, (w->pass && *w->pass) ? "p" : "");
    worlds_hash = (worlds_hash * 16777619UL) ^ (unsigned long)(w->flags & (WORLD_SSL | WORLD_ECHO));
}

/* Tell the bridge when the world list changed. */
static void check_worlds_changed(void)
{
    unsigned long before = worlds_hash;

    worlds_hash = 2166136261UL;
    mapworld(worldhash_cb);
    if (worlds_hash_init && worlds_hash != before) {
	Buf b = { 0 };
	buf_str(&b, "WCHG");
	send_msg(&b);
	buf_free(&b);
    }
    worlds_hash_init = 1;
}

static void world_cb(World *w)
{
    Buf b = { 0 };

    buf_str(&b, "WORLD");
    buf_field(&b, w->name);
    buf_field(&b, w->host);
    buf_field(&b, w->port);
    buf_field(&b, w->type);
    buf_field(&b, w->character);
    buf_num(&b, (w->pass && *w->pass) ? 1 : 0);	/* the password itself is never sent */
    buf_num(&b, (w->flags & WORLD_SSL) ? 1 : 0);
    buf_num(&b, (w->flags & WORLD_ECHO) ? 1 : 0);
#if WIDECHAR
    buf_field(&b, w->charset);
#else
    buf_field(&b, "");
#endif
    buf_field(&b, w->mfile);
    buf_num(&b, (w->flags & WORLD_TEMP) ? 1 : 0);
    send_msg(&b);
    buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* commands from the bridge                                            */
/* ------------------------------------------------------------------ */

static void unescape(char *s)
{
    char *w = s;

    while (*s) {
	if (*s == '\\' && s[1]) {
	    s++;
	    if (*s == 'n') *w++ = '\n';
	    else if (*s == 't') *w++ = '\t';
	    else if (*s == 'r') *w++ = '\r';
	    else if (*s == 'x' && isxdigit((unsigned char)s[1]) &&
		isxdigit((unsigned char)s[2]))
	    {
		char h[3] = { s[1], s[2], 0 };
		*w++ = (char)strtol(h, NULL, 16);
		s += 2;
	    } else *w++ = *s;
	    s++;
	} else *w++ = *s++;
    }
    *w = '\0';
}

#define MAXF 16

static int split_fields(char *line, char **f)
{
    int n = 0;
    char *p = line;

    for (;;) {
	if (n < MAXF) f[n++] = p;
	p = strchr(p, '\t');
	if (!p) break;
	*p++ = '\0';
    }
    return n;
}

static void notice(const char *text)
{
    oprintf("%% squat: %s", text);
}

/* ---- takeover requests ---- */

typedef struct Ask {
    int used;
    char reqid[32];
    char sid[160];		/* a session id, or "*" */
    int input, automation;	/* requested change (-1 = none) */
    char device[64];
    long long deadline;
} Ask;

#define MAXASK 8
static Ask asks[MAXASK];

int squat_ask_pending(void)
{
    int i;
    for (i = 0; i < MAXASK; i++)
	if (asks[i].used) return 1;
    return 0;
}

static int ask_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAXASK; i++) n += asks[i].used;
    return n;
}

/* Apply a control change to one session or all ("*"). Replies to reqid. */
static void apply_control(const char *reqid, const char *sid, int input,
    int automation, const char *device)
{
    SquatSock s;

    if (!strcmp(sid, "*")) {
	size_t i, n = nknown;
	for (i = 0; i < n; i++)
	    if (squat_sock_info(known[i].serial, &s))
		squat_sock_set_control(s.serial, input, automation);
    } else {
	if (!sid_lookup(sid, &s)) { reply_err(reqid, "no_such_session", "no such session"); return; }
	squat_sock_set_control(s.serial, input, automation);
    }
    if (input >= 0 && device[0]) {
	char m[200];
	snprintf(m, sizeof m, "%s: input control of %s is now %s", device, sid, input_name(input));
	notice(m);
    }
    reply_ok(reqid);
}

static void ask_drop(Ask *a) { a->used = 0; }

static void ask_resolve(Ask *a, int allow, const char *why)
{
    if (allow) apply_control(a->reqid, a->sid, a->input, a->automation, a->device);
    else reply_err(a->reqid, "denied", why);
    ask_drop(a);
}

static void ask_expire(void)
{
    int i;
    long long t = now_ms();
    for (i = 0; i < MAXASK; i++)
	if (asks[i].used && t >= asks[i].deadline && squat_fd >= 0)
	    ask_resolve(&asks[i], 0, "nobody at TinyFugue answered in time");
}

static void cmd_ctrl_set(char **f, int nf)
{
    /* CTRL_SET reqid sid axis mode device */
    int input = -1, automation = -1, takeover;
    const char *axis, *mode;
    int pol, i, secs;
    Buf b = { 0 };

    if (nf < 6) { reply_err(f[1], "bad_request", "CTRL_SET needs 5 fields"); return; }
    axis = f[3];
    mode = f[4];
    if (!strcmp(axis, "input")) {
	if (!strcmp(mode, "tf")) input = SQUAT_INPUT_TF;
	else if (!strcmp(mode, "remote")) input = SQUAT_INPUT_REMOTE;
	else if (!strcmp(mode, "both")) input = SQUAT_INPUT_BOTH;
	else { reply_err(f[1], "bad_request", "bad input mode"); return; }
    } else if (!strcmp(axis, "automation")) {
	if (!strcmp(mode, "tf")) automation = SQUAT_AUTO_TF;
	else if (!strcmp(mode, "client")) automation = SQUAT_AUTO_CLIENT;
	else { reply_err(f[1], "bad_request", "bad automation mode"); return; }
    } else { reply_err(f[1], "bad_request", "unknown axis"); return; }

    takeover = input == SQUAT_INPUT_REMOTE || input == SQUAT_INPUT_BOTH ||
	automation == SQUAT_AUTO_CLIENT;
    pol = takeover ? policy_mode() : POLICY_AUTO;
    if (pol == POLICY_OFF) {
	reply_err(f[1], "denied", "takeover is disabled (squat_takeover=off)");
	return;
    }
    if (pol == POLICY_AUTO) {
	apply_control(f[1], f[2], input, automation, f[5]);
	return;
    }
    /* POLICY_ASK: the TinyFugue user decides with /control allow | deny */
    for (i = 0; i < MAXASK && asks[i].used; i++) ;
    if (i == MAXASK) { reply_err(f[1], "busy", "too many takeover requests are waiting"); return; }
    secs = atoi(var_or_default("squat_ask_timeout", "30"));
    if (secs < 1) secs = 30;
    memset(&asks[i], 0, sizeof asks[i]);
    asks[i].used = 1;
    snprintf(asks[i].reqid, sizeof asks[i].reqid, "%s", f[1]);
    snprintf(asks[i].sid, sizeof asks[i].sid, "%s", f[2]);
    snprintf(asks[i].device, sizeof asks[i].device, "%s", f[5]);
    asks[i].input = input;
    asks[i].automation = automation;
    asks[i].deadline = now_ms() + (long long)secs * 1000;
    buf_str(&b, "ASK");
    buf_field(&b, f[2]);
    buf_field(&b, axis);
    buf_field(&b, mode);
    buf_field(&b, f[5]);
    send_msg(&b);
    buf_free(&b);
    oprintf("%% squat: %s wants %s control of %s (%s): /control allow  or  /control deny  (%d s)",
	f[5][0] ? f[5] : "a device", axis, f[2], mode, secs);
}

static int run_tf_command(const char *line);

/* WORLD_PUT reqid name host port type char passmode pass ssl echo charset mfile mfilemode save */
static void cmd_world_put(char **f, int nf)
{
    char err[160] = "";
    int mode, rc;
    char old_mfile[1024] = "";	/* a copy: squat_world_put frees the stored one */
    World *w;

    if (nf < 15) { reply_err(nf > 1 ? f[1] : "", "bad_request", "WORLD_PUT needs 14 fields"); return; }
    mode = !strcmp(f[7], "set") ? 1 : !strcmp(f[7], "clear") ? 2 : 0;
    if (!strcmp(f[13], "set")) {
	/* a macro file is code TF loads on connect: only with squat_exec=on (not allow) */
	if (exec_mode() != EXEC_ON) {
	    reply_err(f[1], "forbidden", "setting a macro file needs squat_exec=on in TinyFugue");
	    return;
	}
    } else if ((w = find_world(f[2]))) {
	if (w->mfile) snprintf(old_mfile, sizeof old_mfile, "%s", w->mfile);	/* keep it: the client may not have seen it */
    }
    rc = squat_world_put(f[2], f[3], f[4], f[5], f[6], f[8], mode,
	!strcmp(f[13], "set") ? f[12] : old_mfile,
	f[9][0] == '1', f[10][0] == '1', f[11], err, sizeof err);
    if (rc < 0) { reply_err(f[1], "bad_request", err); return; }
    {
	char m[200];
	snprintf(m, sizeof m, "world %s %s remotely", f[2], rc ? "created" : "updated");
	notice(m);
    }
    if (f[14][0] == '1') run_tf_command("/saveworld");
    reply_ok2(f[1], rc ? "created" : "updated");
}

/* ---- running a TF command for a remote admin ---- */

static int run_tf_command(const char *line)
{
    String *cmd = Stringnew(line, -1, 0);
    int ok;

    cmd->links++;
    ok = macro_run(CS(cmd), 0, NULL, 0, sub, "\bSQUAT");
    Stringfree(cmd);
    return ok;
}

#define EXEC_MAX_LINES 200
#define EXEC_MAX_BYTES (60 * 1024)

/* EXEC reqid line: run it and send back what it printed. */
static void exec_captured(const char *reqid, const char *cmdline, int sandboxed_load);

static void cmd_exec(char **f, int nf)
{
    if (nf < 3) { reply_err(nf > 1 ? f[1] : "", "bad_request", "EXEC needs 2 fields"); return; }
    if (!exec_on()) {
	reply_err(f[1], "forbidden", "remote commands are off: /set squat_exec=on in TinyFugue");
	return;
    }
    if (f[2][0] != '/') { reply_err(f[1], "bad_request", "a command must start with /"); return; }
    if (exec_mode() == EXEC_ALLOW && !exec_allowed(f[2])) {
	reply_err(f[1], "forbidden", "that command is not in squat_exec_allow in TinyFugue (squat_exec=allow)");
	return;
    }
    exec_captured(f[1], f[2], 0);
}

/* Load an uploaded script with shell, files, new connections and Lua off. Every macro it
 * defines is flagged and keeps running that way (see do_macro). The level is put back
 * afterwards unless the script raised it itself (which cannot be undone). */
static int sandbox_load(const char *path, int sandboxed)
{
    int old = restriction, ok;
    if (!sandboxed) return do_file_load(path, FALSE, NULL) > 0;
    squat_sandbox++;
    if (restriction < SANDBOX_LEVEL) restriction = SANDBOX_LEVEL;
    ok = do_file_load(path, FALSE, NULL) > 0;
    squat_sandbox--;
    if (restriction == SANDBOX_LEVEL && old < SANDBOX_LEVEL) restriction = old;
    return ok;
}

/* Run a TinyFugue command line and send back OK reqid ok out... (ok=0 when it
 * printed anything on the error stream). The caller has already checked policy. */
static void exec_captured(const char *reqid, const char *cmdline, int sandboxed_load)
{
    TFILE *q, *qe, *old_out, *old_err;
    String *ln;
    Buf b = { 0 };
    int ok, lines = 0, had_error = 0;
    size_t bytes = 0;

    /* TinyFugue reports a failed command as a message on its error stream and
     * still returns success, so keep the two streams apart: anything on the
     * error stream (an error or a warning) makes ok false. */
    q = tfopen(NULL, "q");
    qe = tfopen(NULL, "q");
    old_out = tfout;
    old_err = tferr;
    tfout = q;
    tferr = qe;
    if (sandboxed_load) ok = sandbox_load(cmdline, sandboxed_load == 1);
    else ok = run_tf_command((char *)cmdline);
    tfout = old_out;
    tferr = old_err;
    (ln = Stringnew(NULL, 0, 0))->links++;
    {
	Buf lbuf = { 0 };
	TFILE *src[2];
	int k;
	src[0] = q;
	src[1] = qe;
	for (k = 0; k < 2; k++) {
	    while (lines < EXEC_MAX_LINES && tfgetS(ln, src[k])) {
		if (k == 1) had_error = 1;
		if (bytes + ln->len > EXEC_MAX_BYTES) break;
		buf_field(&lbuf, ln->data);
		bytes += ln->len;
		lines++;
		Stringtrunc(ln, 0);
	    }
	}
	buf_str(&b, "OK");
	buf_field(&b, reqid);
	buf_field(&b, (ok && (sandboxed_load || !had_error)) ? "1" : "0"); /* a load reports its own result; its notices are not errors */
	if (lbuf.len) buf_put(&b, lbuf.p, lbuf.len);
	buf_free(&lbuf);
    }
    Stringfree(ln);
    tfclose(q);
    tfclose(qe);
    send_msg(&b);
    buf_free(&b);
}


/* Is a script upload / load permitted right now? On refusal sends the reply and returns 0. */
static int script_gate(const char *reqid)
{
    switch (scripts_mode()) {
    case SCRIPTS_OFF:
	reply_err(reqid, "forbidden", "script upload is off: /set squat_scripts=safe (or on) in TinyFugue");
	return 0;
    case SCRIPTS_SAFE:
	break;
    }
    return 1;
}

/* SCRIPT reqid check | load path : the bridge has put (or is about to put) a file in
 * the scripts directory. check = may a script be uploaded now; load = /load it.
 * Only a file that really is inside the scripts directory is ever loaded. */
static void cmd_script(char **f, int nf)
{
    char real[PATH_MAX + 1], base[PATH_MAX + 1];
    size_t n;
    const char *p;

    if (nf < 3) { reply_err(nf > 1 ? f[1] : "", "bad_request", "SCRIPT needs 2 fields"); return; }
    if (!script_gate(f[1])) return;
    if (!strcmp(f[2], "check")) { reply_ok(f[1]); return; }
    if (strcmp(f[2], "load") || nf < 4) { reply_err(f[1], "bad_request", "SCRIPT: check or load <path>"); return; }
    for (p = f[3]; *p; p++)
	if ((unsigned char)*p < 0x20) { reply_err(f[1], "bad_request", "bad path"); return; }
    if (!squat_scripts_dir[0] || !realpath(squat_scripts_dir, base) || !realpath(f[3], real)) {
	reply_err(f[1], "no_such_script", "no such script");
	return;
    }
    n = strlen(base);
    if (strncmp(real, base, n) != 0 || real[n] != '/' || strchr(real + n + 1, '/')) {
	reply_err(f[1], "forbidden", "only files directly in the scripts directory can be loaded");
	return;
    }
    /* safe: the script is sandboxed; on: it runs with TinyFugue's own rights */
    exec_captured(f[1], real, scripts_mode() == SCRIPTS_SAFE ? 1 : 2);
}

static void handle_message(char *line)
{
    char *f[MAXF];
    int nf, i;
    SquatSock s;

    nf = split_fields(line, f);
    for (i = 0; i < nf; i++) unescape(f[i]);

    if (!strcmp(f[0], "HELLO")) {
	bridge_ready = 1;
	return;
    }
    if (!strcmp(f[0], "SEND")) {
	/* SEND reqid sid device noecho text */
	int n;
	if (nf < 6) { reply_err(nf > 1 ? f[1] : "", "bad_request", "SEND needs 5 fields"); return; }
	if (!sid_lookup(f[2], &s)) { reply_err(f[1], "no_such_session", "no such session"); return; }
	if (s.state != SQUAT_ST_CONNECTED) { reply_err(f[1], "session_ended", "not connected"); return; }
	if (s.input == SQUAT_INPUT_TF) {
	    reply_err(f[1], "not_controller", "TinyFugue has control of this session");
	    return;
	}
	snprintf(origin_buf, sizeof origin_buf, "%s", f[3]);
	n = squat_sock_send(s.serial, f[5], strlen(f[5]), f[4][0] == '1');
	origin_buf[0] = '\0';
	if (n < 0) reply_err(f[1], "session_ended", "send failed");
	else reply_ok(f[1]);
	return;
    }
    if (!strcmp(f[0], "CONNECT")) {
	/* CONNECT reqid world host port ssl */
	char err[160] = "";
	unsigned long serial = 0;
	if (nf < 6) { reply_err(nf > 1 ? f[1] : "", "bad_request", "CONNECT needs 5 fields"); return; }
	if (squat_sock_connect(f[2], f[3], f[4], f[5][0] == '1', &serial, err, sizeof err)) {
	    char sid[160];
	    squat_tick();		/* announce the new session before we answer */
	    if (serial && squat_sock_info(serial, &s)) {
		make_sid(&s, sid, sizeof sid);
		reply_ok2(f[1], sid);
	    } else reply_ok(f[1]);
	} else reply_err(f[1], "connect_failed", err);
	return;
    }
    if (!strcmp(f[0], "DISC")) {
	Known *k;
	if (nf < 3) return;
	if (!sid_lookup(f[2], &s)) { reply_err(f[1], "no_such_session", "no such session"); return; }
	if ((k = known_find(s.serial))) k->quit_requested = 1;
	if (squat_sock_disc(s.serial) < 0) reply_err(f[1], "session_ended", "already ended");
	else reply_ok(f[1]);
	return;
    }
    if (!strcmp(f[0], "DISMISS")) {
	int r;
	if (nf < 3) return;
	if (!sid_lookup(f[2], &s)) { reply_ok(f[1]); return; }
	r = squat_sock_dismiss(s.serial);
	if (r == -2) reply_err(f[1], "bad_request", "session is not ended");
	else if (r == -3) reply_err(f[1], "busy", "still shown in TinyFugue; it will go when you switch away");
	else reply_ok(f[1]);
	return;
    }
    if (!strcmp(f[0], "CTRL_SET")) { cmd_ctrl_set(f, nf); return; }
    if (!strcmp(f[0], "WORLD_PUT")) { cmd_world_put(f, nf); return; }
    if (!strcmp(f[0], "WORLD_REN")) {
	/* WORLD_REN reqid old new save */
	char err[160] = "";
	int r;
	if (nf < 5) { reply_err(nf > 1 ? f[1] : "", "bad_request", "WORLD_REN needs 4 fields"); return; }
	r = squat_world_rename(f[2], f[3], err, sizeof err);
	if (r == -1) reply_err(f[1], "no_such_world", "no such world");
	else if (r < 0) reply_err(f[1], "bad_request", err);
	else {
	    char m[300];
	    snprintf(m, sizeof m, "world %s renamed to %s remotely", f[2], f[3]);
	    notice(m);
	    if (f[4][0] == '1') run_tf_command("/saveworld");
	    reply_ok(f[1]);
	}
	return;
    }
    if (!strcmp(f[0], "WORLD_DEL")) {
	/* WORLD_DEL reqid name */
	int r;
	if (nf < 3) { reply_err(nf > 1 ? f[1] : "", "bad_request", "WORLD_DEL needs 2 fields"); return; }
	r = squat_world_del(f[2]);
	if (r == -1) reply_err(f[1], "no_such_world", "no such world");
	else if (r == -2) reply_err(f[1], "busy", "the world is connected; disconnect it first");
	else {
	    char m[200];
	    snprintf(m, sizeof m, "world %s deleted remotely", f[2]);
	    notice(m);
	    reply_ok(f[1]);
	}
	return;
    }
    if (!strcmp(f[0], "GMCP_SEND")) {
	/* GMCP_SEND reqid sid text */
	int r;
	if (nf < 4) { reply_err(nf > 1 ? f[1] : "", "bad_request", "GMCP_SEND needs 3 fields"); return; }
	if (!sid_lookup(f[2], &s)) { reply_err(f[1], "no_such_session", "no such session"); return; }
	if (s.input == SQUAT_INPUT_TF) { reply_err(f[1], "not_controller", "TinyFugue has control of this session"); return; }
	r = squat_sock_gmcp(s.serial, f[3]);
	if (r == 0) reply_ok(f[1]);
	else if (r == -2) reply_err(f[1], "unsupported", "the game has not negotiated GMCP");
	else reply_err(f[1], "bad_request", "bad GMCP text");
	return;
    }
    if (!strcmp(f[0], "MSDP_SEND")) {
	/* MSDP_SEND reqid sid payload */
	int r;
	if (nf < 4) { reply_err(nf > 1 ? f[1] : "", "bad_request", "MSDP_SEND needs 3 fields"); return; }
	if (!sid_lookup(f[2], &s)) { reply_err(f[1], "no_such_session", "no such session"); return; }
	if (s.input == SQUAT_INPUT_TF) { reply_err(f[1], "not_controller", "TinyFugue has control of this session"); return; }
	r = squat_sock_msdp(s.serial, f[3], strlen(f[3]));
	if (r == 0) reply_ok(f[1]);
	else if (r == -2) reply_err(f[1], "unsupported", "the game has not negotiated MSDP");
	else reply_err(f[1], "bad_request", "bad MSDP payload");
	return;
    }
    if (!strcmp(f[0], "EXEC")) { cmd_exec(f, nf); return; }
    if (!strcmp(f[0], "SCRIPT")) { cmd_script(f, nf); return; }
    if (!strcmp(f[0], "RESIZE")) {
	/* RESIZE sid cols rows */
	if (nf >= 4 && sid_lookup(f[1], &s))
	    squat_set_size(s.serial, (unsigned)atoi(f[2]), (unsigned)atoi(f[3]));
	return;
    }
    if (!strcmp(f[0], "WORLDS")) {
	/* WORLDS reqid */
	mapworld(world_cb);
	reply_ok(nf > 1 ? f[1] : "");
	return;
    }
    if (!strcmp(f[0], "NOTICE")) { if (nf > 1) notice(f[1]); return; }
    if (!strcmp(f[0], "PAIRCODE")) {
	/* PAIRCODE code expires fingerprint hint */
	if (nf >= 5) {
	    oprintf("%% squat: pairing code  %s", f[1]);
	    oprintf("%% squat: expires       %s", f[2]);
	    oprintf("%% squat: fingerprint   %s", f[3]);
	    oprintf("%% squat: connect to    %s", f[4]);
	}
	return;
    }
    if (!strcmp(f[0], "DEVICE")) {
	/* DEVICE id name role lastSeen */
	if (nf >= 5) oprintf("%% squat: device %s  %-16s %-9s last seen %s", f[1], f[2], f[3], f[4]);
	return;
    }
    /* unknown messages are ignored for forward compatibility */
}

void squat_readable(void)
{
    char tmp[16384];
    ssize_t n;
    char *nl;
    size_t start;

    if (squat_fd < 0) return;
    n = read(squat_fd, tmp, sizeof tmp);
    if (n < 0) {
	if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
	squat_stop("control channel read failed");
	return;
    }
    if (n == 0) { squat_stop("bridge exited"); return; }
    buf_put(&inq, tmp, (size_t)n);

    start = 0;
    while (squat_fd >= 0 && (nl = (char *)memchr(inq.p + start, '\n', inq.len - start))) {
	*nl = '\0';
	handle_message(inq.p + start);
	start = (size_t)(nl - inq.p) + 1;
    }
    if (squat_fd < 0) return;
    if (start) {
	memmove(inq.p, inq.p + start, inq.len - start);
	inq.len -= start;
	inq.p[inq.len] = '\0';
    }
    if (inq.len > SQUAT_LINE_MAX) squat_stop("bridge sent an oversized message");
}

void squat_writable(void)
{
    out_flush();
}

/* ------------------------------------------------------------------ */
/* starting and stopping the bridge                                    */
/* ------------------------------------------------------------------ */

static void squat_stop(const char *why)
{
    if (squat_fd >= 0) {
	/* nobody is left to type or to run triggers: TinyFugue takes over again */
	squat_sock_reset_control();
	memset(asks, 0, sizeof asks);
	set_status_var("");
	if (status_field_added && !in_atexit_stop) {
	    run_status_command("/status_rm squat_status");
	    status_field_added = 0;
	}
	readers_clear(squat_fd);
	writers_clear(squat_fd);
	close(squat_fd);
	squat_fd = -1;
    }
    if (bridge_pid > 0) {
	kill(bridge_pid, SIGTERM);
	if (waitpid(bridge_pid, NULL, WNOHANG) == 0) reap_pid = bridge_pid;
	bridge_pid = -1;
    }
    bridge_ready = 0;
    buf_free(&outq);
    buf_free(&inq);
    outoff = 0;
    free(known);
    known = NULL;
    nknown = capknown = 0;
    if (why) oprintf("%% squat: %s", why);
}

static void squat_atexit(void)
{
    in_atexit_stop = 1;
    if (squat_fd >= 0) squat_stop(NULL);
}

void squat_init(void)
{
    squat_fd = -1;
}

void squat_shutdown(void)
{
    squat_stop(NULL);
}

static const char *var_or(const char *name, const char *dflt)
{
    const char *v = getvar(name);
    return (v && *v) ? v : dflt;
}

static int squat_start(void)
{
    int sv[2], i;
    char dir[1024], logpath[1100], port[16];
    const char *bin, *bind, *home, *dirvar, *plain;
    char *argv[16];
    int argc = 0;
    long maxfd;
    pid_t pid;
    unsigned char rnd[8];
    int rfd;

    if (squat_fd >= 0) { eprintf("squat is already on"); return 0; }
    if (restriction >= RESTRICT_SHELL) {
	eprintf("squat: not allowed in restricted mode");
	return 0;
    }
    /* sockets that are not in the foreground are only read when this is on */
    if (getvar("background") && (!strcmp(getvar("background"), "off") ||
	!strcmp(getvar("background"), "0")))
	oprintf("%% squat: warning: /set background=on is needed to see other worlds");

    bin = var_or("squat_bin", "tf-squatterd");
    bind = var_or("squat_bind", "127.0.0.1");
    snprintf(port, sizeof port, "%s", var_or("squat_port", "10328"));
    plain = var_or("squat_plain", "0");
    dirvar = getvar("squat_dir");
    if (dirvar && *dirvar) snprintf(dir, sizeof dir, "%s", dirvar);
    else {
	home = getenv("HOME");
	snprintf(dir, sizeof dir, "%s/.tf-squatter", home ? home : ".");
    }
    mkdir(dir, 0700);
    snprintf(squat_scripts_dir, sizeof squat_scripts_dir, "%s/scripts", dir);
    mkdir(squat_scripts_dir, 0700);
    snprintf(logpath, sizeof logpath, "%s/bridge.log", dir);

    argv[argc++] = (char *)bin;
    argv[argc++] = (char *)"serve";
    argv[argc++] = (char *)"--fd";
    argv[argc++] = (char *)"3";
    argv[argc++] = (char *)"--port";
    argv[argc++] = port;
    argv[argc++] = (char *)"--bind";
    argv[argc++] = (char *)bind;
    argv[argc++] = (char *)"--data-dir";
    argv[argc++] = dir;
    if (strcmp(plain, "0")) argv[argc++] = (char *)"--insecure-loopback";
    argv[argc++] = (char *)"--log-level";
    argv[argc++] = (char *)var_or("squat_log", "info");
    argv[argc] = NULL;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
	operror("squat: socketpair");
	return 0;
    }
    pid = fork();
    if (pid < 0) {
	operror("squat: fork");
	close(sv[0]);
	close(sv[1]);
	return 0;
    }
    if (pid == 0) {
	/* child: become tf-squatterd.  It must inherit ONLY the control fd
	 * (as fd 3): TF does not mark its game sockets close-on-exec. */
	sigset_t none;
	int nullfd, logfd;

	setsid();
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	nullfd = open("/dev/null", O_RDONLY);
	logfd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (logfd < 0) logfd = open("/dev/null", O_WRONLY);
	if (nullfd >= 0) dup2(nullfd, 0);
	if (logfd >= 0) { dup2(logfd, 1); dup2(logfd, 2); }
	if (sv[1] != 3) { dup2(sv[1], 3); }
	maxfd = sysconf(_SC_OPEN_MAX);
	if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
	for (i = 4; i < maxfd; i++) close(i);
	execvp(bin, argv);
	_exit(127);
    }
    close(sv[1]);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    squat_fd = sv[0];
    bridge_pid = pid;
    bridge_ready = 0;
    readers_set(squat_fd);
    if (!atexit_set) { atexit(squat_atexit); atexit_set = 1; }

    boot_epoch = (unsigned long)time(NULL);
    if ((rfd = open("/dev/urandom", O_RDONLY)) >= 0) {
	if (read(rfd, rnd, sizeof rnd) != (ssize_t)sizeof rnd) memset(rnd, 0, sizeof rnd);
	close(rfd);
    } else memset(rnd, 0, sizeof rnd);
    snprintf(instance, sizeof instance, "%02x%02x%02x%02x%02x%02x%02x%02x",
	rnd[0], rnd[1], rnd[2], rnd[3], rnd[4] ^ (unsigned)getpid(), rnd[5],
	rnd[6], rnd[7]);

    {
	Buf b = { 0 };
	char pidbuf[24];
	snprintf(pidbuf, sizeof pidbuf, "%ld", (long)getpid());
	buf_str(&b, "HELLO");
	buf_field(&b, "1");
	buf_field(&b, "tf");
	buf_field(&b, instance);
	buf_field(&b, pidbuf);
	buf_field(&b, hello_features());
	send_msg(&b);
	buf_free(&b);
    }
    oprintf("%% squat: starting %s (log: %s)", bin, logpath);
    if (!status_field_added) {
	run_status_command("/status_add squat_status:14");
	status_field_added = 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* /squat and /control                                                 */
/* ------------------------------------------------------------------ */

static void send_simple(const char *type, const char *arg)
{
    Buf b = { 0 };
    buf_str(&b, type);
    if (arg) buf_field(&b, arg);
    send_msg(&b);
    buf_free(&b);
}

static void squat_usage(void)
{
    oputs("% usage: /squat on | off | status | pair [viewer|operator|admin] | devices | revoke <id>");
}

struct Value *handle_squat_command(String *args, int offset)
{
    if (squat_sandbox > 0) { eprintf("restricted"); return 0; }
    char *cmd = args->data + offset, *arg;
    char word[32];
    size_t n = 0;

    while (*cmd == ' ') cmd++;
    while (cmd[n] && cmd[n] != ' ' && n < sizeof word - 1) { word[n] = cmd[n]; n++; }
    word[n] = '\0';
    arg = cmd + n;
    while (*arg == ' ') arg++;

    if (!*word || !strcmp(word, "status")) {
	oprintf("%% squat: %s%s", squat_fd >= 0 ? "on" : "off",
	    squat_fd >= 0 ? (bridge_ready ? ", bridge ready" : ", bridge starting") : "");
	if (squat_fd >= 0) {
	    oprintf("%% squat: %ld session(s) reported, instance %s",
		(long)nknown, instance);
	    send_simple("INFO", NULL);
	}
	return shareval(val_one);
    }
    if (!strcmp(word, "on")) return newint(squat_start());
    if (!strcmp(word, "off")) {
	if (squat_fd < 0) { oputs("% squat: already off"); return shareval(val_zero); }
	squat_stop("stopped");
	return shareval(val_one);
    }
    if (squat_fd < 0) { eprintf("squat is off; use /squat on"); return shareval(val_zero); }
    if (!strcmp(word, "pair")) { send_simple("PAIR", *arg ? arg : "operator"); return shareval(val_one); }
    if (!strcmp(word, "devices")) { send_simple("DEVICES", NULL); return shareval(val_one); }
    if (!strcmp(word, "revoke")) {
	if (!*arg) { squat_usage(); return shareval(val_zero); }
	send_simple("REVOKE", arg);
	return shareval(val_one);
    }
    squat_usage();
    return shareval(val_zero);
}

struct control_ctx { const char *world; int input, automation; int hit; };

static void control_apply_cb(const SquatSock *s, void *u)
{
    struct control_ctx *c = (struct control_ctx *)u;

    if (strcmp(c->world, "*") && strcmp(c->world, s->world)) return;
    squat_sock_set_control(s->serial, c->input, c->automation);
    c->hit++;
}

static void control_list_cb(const SquatSock *s, void *u)
{
    (void)u;
    oprintf("%% control: %-20s input=%-6s automation=%-6s %s", s->world,
	input_name(s->input), auto_name(s->automation),
	s->state == SQUAT_ST_ENDED ? "(ended)" : "");
}

static void squat_list_asks(void)
{
    int i, n = 0;
    for (i = 0; i < MAXASK; i++)
	if (asks[i].used)
	    oprintf("%% control: request %d: %s wants %s%s%s of %s", ++n, asks[i].device[0] ? asks[i].device : "a device",
		asks[i].input >= 0 ? "input=" : "automation=",
		asks[i].input >= 0 ? input_name(asks[i].input) : auto_name(asks[i].automation),
		"", asks[i].sid);
    if (!n) oputs("% control: no takeover requests are waiting");
}

/* /control allow|deny [n]   /control pending */
static struct Value *control_answer(const char *p)
{
    int allow = !strncmp(p, "allow", 5), n = 1, i, seen = 0;
    const char *arg = strchr(p, ' ');

    if (!strncmp(p, "pending", 7)) { squat_list_asks(); return shareval(val_one); }
    if (arg && atoi(arg) > 0) n = atoi(arg);
    for (i = 0; i < MAXASK; i++) {
	if (!asks[i].used || ++seen != n) continue;
	ask_resolve(&asks[i], allow, "the TinyFugue user said no");
	return shareval(val_one);
    }
    eprintf("no such takeover request");
    return shareval(val_zero);
}

struct Value *handle_control_command(String *args, int offset)
{
    if (squat_sandbox > 0) { eprintf("restricted"); return 0; }
    char *p = args->data + offset;
    char world[128], w2[32], w3[32];
    struct control_ctx c;
    int nw;

    while (*p == ' ') p++;
    if (!*p) {
	squat_sock_each(control_list_cb, NULL);
	if (ask_count()) squat_list_asks();
	return shareval(val_one);
    }
    if (!strncmp(p, "allow", 5) || !strncmp(p, "deny", 4) || !strncmp(p, "pending", 7))
	return control_answer(p);
    world[0] = w2[0] = w3[0] = '\0';
    nw = sscanf(p, "%127s %31s %31s", world, w2, w3);
    c.world = world;
    c.input = c.automation = -1;
    c.hit = 0;
    if (nw == 2) {
	if (!strcmp(w2, "tf")) c.input = SQUAT_INPUT_TF;
	else if (!strcmp(w2, "remote")) c.input = SQUAT_INPUT_REMOTE;
	else if (!strcmp(w2, "both")) c.input = SQUAT_INPUT_BOTH;
    } else if (nw == 3 && !strcmp(w2, "automation")) {
	if (!strcmp(w3, "tf")) c.automation = SQUAT_AUTO_TF;
	else if (!strcmp(w3, "client")) c.automation = SQUAT_AUTO_CLIENT;
    }
    if (c.input < 0 && c.automation < 0) {
	oputs("% usage: /control [<world>|* tf|remote|both]  or  /control <world>|* automation tf|client");
	return shareval(val_zero);
    }
    squat_sock_each(control_apply_cb, &c);
    if (!c.hit) { eprintf("no such world: %s", world); return shareval(val_zero); }
    return shareval(val_one);
}

#endif /* ENABLE_SQUAT */
