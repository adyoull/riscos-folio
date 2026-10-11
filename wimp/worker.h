/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio's page worker thread (see worker.c). */
#ifndef WORKER_H
#define WORKER_H

#include "folio.h"
#include <stddef.h>
#include <stdio.h>

enum { JOB_PAGE = 0, JOB_THUMB = 1, JOB_COVER = 2 };

typedef struct {
	void *owner;             /* the window */
	int kind;                /* JOB_PAGE, JOB_THUMB or JOB_COVER */
	folio_ctx *fc;
	folio_doc *doc;
	int page, gen;
	folio_page_mode mode;
	folio_loc loc;
	float ppp;               /* pixels per point */
	int px_w, px_h;
	int *area;               /* sprite area, header filled in; pixels go after it */
	size_t bytes;
	/* JOB_COVER: the worker opens path itself and draws page 1 px_w wide
	 * (at most px_h high: the sprite's height is set to what was drawn),
	 * and returns the title and author. */
	char path[256];
	char title[128], author[96];
	/* results */
	int ok, aborted, cs;
	char err[128];
} render_job;

int worker_init(void);
int worker_idle(void);              /* nothing queued, running or waiting to be taken */
int worker_busy(void);              /* queued or running */
int worker_submit(const render_job *j);
const render_job *worker_current(void);
/* Gives the worker up to cs centiseconds; returns 1 if a result is ready. */
int worker_slice(int cs);
int worker_take(render_job *out);
/* Stops and discards the job if it belongs to owner (any job if NULL). */
void worker_stop_for(void *owner);

void core_lock(void);
void core_unlock(void);
int core_trylock(void);
/* Stops the worker's page, then takes the lock (main thread). */
void core_now(void);

#endif
