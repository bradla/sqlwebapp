/* fastcgi.c - FastCGI responder protocol (C99 + POSIX sockets). */
#include "fastcgi.h"
#include "map.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#define FCGI_LISTENSOCK 0   /* fd the web server passes the listen socket on */

/* Record types */
enum { FCGI_BEGIN_REQUEST = 1, FCGI_ABORT_REQUEST = 2, FCGI_END_REQUEST = 3,
       FCGI_PARAMS = 4, FCGI_STDIN = 5, FCGI_STDOUT = 6, FCGI_STDERR = 7,
       FCGI_GET_VALUES = 9, FCGI_GET_VALUES_RESULT = 10, FCGI_UNKNOWN_TYPE = 11 };
enum { FCGI_RESPONDER = 1 };
enum { FCGI_KEEP_CONN = 1 };
enum { FCGI_REQUEST_COMPLETE = 0, FCGI_UNKNOWN_ROLE = 3 };

#define MAX_CONTENT 0xFFFF  /* per-record content limit (2-byte length) */

/* ---- transport-backed io_t over a parsed FastCGI request --------------- */

typedef struct {
    io_t         base;
    const map_t *params;
    const char  *body;
    size_t       bodylen, bodypos;
    sb_t        *out;
} fcgi_io_t;

static const char *fcgi_env(io_t *self, const char *name) {
    /* Per-request FastCGI params win; fall back to the process environment so
       static config (SQLPAGE_WEB_ROOT, SQLPAGE_DATABASE_URL, ...) can be set
       once when launching the resident server. */
    const char *v = map_get(((fcgi_io_t *)self)->params, name);
    return v ? v : getenv(name);
}
static size_t fcgi_read(io_t *self, char *buf, size_t n) {
    fcgi_io_t *f = (fcgi_io_t *)self;
    size_t avail = f->bodylen - f->bodypos;
    if (n > avail) n = avail;
    memcpy(buf, f->body + f->bodypos, n);
    f->bodypos += n;
    return n;
}
static void fcgi_write(io_t *self, const char *buf, size_t n) {
    sb_write(((fcgi_io_t *)self)->out, buf, n);
}

/* ---- low-level socket I/O ---------------------------------------------- */

static int read_n(int fd, void *buf, size_t n) {
    char  *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;            /* EOF */
        got += (size_t)r;
    }
    return 0;
}

static int write_n(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t      sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        sent += (size_t)w;
    }
    return 0;
}

/* Read one FCGI record. content is malloc'd (len bytes) or NULL when len==0;
   caller frees. Returns 0 on success, -1 on EOF/error. */
static int read_record(int fd, int *type, int *req_id, char **content, size_t *len) {
    unsigned char h[8];
    size_t        clen, pad;

    if (read_n(fd, h, 8) != 0) return -1;
    *type   = h[1];
    *req_id = (h[2] << 8) | h[3];
    clen    = ((size_t)h[4] << 8) | h[5];
    pad     = h[6];
    *len    = clen;

    if (clen) {
        *content = xmalloc(clen);
        if (read_n(fd, *content, clen) != 0) { free(*content); return -1; }
    } else {
        *content = NULL;
    }
    if (pad) {
        char skip[256];
        while (pad) {
            size_t take = pad < sizeof skip ? pad : sizeof skip;
            if (read_n(fd, skip, take) != 0) { free(*content); return -1; }
            pad -= take;
        }
    }
    return 0;
}

static int write_record(int fd, int type, int req_id, const void *data, size_t len) {
    unsigned char h[8];
    h[0] = 1;                 /* version */
    h[1] = (unsigned char)type;
    h[2] = (unsigned char)(req_id >> 8);
    h[3] = (unsigned char)(req_id & 0xff);
    h[4] = (unsigned char)(len >> 8);
    h[5] = (unsigned char)(len & 0xff);
    h[6] = 0;                 /* padding */
    h[7] = 0;
    if (write_n(fd, h, 8) != 0) return -1;
    if (len && write_n(fd, data, len) != 0) return -1;
    return 0;
}

/* Write `data` as a record stream, then the terminating empty record. */
static int write_stream(int fd, int type, int req_id, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > MAX_CONTENT) chunk = MAX_CONTENT;
        if (write_record(fd, type, req_id, data + off, chunk) != 0) return -1;
        off += chunk;
    }
    return write_record(fd, type, req_id, NULL, 0);   /* end of stream */
}

