/* jwt.c - implementation (C99) */
#include "jwt.h"
#include "json.h"
#include "sha256.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Unpadded base64url, as JWS requires. */
static void b64url_put(sb_t *b, const unsigned char *p, size_t n) {
    size_t i;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (unsigned)p[i + 1] << 8 | p[i + 2];
        sb_putc(b, B64URL[v >> 18]);
        sb_putc(b, B64URL[(v >> 12) & 63]);
        sb_putc(b, B64URL[(v >> 6) & 63]);
        sb_putc(b, B64URL[v & 63]);
    }
    if (n - i == 1) {
        unsigned v = (unsigned)p[i] << 16;
        sb_putc(b, B64URL[v >> 18]);
        sb_putc(b, B64URL[(v >> 12) & 63]);
    } else if (n - i == 2) {
        unsigned v = (unsigned)p[i] << 16 | (unsigned)p[i + 1] << 8;
        sb_putc(b, B64URL[v >> 18]);
        sb_putc(b, B64URL[(v >> 12) & 63]);
        sb_putc(b, B64URL[(v >> 6) & 63]);
    }
}

static int b64url_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/* Decode s[0..n) into `out` (cleared first). Rejects padding and stray bits. */
static int b64url_get(sb_t *out, const char *s, size_t n) {
    unsigned acc = 0;
    int      bits = 0;
    size_t   i;
    sb_clear(out);
    if (n % 4 == 1) return -1;
    for (i = 0; i < n; i++) {
        int v = b64url_val((unsigned char)s[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            sb_putc(out, (char)((acc >> bits) & 0xff));
        }
    }
    if (acc & ((1u << bits) - 1)) return -1;   /* non-canonical trailing bits */
    return 0;
}

char *jwt_sign(const char *secret, const char *sub, long now, long ttl) {
    static const char header[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    unsigned char     mac[32];
    sb_t              payload, tok;
    const char       *c;

    sb_init(&payload);
    sb_puts(&payload, "{\"sub\":\"");
    for (c = sub; *c; c++) {                  /* JSON-escape the subject */
        if (*c == '"' || *c == '\\') sb_putc(&payload, '\\');
        if ((unsigned char)*c < 0x20) sb_printf(&payload, "\\u%04x", (unsigned char)*c);
        else                          sb_putc(&payload, *c);
    }
    sb_printf(&payload, "\",\"iat\":%ld,\"exp\":%ld}", now, now + ttl);

    sb_init(&tok);
    b64url_put(&tok, (const unsigned char *)header, sizeof header - 1);
    sb_putc(&tok, '.');
    b64url_put(&tok, (const unsigned char *)payload.data, payload.len);
    hmac_sha256(secret, strlen(secret), tok.data, tok.len, mac);
    sb_putc(&tok, '.');
    b64url_put(&tok, mac, sizeof mac);

    sb_free(&payload);
    return tok.data;   /* ownership transferred */
}

int jwt_verify(const char *secret, const char *token, long now, char **sub) {
    const char   *dot1, *dot2, *v;
    size_t        vlen, i;
    unsigned char mac[32], diff = 0;
    sb_t          buf;
    int           rc = -1, type;

    *sub = NULL;
    dot1 = strchr(token, '.');
    if (!dot1) return -1;
    dot2 = strchr(dot1 + 1, '.');
    if (!dot2 || strchr(dot2 + 1, '.')) return -1;

    sb_init(&buf);

    /* Signature first: nothing in an unauthenticated token is trusted. */
    if (b64url_get(&buf, dot2 + 1, strlen(dot2 + 1)) != 0 || buf.len != sizeof mac) goto done;
    hmac_sha256(secret, strlen(secret), token, (size_t)(dot2 - token), mac);
    for (i = 0; i < sizeof mac; i++) diff |= (unsigned char)(mac[i] ^ (unsigned char)buf.data[i]);
    if (diff) goto done;

    /* Header: alg must be exactly HS256 (rejects "none" and alg confusion). */
    if (b64url_get(&buf, token, (size_t)(dot1 - token)) != 0) goto done;
    if (json_member(buf.data, buf.len, "alg", &v, &vlen) != 's' ||
        vlen != 5 || memcmp(v, "HS256", 5) != 0) goto done;

    /* Payload: exp required and in the future; sub required as a string. */
    if (b64url_get(&buf, dot1 + 1, (size_t)(dot2 - dot1 - 1)) != 0) goto done;
    if (json_member(buf.data, buf.len, "exp", &v, &vlen) != 'n') goto done;
    {
        char  num[64];
        if (vlen >= sizeof num) goto done;
        memcpy(num, v, vlen);
        num[vlen] = '\0';
        if (strtod(num, NULL) <= (double)now) goto done;
    }
    type = json_member(buf.data, buf.len, "sub", &v, &vlen);
    if (type != 's' || memchr(v, '\\', vlen)) goto done;
    *sub = xmalloc(vlen + 1);
    memcpy(*sub, v, vlen);
    (*sub)[vlen] = '\0';
    rc = 0;

done:
    sb_free(&buf);
    return rc;
}
