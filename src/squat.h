/*************************************************************************
 *  TinyFugue - programmable mud client
 *
 *  squat: remote control of a running TinyFugue.
 *
 *  A helper process (tf-squatterd, started by "/squat on") speaks TLS and
 *  the TFSP protocol to remote clients and talks to this module over a local
 *  socketpair using the TFCTL line protocol.  See docs/TF-SQUATTER.md in the
 *  squatClientDaemon repository for the full design.
 *
 *  TinyFugue (aka "tf") is protected under the terms of the GNU
 *  General Public License.  See the file "COPYING" for details.
 ************************************************************************/

#ifndef SQUAT_H
#define SQUAT_H

#if ENABLE_SQUAT

/* Per-socket control mode (who may send to the game from where). */
#define SQUAT_INPUT_TF     0	/* only the TF keyboard (default) */
#define SQUAT_INPUT_REMOTE 1	/* only remote devices; TF keyboard locked */
#define SQUAT_INPUT_BOTH   2	/* either */

/* Who runs triggers for a world. */
#define SQUAT_AUTO_TF      0
#define SQUAT_AUTO_CLIENT  1

/* Line kinds handed to squat_line(). */
#define SQUAT_KIND_GAME    'g'
#define SQUAT_KIND_ECHO    'e'
#define SQUAT_KIND_SYSTEM  's'

/* Line flags handed to squat_line(). */
#define SQUAT_LF_GAG       0x01
#define SQUAT_LF_PROMPT    0x02
#define SQUAT_LF_REPLAY    0x04

/* Connection states handed to squat code by socket.c. */
#define SQUAT_ST_CONNECTING 0
#define SQUAT_ST_CONNECTED  1
#define SQUAT_ST_ENDED      2

/* A read-only snapshot of one socket, produced by socket.c. */
typedef struct SquatSock {
    unsigned long serial;	/* unique per connection, never reused */
    const char *world;
    const char *host;
    const char *port;
    int tls;
    int state;			/* SQUAT_ST_* */
    int remote_echo;		/* server said WILL ECHO (password prompt) */
    int fg;			/* is TF's foreground socket */
    int input;			/* SQUAT_INPUT_* */
    int automation;		/* SQUAT_AUTO_* */
    long long started_ms;	/* unix ms when this connection began */
} SquatSock;

/* ---- globals consulted by socket.c / keyboard.c ---- */
extern int squat_kbd_depth;	/* >0 while keyboard-originated input is running */
extern int squat_in_remote_send;/* set while applying a remote SEND */
extern int squat_silent;	/* set to suppress secho and recording for one send */
extern int squat_skip_output;	/* next world_output() was already reported */
extern int squat_fd;		/* control fd, or -1 */
#define squat_active()	(squat_fd >= 0)
extern const char *squat_origin_name(void); /* "tf" or the sending device */

/* ---- commands (cmdlist.h) ---- */
extern struct Value *handle_squat_command(String *args, int offset);
extern struct Value *handle_control_command(String *args, int offset);

/* ---- called from the main loop ---- */
extern void squat_init(void);
extern void squat_shutdown(void);
extern void squat_tick(void);		/* once per loop: socket diff, flush */
extern void squat_readable(void);	/* control fd is readable */
extern void squat_writable(void);	/* control fd is writable */

/* ---- event taps (cheap no-ops when the bridge is not running) ---- */
extern void squat_line(const SquatSock *s, int kind, int flags,
    const char *origin, const char *text, size_t len);
/* like squat_line, plus the text TF will display after triggers (or NULL) */
extern void squat_line_disp(const SquatSock *s, int kind, int flags,
    const char *origin, const char *text, size_t len,
    const char *disp, size_t displen);
extern int  squat_want_disp(void);
extern int  squat_mtts_reply(int *step, char *buf, size_t n);
extern void squat_gmcp(const SquatSock *s, const char *text);
extern int  squat_ask_pending(void);	/* a takeover request awaits the TF user */

/* ---- called by squat.c, implemented in socket.c ---- */
struct Sock;
extern void squat_sock_each(void (*cb)(const SquatSock *, void *), void *u);
extern int  squat_sock_info(unsigned long serial, SquatSock *out);
extern int  squat_sock_describe(struct Sock *sock, SquatSock *out);
extern int  squat_sock_send(unsigned long serial, const char *text, size_t len,
    int silent);
extern int  squat_sock_disc(unsigned long serial);
extern int  squat_sock_dismiss(unsigned long serial);
extern int  squat_sock_set_control(unsigned long serial, int input, int automation);
extern int  squat_sock_resend_naws(unsigned long serial);
extern void squat_sock_reset_control(void);
extern void squat_sock_replay(unsigned long serial);
extern int  squat_sock_connect(const char *world, const char *host,
    const char *port, int ssl, unsigned long *serial, char *err, size_t errlen);
extern int  squat_naws_override(const struct Sock *sock, unsigned int *cols,
    unsigned int *rows);
extern int  squat_sock_gmcp(unsigned long serial, const char *text);
extern int  squat_sock_msdp(unsigned long serial, const char *data, size_t len);
extern void squat_sock_msdp_event(struct Sock *sock, const char *data, int len);
extern void squat_msdp(const SquatSock *s, const char *data, size_t len);
extern void squat_sock_gmcp_event(struct Sock *sock, const char *text);
extern int  squat_sock_gate(void);	/* nonzero: this send_line must be refused */
extern int  squat_prompt_pending(void);	/* a partial line may soon become a prompt */
extern void squat_sock_scan_prompts(void);
extern void squat_set_size(unsigned long serial, unsigned int cols,
    unsigned int rows);

#endif /* ENABLE_SQUAT */
#endif /* SQUAT_H */
