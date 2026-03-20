/*************************************************************************
 *  TinyFugue - programmable mud client
 *  Copyright (C) 1993, 1994, 1995, 1996, 1997, 1998, 1999, 2002, 2003, 2004, 2005, 2006-2007 Ken Keys
 *
 *  TinyFugue (aka "tf") is protected under the terms of the GNU
 *  General Public License.  See the file "COPYING" for details.
 ************************************************************************/

#ifndef SOCKET_H
#define SOCKET_H

/* socktime ids */
#define SOCK_RECV	0
#define SOCK_SEND	1

/* /connect flags */
#define CONN_AUTOLOGIN	0x01
#define CONN_QUIETLOGIN	0x02
#define CONN_SSL	0x04
#define CONN_BG		0x08
#define CONN_FG		0x10

#define RESTART_TELNET_VECTOR_BYTES 32

typedef struct RestartSockState {
    int mode;
    int fd;
    int foreground;
    int active;
    int constate;
    unsigned int flags;
    int numquiet;
    int ttype;
    attr_t attrs;
    attr_t prepromptattrs;
    unsigned long alert_id;
    struct timeval recv_time;
    struct timeval send_time;
    struct timeval prompt_timeout;
    char fsastate;
    char substate;
    unsigned char tn_us[RESTART_TELNET_VECTOR_BYTES];
    unsigned char tn_us_tog[RESTART_TELNET_VECTOR_BYTES];
    unsigned char tn_them[RESTART_TELNET_VECTOR_BYTES];
    unsigned char tn_them_tog[RESTART_TELNET_VECTOR_BYTES];
    char *prompt_data;
    attr_t prompt_attrs;
    char *buffer_data;
} RestartSockState;

extern String *incoming_text;
extern int quit_flag;
extern struct Sock *xsock;

extern void    main_loop(void);
extern void    init_sock(void);
extern int     sockecho(void);
extern int     is_active(int fd);
extern void    readers_clear(int fd);
extern void    readers_set(int fd);
extern struct timeval *socktime(const char *name, int dir);
extern int     tog_bg(Var *var);
extern int     tog_keepalive(Var *var);
extern int     openworld(const char *name, const char *port, int flags);
extern void    world_output(struct World *world, conString *line);
extern int     send_line(const char *s, unsigned int len, int eol_flag);
extern conString *fgprompt(void);
extern int     tog_lp(Var *var);
extern void    transmit_window_size(void);
extern int     local_echo(int flag);
extern int     handle_send_function(conString *string, const char *world,
                     const char *flags);
#if ENABLE_ATCP
extern int     handle_atcp_function(conString *string, const char *world);
#endif
#if ENABLE_GMCP
extern int     handle_gmcp_function(conString *string, const char *world);
#endif
#if ENABLE_OPTION102
extern int     handle_option102_function(conString *string, const char *world);
#endif
extern int     handle_fake_recv_function(conString *string, const char *world,
		    const char *flags);
extern int     is_connected(const char *worldname);
extern int     is_open(const char *worldname);
extern int     nactive(const char *worldname);
extern int     world_hook(const char *fmt, const char *name);

extern struct World *xworld(void);
extern int	     xsock_is_fg(void);
extern int	     have_active_socks(void);
extern void          xsock_alert_id(void);
extern const char   *fgname(void);
extern const char   *world_info(const char *worldname, const char *fieldname);
extern struct World *named_or_current_world(const char *name);
extern int restart_flush_sockets(void);
extern int restart_snapshot_socket(struct World *world, RestartSockState *state);
extern void restart_free_socket_state(RestartSockState *state);
extern int restart_restore_socket(struct World *world,
    const RestartSockState *state);
extern int restart_set_foreground(struct World *world);
extern void restart_recount_active(void);

#endif /* SOCKET_H */
