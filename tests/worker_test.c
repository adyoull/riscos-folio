/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* The page worker (wimp/worker.c) on ARM Linux: submit, slices, take, stop
 * a running job, take back a queued one, and the core lock. */
#include "worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static render_job make(folio_ctx *fc, folio_doc *d, float ppp, int w, int h, void *owner)
{
	render_job j;
	memset(&j, 0, sizeof j);
	j.owner = owner; j.fc = fc; j.doc = d; j.ppp = ppp; j.px_w = w; j.px_h = h;
	j.loc.chapter = 0; j.loc.page = 0;
	j.bytes = 16 + 44 + (size_t)w * h * 4;
	j.area = malloc(j.bytes);
	return j;
}

int main(int argc, char **argv)
{
	folio_ctx *fc = folio_new(64 << 20);
	folio_doc *d = folio_open(fc, argv[1], NULL);
	render_job j, r;
	int slices, owner_a, owner_b;
	folio_count_step(d, 1);
	CHECK(worker_init() == 0, "thread starts");

	/* A normal page: slices until done, then take. */
	j = make(fc, d, 1.0f, 595, 842, &owner_a);
	CHECK(worker_submit(&j) == 0 && !worker_idle(), "submitted");
	CHECK(worker_submit(&j) != 0, "a second job is refused while busy");
	for (slices = 0; !worker_slice(5) && slices < 2000; slices++)
		;
	CHECK(worker_take(&r) && r.ok && worker_idle(), "page done in %d slices (%s)", slices, r.err);
	{
		unsigned char *px = (unsigned char *)(r.area + 15);
		CHECK(px[0] == 255 && px[3] == 0, "white paper, top byte 0");
	}
	free(r.area);

	/* A slow page, stopped while running: nothing to take, idle again. */
	j = make(fc, d, 8.0f, 2400, 2400, &owner_b);
	worker_submit(&j);
	worker_slice(2);
	worker_stop_for(&owner_b);
	CHECK(worker_idle(), "stopped and idle");

	/* stop_for another owner leaves the job alone. */
	j = make(fc, d, 1.0f, 595, 842, &owner_a);
	worker_submit(&j);
	worker_stop_for(&owner_b);
	CHECK(!worker_idle(), "other owner's job untouched");
	worker_stop_for(NULL);
	CHECK(worker_idle(), "stop_for(NULL) stops any job");

	/* The core lock: held by the main thread, the worker waits for it. */
	j = make(fc, d, 1.0f, 595, 842, &owner_a);
	core_lock();
	worker_submit(&j);
	worker_slice(20);
	CHECK(worker_busy(), "worker waits while the main thread holds the core");
	core_unlock();
	for (slices = 0; !worker_slice(5) && slices < 2000; slices++)
		;
	CHECK(worker_take(&r) && r.ok, "then finishes");
	free(r.area);
	CHECK(core_trylock(), "trylock when free");
	core_unlock();

	/* A cover: the worker opens the file itself, draws page 1 80 px wide
	 * (A4: 113 high, within 200), and reads the title and author. */
	j = make(fc, NULL, 0, 80, 200, &owner_a);
	j.kind = JOB_COVER;
	snprintf(j.path, sizeof j.path, "%s", argv[1]);
	worker_submit(&j);
	for (slices = 0; !worker_slice(5) && slices < 2000; slices++)
		;
	CHECK(worker_take(&r) && r.ok, "cover: %s", r.err);
	CHECK(r.px_h == 113 && r.area[4 + 5] == 112, "cover height %d (sprite %d)", r.px_h, r.area[4 + 5]);
	CHECK(!strcmp(r.title, "Folio Test PDF") && !strcmp(r.author, "A. Tester"), "cover title '%s' author '%s'",
		r.title, r.author);
	CHECK(r.doc == NULL, "the cover's document wasn't let go");
	free(r.area);

	/* A cover that can't be made: a failed result, with a reason. */
	j = make(fc, NULL, 0, 80, 200, &owner_a);
	j.kind = JOB_COVER;
	snprintf(j.path, sizeof j.path, "%s.missing", argv[1]);
	worker_submit(&j);
	for (slices = 0; !worker_slice(5) && slices < 2000; slices++)
		;
	CHECK(worker_take(&r) && !r.ok && r.err[0], "missing cover: ok %d err '%s'", r.ok, r.err);
	free(r.area);

	/* Stopping covers over and over while they run must be safe. */
	for (slices = 0; slices < 20; slices++) {
		j = make(fc, NULL, 0, 80, 200, &owner_a);
		j.kind = JOB_COVER;
		snprintf(j.path, sizeof j.path, "%s", argv[1]);
		worker_submit(&j);
		worker_slice(slices % 3);
		worker_stop_for(NULL);
	}
	CHECK(worker_idle(), "idle after stopping covers");

	folio_close(d);
	folio_drop(fc);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
