/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio's page worker: one thread that loads and draws pages, so a heavy
 * page (up to a second on a Pi, mostly reading its contents) never holds up
 * the desktop. The main thread gives it a time slice after each Wimp_Poll
 * (worker_slice) and can stop a page that's no longer wanted (folio_abort).
 *
 * MuPDF isn't used by two threads at once: every core call holds core_lock.
 * The worker takes it for each page; the main thread takes it for its own
 * core calls (opening, closing, counting with trylock, marks).
 *
 * Threads only run while Folio has the machine: UnixLib's ticker switches
 * threads as Wimp_Poll returns and on its own ticks (UnixLib 5.0.3.1 K9),
 * with PThreadTicker loaded by !Run. */
#include "worker.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <swis.h>
#ifdef __riscos__
#define yield() pthread_yield()       /* UnixLib */
#else
#include <sched.h>
#define yield() sched_yield()         /* the Linux test build */
#endif

enum { IDLE, QUEUED, RUNNING, DONE };

static pthread_mutex_t core_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t job_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static volatile int state = IDLE;
static render_job job;
static int started;

void core_lock(void) { pthread_mutex_lock(&core_mutex); }
void core_unlock(void) { pthread_mutex_unlock(&core_mutex); }
int core_trylock(void) { return pthread_mutex_trylock(&core_mutex) == 0; }

static unsigned now_cs(void)
{
	unsigned t;
	_swix(OS_ReadMonotonicTime, _OUT(0), &t);
	return t;
}

/* Draws the current job (in the worker, or in the caller without one). */
static void run_job(void)
{
	folio_irect tile;
	unsigned t0;
	int rc;
	t0 = now_cs();
	tile.x0 = 0;
	tile.y0 = 0;
	tile.x1 = job.px_w;
	tile.y1 = job.px_h;
	core_lock();
	rc = folio_render_tile(job.doc, job.loc, job.ppp, tile, job.mode, 1,
			(uint32_t *)(job.area + 4 + 11), job.px_w * 4);
	job.ok = rc == 0;
	job.aborted = rc != 0 && folio_aborted(job.doc);
	if (rc)
		snprintf(job.err, sizeof job.err, "%s", folio_error(job.fc));
	core_unlock();
	job.cs = (int)(now_cs() - t0);
}

static void *worker_main(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&job_mutex);
		while (state != QUEUED)
			pthread_cond_wait(&job_cond, &job_mutex);
		state = RUNNING;
		pthread_mutex_unlock(&job_mutex);
		run_job();
		pthread_mutex_lock(&job_mutex);
		state = DONE;
		pthread_mutex_unlock(&job_mutex);
	}
	return NULL;
}

int worker_init(void)
{
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	/* MuPDF's interpreter and rasteriser recurse; give them room. */
	pthread_attr_setstacksize(&attr, 1024 * 1024);
	if (pthread_create(&t, &attr, worker_main, NULL))
		return -1;
	started = 1;
	return 0;
}

int worker_idle(void)
{
	return state == IDLE;
}

int worker_busy(void)
{
	return state == QUEUED || state == RUNNING;
}

int worker_submit(const render_job *j)
{
	if (state != IDLE)
		return -1;
	if (!started) {
		/* No thread: draw it now, in the poll loop. */
		job = *j;
		job.ok = job.aborted = 0;
		job.err[0] = 0;
		state = RUNNING;
		run_job();
		state = DONE;
		return 0;
	}
	pthread_mutex_lock(&job_mutex);
	job = *j;
	job.ok = 0;
	job.aborted = 0;
	job.err[0] = 0;
	state = QUEUED;
	pthread_cond_signal(&job_cond);
	pthread_mutex_unlock(&job_mutex);
	return 0;
}

const render_job *worker_current(void)
{
	return state == IDLE ? NULL : &job;
}

int worker_slice(int cs)
{
	unsigned t0 = now_cs();
	while (worker_busy() && (int)(now_cs() - t0) < cs)
		yield();
	return state == DONE;
}

int worker_take(render_job *out)
{
	if (state != DONE)
		return 0;
	*out = job;
	state = IDLE;
	return 1;
}

void worker_stop_for(void *owner)
{
	render_job r;
	if (state == IDLE)
		return;
	if (owner && job.owner != owner)
		return;
	/* Not started yet: take it back. Running: stop it. */
	pthread_mutex_lock(&job_mutex);
	if (state == QUEUED) {
		state = IDLE;
		pthread_mutex_unlock(&job_mutex);
		free(job.area);
		return;
	}
	pthread_mutex_unlock(&job_mutex);
	if (state == RUNNING)
		folio_abort(job.doc);
	while (worker_busy())
		yield();
	if (worker_take(&r))
		free(r.area);
}
