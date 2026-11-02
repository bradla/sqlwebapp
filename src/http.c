/* http.c - HTTP/1.1 server (C99 + Linux epoll + POSIX threads). */
#define _GNU_SOURCE 1   /* accept4 */
#include "http.h"
#include "util.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <fcntl.h>

#define MAX_HEADER  (64 * 1024)
#define MAX_BODY    (1024 * 1024)

static void (*g_handle)(io_t *io);
static int    g_listen_fd = -1;

/* ---- one parsed request, exposed to the core as CGI variables ----------- */

enum { V_METHOD, V_PATH, V_QUERY, V_CLEN, V_CTYPE, V_AUTH, V_COOKIE, NVARS };
static const char *VAR_NAMES[NVARS] = {
    "REQUEST_METHOD", "PATH_INFO", "QUERY_STRING", "CONTENT_LENGTH",
    "CONTENT_TYPE", "HTTP_AUTHORIZATION", "HTTP_COOKIE"
};

typedef struct {
    io_t        base;
    char       *vars[NVARS];    /* owned, or NULL when absent */
    const char *body;
    size_t      bodylen, bodypos;
    sb_t        out;            /* CGI-style response from the handler */
} http_io_t;

static const char *hio_env(io_t *self, const char *name) {
    http_io_t *h = (http_io_t *)self;
    int        i;
    for (i = 0; i < NVARS; i++)
        if (strcmp(name, VAR_NAMES[i]) == 0) return h->vars[i];
    return getenv(name);        /* static config (SQLPAGE_*) */
}
static size_t hio_read(io_t *self, char *buf, size_t n) {
    http_io_t *h = (http_io_t *)self;
    size_t     avail = h->bodylen - h->bodypos;
    if (n > avail) n = avail;
    memcpy(buf, h->body + h->bodypos, n);
    h->bodypos += n;
    return n;
}
static void hio_write(io_t *self, const char *buf, size_t n) {
    sb_write(&((http_io_t *)self)->out, buf, n);
}

/* ---- connections -------------------------------------------------------- */

typedef struct {
    int    fd;
    sb_t   in;          /* bytes received, not yet consumed */
    sb_t   out;         /* response bytes not yet sent      */
    size_t out_pos;
    int    close_after; /* close once `out` drains          */
    int    peer_closed; /* client sent EOF                  */
} conn_t;

