/*************************************************************************
 *  TinyFugue - programmable mud client
 *  Copyright (C) 1993, 1994, 1995, 1996, 1997, 1998, 1999, 2002, 2003, 2004, 2005, 2006-2007 Ken Keys
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
#include "world.h"
#include "socket.h"
#include "output.h"
#include "history.h"
#include "variable.h"
#include "restart.h"

#include <stdio.h>
#include <string.h>

#ifdef PLATFORM_UNIX
# include <unistd.h>
# include <fcntl.h>
#endif

#define RESTART_MAGIC "TFRESTART1"
#define RESTART_MAGIC_LEN 10
#define RESTART_SCREEN_VERSION 1
#define RESTART_MODE_NONE 0
#define RESTART_MODE_RESUME_FD 1
#define RESTART_MODE_RECONNECT 2
#define RESTART_NULL_LEN 0xffffffffUL

static int saved_argc = 0;
static char **saved_argv = NULL;

#ifdef PLATFORM_UNIX
typedef struct ReconnectWorld {
    char *name;
    int foreground;
    struct ReconnectWorld *next;
} ReconnectWorld;

static FILE *save_fp = NULL;
static int save_error = 0;
static unsigned long save_world_count = 0;

static int write_u32(FILE *fp, unsigned long value)
{
    unsigned char buf[4];
    buf[0] = (unsigned char)(value & 0xff);
    buf[1] = (unsigned char)((value >> 8) & 0xff);
    buf[2] = (unsigned char)((value >> 16) & 0xff);
    buf[3] = (unsigned char)((value >> 24) & 0xff);
    return fwrite(buf, 1, 4, fp) == 4;
}

static int write_s32(FILE *fp, long value)
{
    return write_u32(fp, (unsigned long)(value & 0xffffffffUL));
}

static int read_u32(FILE *fp, unsigned long *value)
{
    unsigned char buf[4];
    if (fread(buf, 1, 4, fp) != 4)
        return 0;
    *value = (unsigned long)buf[0] |
        ((unsigned long)buf[1] << 8) |
        ((unsigned long)buf[2] << 16) |
        ((unsigned long)buf[3] << 24);
    return 1;
}

static int read_s32(FILE *fp, long *value)
{
    unsigned long raw;
    if (!read_u32(fp, &raw))
        return 0;
    if (raw & 0x80000000UL)
        *value = -((long)(((~raw) + 1) & 0xffffffffUL));
    else
        *value = (long)raw;
    return 1;
}

static int write_string(FILE *fp, const char *s)
{
    unsigned long len;

    if (!s)
        return write_u32(fp, RESTART_NULL_LEN);
    len = (unsigned long)strlen(s);
    return write_u32(fp, len) && (len == 0 || fwrite(s, 1, len, fp) == len);
}

static char *read_string(FILE *fp)
{
    unsigned long len;
    char *s;

    if (!read_u32(fp, &len))
        return NULL;
    if (len == RESTART_NULL_LEN)
        return NULL;
    s = XMALLOC(len + 1);
    if (len && fread(s, 1, len, fp) != len) {
        FREE(s);
        return NULL;
    }
    s[len] = '\0';
    return s;
}

static int write_bytes(FILE *fp, const void *data, unsigned long len)
{
    return len == 0 || fwrite(data, 1, len, fp) == len;
}

static int read_bytes(FILE *fp, void *data, unsigned long len)
{
    return len == 0 || fread(data, 1, len, fp) == len;
}

static unsigned long screen_logical_line_count(Screen *screen)
{
    ListEntry *node;
    conString *last = NULL;
    unsigned long count = 0;

    for (node = screen->pline.head; node; node = node->next) {
        PhysLine *pl = node->datum;
        if (pl->str != last) {
            last = pl->str;
            count++;
        }
    }
    return count;
}

static int save_screen(FILE *fp, Screen *screen)
{
    ListEntry *node;
    conString *last = NULL;
    unsigned long line_count = screen_logical_line_count(screen);
    int ok;

    ok = write_u32(fp, RESTART_SCREEN_VERSION) &&
        write_u32(fp, (unsigned long)screen->maxlline) &&
        write_u32(fp, (unsigned long)screen->nnew) &&
        write_u32(fp, (unsigned long)screen->active) &&
        write_u32(fp, (unsigned long)screen->paused) &&
        write_u32(fp, line_count);
    for (node = screen->pline.head; ok && node; node = node->next) {
        PhysLine *pl = node->datum;
        if (pl->str == last)
            continue;
        last = pl->str;
        ok = write_u32(fp, (unsigned long)pl->str->attrs) &&
            write_string(fp, pl->str->data);
    }
    return ok;
}

static int save_empty_screen(FILE *fp, long maxlline)
{
    Screen *screen = new_screen(maxlline);
    int ok = save_screen(fp, screen);
    free_screen(screen);
    return ok;
}

static int restore_screen(FILE *fp, Screen *screen)
{
    unsigned long version, maxlline, nnew, active, paused, line_count, i;

    if (!read_u32(fp, &version) ||
        !read_u32(fp, &maxlline) ||
        !read_u32(fp, &nnew) ||
        !read_u32(fp, &active) ||
        !read_u32(fp, &paused) ||
        !read_u32(fp, &line_count))
        return 0;
    if (version != RESTART_SCREEN_VERSION)
        return 0;

    free_screen_lines(screen);
    init_list(&screen->pline);
    screen->maxlline = (int)maxlline;
    screen->npline = screen->nlline = 0;
    screen->nback = screen->nnew = 0;
    screen->nback_filtered = screen->nnew_filtered = 0;
    screen->top = screen->bot = screen->maxbot = NULL;
    screen->viewsize = 0;
    screen->paused = 0;
    screen->active = 0;

    for (i = 0; i < line_count; i++) {
        unsigned long attrs;
        char *data;
        String *line;

        if (!read_u32(fp, &attrs))
            return 0;
        data = read_string(fp);
        if (!data && ferror(fp))
            return 0;
        line = Stringnew(data ? data : "", -1, (attr_t)attrs);
        enscreen(screen, CS(line));
        Stringfree(line);
        if (data)
            FREE(data);
    }

    screen->nnew = nnew > (unsigned long)screen->nback ? screen->nback : (int)nnew;
    screen->nback_filtered = screen->nback;
    screen->nnew_filtered = screen->nnew;
    screen->active = !!active;
    screen->paused = !!paused;
    return 1;
}

static int save_socket_state(FILE *fp, World *world)
{
    RestartSockState state;
    int ok;

    restart_snapshot_socket(world, &state);
    ok =
        write_u32(fp, (unsigned long)state.mode) &&
        write_u32(fp, (unsigned long)state.fd) &&
        write_u32(fp, (unsigned long)state.foreground) &&
        write_u32(fp, (unsigned long)state.active) &&
        write_u32(fp, (unsigned long)state.constate) &&
        write_u32(fp, state.flags) &&
        write_u32(fp, (unsigned long)state.numquiet) &&
        write_u32(fp, (unsigned long)state.ttype) &&
        write_u32(fp, (unsigned long)state.attrs) &&
        write_u32(fp, (unsigned long)state.prepromptattrs) &&
        write_u32(fp, state.alert_id) &&
        write_s32(fp, (long)state.recv_time.tv_sec) &&
        write_s32(fp, (long)state.recv_time.tv_usec) &&
        write_s32(fp, (long)state.send_time.tv_sec) &&
        write_s32(fp, (long)state.send_time.tv_usec) &&
        write_s32(fp, (long)state.prompt_timeout.tv_sec) &&
        write_s32(fp, (long)state.prompt_timeout.tv_usec) &&
        write_u32(fp, (unsigned long)(unsigned char)state.fsastate) &&
        write_u32(fp, (unsigned long)(unsigned char)state.substate) &&
        write_bytes(fp, state.tn_us, RESTART_TELNET_VECTOR_BYTES) &&
        write_bytes(fp, state.tn_us_tog, RESTART_TELNET_VECTOR_BYTES) &&
        write_bytes(fp, state.tn_them, RESTART_TELNET_VECTOR_BYTES) &&
        write_bytes(fp, state.tn_them_tog, RESTART_TELNET_VECTOR_BYTES) &&
        write_u32(fp, (unsigned long)state.prompt_attrs) &&
        write_string(fp, state.prompt_data) &&
        write_string(fp, state.buffer_data);
    if (state.mode == RESTART_MODE_RESUME_FD && state.fd >= 0) {
        int fdflags = fcntl(state.fd, F_GETFD, 0);
        if (fdflags >= 0)
            fcntl(state.fd, F_SETFD, fdflags & ~FD_CLOEXEC);
    }
    restart_free_socket_state(&state);
    return ok;
}

static int restore_socket_state(FILE *fp, RestartSockState *state)
{
    unsigned long temp;
    long stemp;

    memset(state, 0, sizeof(*state));
    if (!read_u32(fp, &temp)) return 0;
    state->mode = (int)temp;
    if (!read_u32(fp, &temp)) return 0;
    state->fd = (int)temp;
    if (!read_u32(fp, &temp)) return 0;
    state->foreground = (int)temp;
    if (!read_u32(fp, &temp)) return 0;
    state->active = (int)temp;
    if (!read_u32(fp, &temp)) return 0;
    state->constate = (int)temp;
    if (!read_u32(fp, &temp)) return 0;
    state->flags = (unsigned int)temp;
    if (!read_u32(fp, &temp)) return 0;
    state->numquiet = (int)temp;
    if (!read_u32(fp, &temp)) return 0;
    state->ttype = (int)temp;
    if (!read_u32(fp, &temp)) return 0;
    state->attrs = (attr_t)temp;
    if (!read_u32(fp, &temp)) return 0;
    state->prepromptattrs = (attr_t)temp;
    if (!read_u32(fp, &temp)) return 0;
    state->alert_id = temp;
    if (!read_s32(fp, &stemp)) return 0;
    state->recv_time.tv_sec = stemp;
    if (!read_s32(fp, &stemp)) return 0;
    state->recv_time.tv_usec = stemp;
    if (!read_s32(fp, &stemp)) return 0;
    state->send_time.tv_sec = stemp;
    if (!read_s32(fp, &stemp)) return 0;
    state->send_time.tv_usec = stemp;
    if (!read_s32(fp, &stemp)) return 0;
    state->prompt_timeout.tv_sec = stemp;
    if (!read_s32(fp, &stemp)) return 0;
    state->prompt_timeout.tv_usec = stemp;
    if (!read_u32(fp, &temp)) return 0;
    state->fsastate = (char)temp;
    if (!read_u32(fp, &temp)) return 0;
    state->substate = (char)temp;
    if (!read_bytes(fp, state->tn_us, RESTART_TELNET_VECTOR_BYTES) ||
        !read_bytes(fp, state->tn_us_tog, RESTART_TELNET_VECTOR_BYTES) ||
        !read_bytes(fp, state->tn_them, RESTART_TELNET_VECTOR_BYTES) ||
        !read_bytes(fp, state->tn_them_tog, RESTART_TELNET_VECTOR_BYTES))
        return 0;
    if (!read_u32(fp, &temp)) return 0;
    state->prompt_attrs = (attr_t)temp;
    state->prompt_data = read_string(fp);
    if (ferror(fp)) return 0;
    state->buffer_data = read_string(fp);
    return !ferror(fp);
}

static void count_world_cb(World *world)
{
    (void)world;
    save_world_count++;
}

static void save_world_cb(World *world)
{
    int ok;

    if (save_error)
        return;
    ok =
        !write_string(save_fp, world->name) ||
        !write_u32(save_fp, (unsigned long)world->flags) ||
        !write_string(save_fp, world->character) ||
        !write_string(save_fp, world->pass) ||
        !write_string(save_fp, world->host) ||
        !write_string(save_fp, world->port) ||
        !write_string(save_fp, world->myhost) ||
        !write_string(save_fp, world->mfile) ||
        !write_string(save_fp, world->type) ||
#if WIDECHAR
        !write_string(save_fp, world->charset) ||
#else
        !write_string(save_fp, NULL) ||
#endif
        !(
            world->screen ?
                save_screen(save_fp, world->screen) :
                save_empty_screen(save_fp, hist_getsize(world->history))
        ) ||
        !save_socket_state(save_fp, world);
    save_error = ok;
}

static int queue_reconnect(ReconnectWorld **head, const char *name, int foreground)
{
    ReconnectWorld *node = XMALLOC(sizeof(*node));
    node->name = STRDUP(name);
    node->foreground = foreground;
    node->next = *head;
    *head = node;
    return 1;
}

static void free_reconnects(ReconnectWorld *head)
{
    ReconnectWorld *next;
    while (head) {
        next = head->next;
        FREE(head->name);
        FREE(head);
        head = next;
    }
}

void restart_set_argv(int argc, char **argv)
{
    int i;
    int out = 0;

    saved_argv = XMALLOC(sizeof(char *) * (argc + 1));
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--resume") == 0) {
            i++;
            continue;
        }
        if (strncmp(argv[i], "--resume=", 9) == 0)
            continue;
        saved_argv[out++] = STRDUP(argv[i]);
    }
    saved_argc = out;
    saved_argv[out] = NULL;
}

int restart_exec(void)
{
    char path[] = "/tmp/tf-restart.XXXXXX";
    char **argv;
    FILE *fp;
    int fd, i, rc;
    unsigned long world_count;
    const char *fg_world = fgname();

    if (!saved_argv || saved_argc <= 0) {
        eprintf("restart argv not initialized");
        return 0;
    }

    restart_flush_sockets();
    fd = mkstemp(path);
    if (fd < 0) {
        eprintf("mkstemp: %s", strerror(errno));
        return 0;
    }
    if (!(fp = fdopen(fd, "wb"))) {
        close(fd);
        unlink(path);
        eprintf("fdopen: %s", strerror(errno));
        return 0;
    }

    save_world_count = 0;
    mapworld(count_world_cb);
    world_count = save_world_count;
    if (fwrite(RESTART_MAGIC, 1, RESTART_MAGIC_LEN, fp) != RESTART_MAGIC_LEN ||
        !write_string(fp, fg_world ? fg_world : "") ||
        !write_u32(fp, world_count) ||
        !save_screen(fp, default_screen))
    {
        fclose(fp);
        unlink(path);
        eprintf("failed to write restart state");
        return 0;
    }

    save_fp = fp;
    save_error = 0;
    mapworld(save_world_cb);
    save_fp = NULL;
    if (save_error || fclose(fp) == EOF) {
        unlink(path);
        eprintf("failed to finalize restart state");
        return 0;
    }

    argv = XCALLOC(sizeof(char *) * (saved_argc + 3));
    for (i = 0; i < saved_argc; i++)
        argv[i] = saved_argv[i];
    argv[saved_argc] = "--resume";
    argv[saved_argc + 1] = path;
    argv[saved_argc + 2] = NULL;

    oflush();
    rc = execvp(saved_argv[0], argv);
    if (rc < 0) {
        unlink(path);
        eprintf("execvp: %s", strerror(errno));
    }
    FREE(argv);
    return 0;
}

int restart_resume(const char *path)
{
    FILE *fp;
    char magic[RESTART_MAGIC_LEN];
    char *fg_world_name = NULL;
    unsigned long world_count, i;
    ReconnectWorld *reconnects = NULL;
    int ok = 0;

    if (!(fp = fopen(path, "rb"))) {
        eprintf("%s: %s", path, strerror(errno));
        return 0;
    }
    if (fread(magic, 1, RESTART_MAGIC_LEN, fp) != RESTART_MAGIC_LEN ||
        memcmp(magic, RESTART_MAGIC, RESTART_MAGIC_LEN) != 0 ||
        (((fg_world_name = read_string(fp)) == NULL) && (ferror(fp) || feof(fp))) ||
        !read_u32(fp, &world_count) ||
        !restore_screen(fp, default_screen))
    {
        goto cleanup;
    }

    for (i = 0; i < world_count; i++) {
        char *name = NULL, *character = NULL, *pass = NULL, *host = NULL;
        char *port = NULL, *myhost = NULL, *mfile = NULL, *type = NULL;
        char *charset = NULL;
        unsigned long flags;
        RestartSockState sockstate;
        World *world;

        if (!(name = read_string(fp)) ||
            !read_u32(fp, &flags))
            goto cleanup;
        character = read_string(fp);
        pass = read_string(fp);
        host = read_string(fp);
        port = read_string(fp);
        myhost = read_string(fp);
        mfile = read_string(fp);
        type = read_string(fp);
        charset = read_string(fp);
        if (ferror(fp))
            goto cleanup;

        world = new_world(name, type ? type : "", host ? host : "", port ? port : "",
            character ? character : "", pass ? pass : "", mfile ? mfile : "",
            (int)flags, myhost ? myhost : "");
        if (!world)
            goto cleanup;
#if WIDECHAR
        if (charset && *charset) {
            if (world->charset) FREE(world->charset);
            world->charset = STRDUP(charset);
        }
#endif
        if (!world->screen)
            world->screen = new_screen(hist_getsize(world->history));
        if (!restore_screen(fp, world->screen))
            goto cleanup;
        if (!restore_socket_state(fp, &sockstate))
            goto cleanup;

        world->screen->active = !!sockstate.active;
        if (sockstate.mode == RESTART_MODE_RESUME_FD) {
            if (!restart_restore_socket(world, &sockstate)) {
                restart_free_socket_state(&sockstate);
                goto cleanup;
            }
        } else if (sockstate.mode == RESTART_MODE_RECONNECT) {
            queue_reconnect(&reconnects, world->name, sockstate.foreground);
        }
        restart_free_socket_state(&sockstate);

        FREE(name);
        if (character) FREE(character);
        if (pass) FREE(pass);
        if (host) FREE(host);
        if (port) FREE(port);
        if (myhost) FREE(myhost);
        if (mfile) FREE(mfile);
        if (type) FREE(type);
        if (charset) FREE(charset);
    }

    while (reconnects) {
        ReconnectWorld *node = reconnects;
        reconnects = reconnects->next;
        openworld(node->name, NULL,
            (node->foreground ? CONN_FG : CONN_BG) |
            (login ? CONN_AUTOLOGIN : 0) |
            (quietflag ? CONN_QUIETLOGIN : 0));
        FREE(node->name);
        FREE(node);
    }

    if (fg_world_name && *fg_world_name) {
        World *fg_world = find_world(fg_world_name);
        if (fg_world && fg_world->sock)
            restart_set_foreground(fg_world);
    }
    restart_recount_active();
    redraw();
    ok = 1;

cleanup:
    if (!ok)
        eprintf("failed to restore restart state from %s", path);
    if (fg_world_name)
        FREE(fg_world_name);
    free_reconnects(reconnects);
    fclose(fp);
    unlink(path);
    return ok;
}

#else

void restart_set_argv(int argc, char **argv)
{
    (void)argc;
    (void)argv;
}

int restart_exec(void)
{
    eprintf("online reboot is only supported on unix builds");
    return 0;
}

int restart_resume(const char *path)
{
    (void)path;
    eprintf("online reboot is only supported on unix builds");
    return 0;
}

#endif
