/* render.h - turn result rows into an HTTP/CGI response (C99) */
#ifndef CSQLPAGE_RENDER_H
#define CSQLPAGE_RENDER_H

#include "row.h"
#include "util.h"

typedef struct render_state render_t;

render_t *render_new(void);

/* Row sink: pass this (with the render_t as `user`) to db_run_script. */
void      render_row(const row_t *row, void *user);

/* Flush the complete CGI response (status line, headers, body) to stdout
   and free the renderer. Safe to call even if no rows were produced. */
void      render_finish(render_t *r);

/* Emit a complete error page (CGI 500) and free. */
void      render_error(render_t *r, const char *message);

#endif