static char *dupn(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode a path ('+' stays '+', unlike in a query string). */
static char *path_decode(const char *s, size_t n) {
    char  *out = xmalloc(n + 1);
    size_t i, o = 0;
    for (i = 0; i < n; i++) {
        int hi, lo;
        if (s[i] == '%' && i + 2 < n &&
            (hi = hexval((unsigned char)s[i + 1])) >= 0 &&
            (lo = hexval((unsigned char)s[i + 2])) >= 0 && (hi | lo)) {   /* keep %00 */
            out[o++] = (char)(hi << 4 | lo);
            i += 2;
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = '\0';
    return out;
}

static int ieq_n(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        int x = (a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : a[i];
        int y = (b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : b[i];
        if (x != y) return 0;
    }
    return 1;
}

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";                 case 201: return "Created";
    case 204: return "No Content";         case 301: return "Moved Permanently";
    case 302: return "Found";              case 303: return "See Other";
    case 304: return "Not Modified";       case 400: return "Bad Request";
    case 401: return "Unauthorized";       case 403: return "Forbidden";
    case 404: return "Not Found";          case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";  case 422: return "Unprocessable Content";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";    default:  return "Status";
    }
}

/* Turn the handler's CGI response ("Status: N" + headers + body) into an
   HTTP/1.1 response appended to `out`. */
static void cgi_to_http(const sb_t *cgi, sb_t *out, int head_only, int closing) {
    const char *data = cgi->data, *end = data + cgi->len;
    const char *hend = strstr(data, "\r\n\r\n");
    const char *body = hend ? hend + 4 : end, *line;
    size_t      blen = (size_t)(end - body);
    int         status = 200;
    sb_t        hdrs;

    sb_init(&hdrs);
    for (line = data; hend && line < hend;) {
        const char *eol = strstr(line, "\r\n");
        size_t      len = (size_t)(eol - line);
        if (len > 7 && ieq_n(line, "Status:", 7)) status = atoi(line + 7);
        else if (len) { sb_write(&hdrs, line, len); sb_puts(&hdrs, "\r\n"); }
        line = eol + 2;
    }
    sb_printf(out, "HTTP/1.1 %d %s\r\n", status, reason(status));
    sb_write(out, hdrs.data, hdrs.len);
    if (status != 204 && status != 304) sb_printf(out, "Content-Length: %lu\r\n", (unsigned long)blen);
    if (closing) sb_puts(out, "Connection: close\r\n");
    sb_puts(out, "\r\n");
    if (!head_only && status != 204 && status != 304) sb_write(out, body, blen);
    sb_free(&hdrs);
}

static void simple_response(conn_t *c, int status) {
    sb_printf(&c->out, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
              status, reason(status));
    c->close_after = 1;
}

/* Parse and handle every complete request `in` holds, appending responses
   to `out`. Sets close_after when the connection must close once flushed. */
static void process(conn_t *c) {
    size_t consumed = 0;

    while (!c->close_after) {
        const char *buf = c->in.data + consumed;
        size_t      avail = c->in.len - consumed;
        const char *hend, *p, *eol, *sp1, *sp2, *qm;
        http_io_t   h;
        long        clen = 0;
        int         keep, chunked = 0, head_only, i;

        if (avail == 0) break;
        hend = strstr(buf, "\r\n\r\n");
        if (!hend) {
            if (avail > MAX_HEADER) simple_response(c, 400);
            break;
        }

        /* request line: METHOD SP target SP HTTP/1.x */
        eol = strstr(buf, "\r\n");
        sp1 = memchr(buf, ' ', (size_t)(eol - buf));
        sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1)) : NULL;
        if (!sp1 || !sp2 || eol - sp2 != 9 || strncmp(sp2 + 1, "HTTP/1.", 7) != 0) {
            simple_response(c, 400);
            break;
        }
        keep = sp2[8] == '1';                    /* HTTP/1.1 defaults to keep-alive */

        memset(&h, 0, sizeof h);
        h.base.env = hio_env; h.base.read = hio_read; h.base.write = hio_write;
        h.vars[V_METHOD] = dupn(buf, (size_t)(sp1 - buf));
        qm = memchr(sp1 + 1, '?', (size_t)(sp2 - sp1 - 1));
        h.vars[V_PATH]  = path_decode(sp1 + 1, (size_t)((qm ? qm : sp2) - sp1 - 1));
        h.vars[V_QUERY] = qm ? dupn(qm + 1, (size_t)(sp2 - qm - 1)) : dupn("", 0);

        /* headers */
        for (p = eol + 2; p < hend + 2; p = eol + 2) {
            const char *colon, *v, *ve;
            eol = strstr(p, "\r\n");
            colon = memchr(p, ':', (size_t)(eol - p));
            if (!colon) continue;
            for (v = colon + 1; v < eol && (*v == ' ' || *v == '\t'); v++) {}
            for (ve = eol; ve > v && (ve[-1] == ' ' || ve[-1] == '\t'); ve--) {}
#define IS(name) ((size_t)(colon - p) == sizeof name - 1 && ieq_n(p, name, sizeof name - 1))
            if (IS("Content-Length")) {
                char *num = dupn(v, (size_t)(ve - v)), *e;
                clen = strtol(num, &e, 10);
                if (*e || clen < 0) clen = -1;
                free(num);
            } else if (IS("Content-Type"))      { free(h.vars[V_CTYPE]);  h.vars[V_CTYPE]  = dupn(v, (size_t)(ve - v)); }
            else if (IS("Authorization"))       { free(h.vars[V_AUTH]);   h.vars[V_AUTH]   = dupn(v, (size_t)(ve - v)); }
            else if (IS("Cookie"))              { free(h.vars[V_COOKIE]); h.vars[V_COOKIE] = dupn(v, (size_t)(ve - v)); }
            else if (IS("Transfer-Encoding"))   chunked = 1;
            else if (IS("Connection")) {
                if ((size_t)(ve - v) == 5 && ieq_n(v, "close", 5)) keep = 0;
                if ((size_t)(ve - v) == 10 && ieq_n(v, "keep-alive", 10)) keep = 1;
            }
#undef IS
        }

        if (chunked || clen < 0 || clen > MAX_BODY) {
            simple_response(c, chunked ? 501 : clen < 0 ? 400 : 413);
            for (i = 0; i < NVARS; i++) free(h.vars[i]);
            break;
        }
        if ((size_t)(hend + 4 - buf) + (size_t)clen > avail) {   /* body incomplete */
            for (i = 0; i < NVARS; i++) free(h.vars[i]);
            break;
        }
        h.body    = hend + 4;
        h.bodylen = (size_t)clen;
        if (clen > 0) {
            char num[24];
            sprintf(num, "%ld", clen);
            h.vars[V_CLEN] = xstrdup(num);
        }
        head_only = strcmp(h.vars[V_METHOD], "HEAD") == 0;
        if (head_only) { free(h.vars[V_METHOD]); h.vars[V_METHOD] = xstrdup("GET"); }

        sb_init(&h.out);
        g_handle(&h.base);
        cgi_to_http(&h.out, &c->out, head_only, !keep);
        sb_free(&h.out);
        for (i = 0; i < NVARS; i++) free(h.vars[i]);

        consumed += (size_t)(hend + 4 - buf) + (size_t)clen;
        if (!keep) c->close_after = 1;
    }

    if (consumed) {                              /* drop handled bytes */
        memmove(c->in.data, c->in.data + consumed, c->in.len - consumed);
        c->in.len -= consumed;
        c->in.data[c->in.len] = '\0';
    }
}

