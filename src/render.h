/* render.h - turn result rows into an HTTP/CGI response (C99) */
#ifndef CSQLPAGE_RENDER_H
#define CSQLPAGE_RENDER_H

#include "io.h"
#include "row.h"
#include "util.h"

typedef struct render_state render_t;

render_t *render_new(void);

/* Row sink: pass this (with the render_t as `user`) to db_run_script. */
void      render_row(const row_t *row, void *user);

/* Write the complete response (status line, headers, body) to `io` and free
   the renderer. Safe to call even if no rows were produced. */
void      render_finish(render_t *r, io_t *io);

/* Write a complete error page (500) to `io` and free. */
void      render_error(render_t *r, io_t *io, const char *message);

/* Enable a debug footer reporting timing and peak memory. Call before
   render_finish / render_error. */
void      render_set_debug(render_t *r, double total_ms, double db_ms, long rss_kb);

#endif
