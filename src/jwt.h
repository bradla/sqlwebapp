/* jwt.h - HS256 JSON Web Tokens: issue and verify (RFC 7519) (C99) */
#ifndef CSQLPAGE_JWT_H
#define CSQLPAGE_JWT_H

/* Issue a token with claims {"sub": sub, "iat": now, "exp": now + ttl}.
   Caller frees. */
char *jwt_sign(const char *secret, const char *sub, long now, long ttl);

/* Verify `token`: the header's alg must be HS256, the signature must match
   and `exp` must be present and in the future. `sub` must be a string. On
   success returns 0 and stores a malloc'd copy of sub in *sub; else -1. */
int   jwt_verify(const char *secret, const char *token, long now, char **sub);

#endif