static void conn_close(int ep, conn_t *c) {
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    sb_free(&c->in);
    sb_free(&c->out);
    free(c);
}

/* Send pending output. Returns -1 on a dead socket, 1 if bytes remain. */
static int flush_out(conn_t *c) {
    while (c->out_pos < c->out.len) {
        ssize_t n = send(c->fd, c->out.data + c->out_pos, c->out.len - c->out_pos, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            return -1;
        }
        c->out_pos += (size_t)n;
    }
    sb_clear(&c->out);
    c->out_pos = 0;
    return 0;
}

/* Hand the connection back to the shared epoll (registrations are one-shot,
   so exactly one thread owns a connection between wakeups). */
static void rearm(int ep, conn_t *c, int want_out) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT | (want_out ? EPOLLOUT : 0);
    ev.data.ptr = c;
    epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
}

/* Read what is available, handle complete requests, send responses. */
static void on_ready(int ep, conn_t *c, unsigned events) {
    int pending;

    if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        for (;;) {
            char    buf[16384];
            ssize_t n = recv(c->fd, buf, sizeof buf, 0);
            if (n > 0) {
                sb_write(&c->in, buf, (size_t)n);
                if ((size_t)n < sizeof buf) break;
            } else if (n == 0) {
                c->peer_closed = 1;
                break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                conn_close(ep, c);
                return;
            }
        }
    }

    /* Responses go out in order: new requests are only handled once the
       previous responses have been fully sent. */
    for (;;) {
        size_t before;
        pending = flush_out(c);
        if (pending < 0) { conn_close(ep, c); return; }
        if (pending > 0) break;
        if (c->close_after) { conn_close(ep, c); return; }
        before = c->in.len;
        if (before) process(c);
        if (c->out.len == 0 && c->in.len == before) {   /* nothing complete */
            if (c->peer_closed) { conn_close(ep, c); return; }
            break;
        }
    }
    rearm(ep, c, pending > 0);
}

/* Out of file descriptors: the pending connection would keep the listener
   readable and every worker would spin on it. Free a reserved descriptor,
   accept the connection and close it at once, then take the reserve back. */
static int             g_reserve_fd = -1;
static pthread_mutex_t g_reserve_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_warned_fds = 0;