static int end_request(int fd, int req_id, int proto_status) {
    unsigned char body[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    body[4] = (unsigned char)proto_status;
    return write_record(fd, FCGI_END_REQUEST, req_id, body, 8);
}

/* ---- name-value (PARAMS) decoding -------------------------------------- */

static int read_len(const unsigned char *d, size_t len, size_t *pos, size_t *out) {
    unsigned char b;
    if (*pos >= len) return -1;
    b = d[*pos];
    if ((b >> 7) == 0) {
        *out = b;
        *pos += 1;
    } else {
        if (*pos + 4 > len) return -1;
        *out = ((size_t)(b & 0x7f) << 24) | ((size_t)d[*pos + 1] << 16) |
               ((size_t)d[*pos + 2] << 8) | d[*pos + 3];
        *pos += 4;
    }
    return 0;
}

static void decode_params(map_t *m, const char *data, size_t len) {
    const unsigned char *d = (const unsigned char *)data;
    size_t pos = 0;
    while (pos < len) {
        size_t nlen, vlen;
        char  *name, *val;
        if (read_len(d, len, &pos, &nlen) != 0) break;
        if (read_len(d, len, &pos, &vlen) != 0) break;
        if (pos + nlen + vlen > len) break;
        name = xmalloc(nlen + 1);
        val  = xmalloc(vlen + 1);
        memcpy(name, d + pos, nlen);            name[nlen] = '\0';
        memcpy(val,  d + pos + nlen, vlen);     val[vlen]  = '\0';
        map_add(m, name, val);
        free(name);
        free(val);
        pos += nlen + vlen;
    }
}

/* ---- per-connection request loop --------------------------------------- */

static void serve_connection(int fd, void (*handle)(io_t *)) {
    for (;;) {                 /* loop while the client keeps the connection */
        sb_t  params_raw, stdin_buf;
        int   type, id, rc;
        char *content;
        size_t clen;
        int   req_id = 0, role = 0, keep = 0;
        int   got_begin = 0, params_done = 0, stdin_done = 0, aborted = 0;

        sb_init(&params_raw);
        sb_init(&stdin_buf);

        while ((rc = read_record(fd, &type, &id, &content, &clen)) == 0) {
            switch (type) {
            case FCGI_BEGIN_REQUEST:
                req_id = id;
                if (clen >= 3) {
                    role = (content[0] << 8) | (unsigned char)content[1];
                    keep = content[2] & FCGI_KEEP_CONN;
                }
                got_begin = 1;
                break;
            case FCGI_PARAMS:
                if (clen == 0) params_done = 1;
                else sb_write(&params_raw, content, clen);
                break;
            case FCGI_STDIN:
                if (clen == 0) stdin_done = 1;
                else sb_write(&stdin_buf, content, clen);
                break;
            case FCGI_ABORT_REQUEST:
                aborted = 1;
                break;
            case FCGI_GET_VALUES:
                write_record(fd, FCGI_GET_VALUES_RESULT, 0, NULL, 0);
                break;
            default: {
                unsigned char b[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                b[0] = (unsigned char)type;
                write_record(fd, FCGI_UNKNOWN_TYPE, 0, b, 8);
                break;
            }
            }
            free(content);
            if (aborted || (got_begin && params_done && stdin_done)) break;
        }

        /* Connection closed or errored before a full request arrived. */
        if (rc != 0 && !(got_begin && params_done && stdin_done)) {
            sb_free(&params_raw);
            sb_free(&stdin_buf);
            break;
        }

        if (aborted) {
            end_request(fd, req_id, FCGI_REQUEST_COMPLETE);
        } else if (role != FCGI_RESPONDER) {
            end_request(fd, req_id, FCGI_UNKNOWN_ROLE);
        } else {
            map_t     params;
            sb_t      out;
            fcgi_io_t fio;

            map_init(&params);
            decode_params(&params, params_raw.data, params_raw.len);
            sb_init(&out);

            fio.base.env   = fcgi_env;
            fio.base.read  = fcgi_read;
            fio.base.write = fcgi_write;
            fio.params     = &params;
            fio.body       = stdin_buf.data;
            fio.bodylen    = stdin_buf.len;
            fio.bodypos    = 0;
            fio.out        = &out;

            handle((io_t *)&fio);

            write_stream(fd, FCGI_STDOUT, req_id, out.data, out.len);
            end_request(fd, req_id, FCGI_REQUEST_COMPLETE);

            sb_free(&out);
            map_free(&params);
        }

        sb_free(&params_raw);
        sb_free(&stdin_buf);

        if (!keep) break;
    }
    close(fd);
}

/* ---- listen socket setup ----------------------------------------------- */

/* Open a listening socket from "host:port", ":port", or a unix path "/...".
   Returns the fd, or -1 on error. */
static int open_listen(const char *spec) {
    int fd;
    if (strchr(spec, '/')) {                 /* unix domain socket */
        struct sockaddr_un un;
        if (strlen(spec) >= sizeof un.sun_path) return -1;
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        memset(&un, 0, sizeof un);
        un.sun_family = AF_UNIX;
        strcpy(un.sun_path, spec);
        unlink(spec);
        if (bind(fd, (struct sockaddr *)&un, sizeof un) != 0) { close(fd); return -1; }
    } else {                                 /* TCP "[host]:port" */
        struct sockaddr_in in;
        const char *colon = strrchr(spec, ':');
        int  port = atoi(colon ? colon + 1 : spec);
        int  on = 1;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        memset(&in, 0, sizeof in);
        in.sin_family = AF_INET;
        in.sin_addr.s_addr = htonl(INADDR_ANY);  /* loopback/any; host part ignored */
        in.sin_port = htons((unsigned short)port);
        if (bind(fd, (struct sockaddr *)&in, sizeof in) != 0) { close(fd); return -1; }
    }
    if (listen(fd, 128) != 0) { close(fd); return -1; }
    return fd;
}

static int is_listen_socket(int fd) {
    struct sockaddr_storage sa;
    socklen_t len = sizeof sa;
    errno = 0;
    if (getpeername(fd, (struct sockaddr *)&sa, &len) < 0 && errno == ENOTCONN)
        return 1;                            /* a socket, but not connected */
    return 0;
}

int fcgi_should_serve(void) {
    const char *bind = getenv("SQLPAGE_FCGI_BIND");
    if (bind && *bind) return 1;
    return is_listen_socket(FCGI_LISTENSOCK);
}

int fcgi_serve(void (*handle)(io_t *io)) {
    const char *bind = getenv("SQLPAGE_FCGI_BIND");
    int listen_fd;

    signal(SIGPIPE, SIG_IGN);                /* survive client disconnects */

    if (bind && *bind) {
        listen_fd = open_listen(bind);
        if (listen_fd < 0) return 1;
    } else {
        listen_fd = FCGI_LISTENSOCK;
    }

    for (;;) {
        int conn = accept(listen_fd, NULL, NULL);
        if (conn < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        serve_connection(conn, handle);
    }
}