static void shed_connection(void) {
    int fd;
    pthread_mutex_lock(&g_reserve_lock);
    if (!g_warned_fds) {
        g_warned_fds = 1;
        fprintf(stderr, "csqlpage: out of file descriptors; refusing connections (raise ulimit -n)\n");
    }
    if (g_reserve_fd >= 0) { close(g_reserve_fd); g_reserve_fd = -1; }
    fd = accept4(g_listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd >= 0) close(fd);
    g_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    pthread_mutex_unlock(&g_reserve_lock);
}

/* Every connection is a descriptor: lift the soft limit to the hard one. */
static void raise_nofile(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

/* Accept every pending connection into the shared epoll. */
static void accept_all(int ep) {
    struct epoll_event ev;
    for (;;) {
        int     fd = accept4(g_listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC), one = 1;
        conn_t *c;
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) shed_connection();
            break;                                   /* EAGAIN: none left */
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        c = xmalloc(sizeof *c);
        memset(c, 0, sizeof *c);
        c->fd = fd;
        sb_init(&c->in);
        sb_init(&c->out);
        ev.events   = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
        ev.data.ptr = c;
        if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) != 0) {
            close(fd); sb_free(&c->in); sb_free(&c->out); free(c);
        }
    }
    ev.events   = EPOLLIN | EPOLLONESHOT;
    ev.data.ptr = NULL;
    epoll_ctl(ep, EPOLL_CTL_MOD, g_listen_fd, &ev);
}

/* All workers wait on one epoll and take one ready connection at a time, so
   an idle worker always picks up the next request: a slow request (a bcrypt
   login) delays only its own connection, never others queued behind it. */
static void *worker(void *arg) {
    int ep = *(int *)arg;
    for (;;) {
        struct epoll_event ev;
        int                n = epoll_wait(ep, &ev, 1, -1);
        if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }
        if (n == 0) continue;
        if (ev.data.ptr == NULL) accept_all(ep);     /* NULL marks the listener */
        else                     on_ready(ep, ev.data.ptr, ev.events);
    }
    return NULL;
}

/* ---- startup ------------------------------------------------------------ */

int http_should_serve(void) {
    const char *b = getenv("SQLPAGE_HTTP_LISTEN");
    return b && *b;
}

static int open_listener(const char *spec) {
    char            *host = xstrdup(spec), *port = strrchr(host, ':');
    struct addrinfo  hints, *res = NULL, *ai;
    int              fd = -1, one = 1;

    if (!port) { fprintf(stderr, "SQLPAGE_HTTP_LISTEN must be host:port\n"); free(host); return -1; }
    *port++ = '\0';
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;
    if (getaddrinfo(*host ? host : NULL, port, &hints, &res) != 0) {
        fprintf(stderr, "cannot resolve %s\n", spec); free(host); return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 4096) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    free(host);
    if (fd < 0) perror("bind/listen");
    return fd;
}

int http_serve(void (*handle)(io_t *io)) {
    const char *t = getenv("SQLPAGE_THREADS");
    long        nthreads = t ? strtol(t, NULL, 10) : 4 * sysconf(_SC_NPROCESSORS_ONLN);
    long        i;
    pthread_t  *threads;
    int         ep;
    struct epoll_event ev;

    if (nthreads < 1) nthreads = 1;
    signal(SIGPIPE, SIG_IGN);
    raise_nofile();
    g_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_handle = handle;
    g_listen_fd = open_listener(getenv("SQLPAGE_HTTP_LISTEN"));
    if (g_listen_fd < 0) return 1;

    ep = epoll_create1(EPOLL_CLOEXEC);
    ev.events   = EPOLLIN | EPOLLONESHOT;
    ev.data.ptr = NULL;
    if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, g_listen_fd, &ev) != 0) { perror("epoll"); return 1; }

    threads = xmalloc((size_t)nthreads * sizeof *threads);
    for (i = 0; i < nthreads; i++) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1 << 20);
        if (pthread_create(&threads[i], &attr, worker, &ep) != 0) { perror("pthread_create"); return 1; }
        pthread_attr_destroy(&attr);
    }
    fprintf(stderr, "csqlpage: HTTP on %s, %ld threads\n", getenv("SQLPAGE_HTTP_LISTEN"), nthreads);
    for (i = 0; i < nthreads; i++) pthread_join(threads[i], NULL);
    return 0;
}
