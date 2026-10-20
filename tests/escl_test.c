/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* eSCL scanning and mDNS discovery (core/escl.c), against the pretend
 * scanner tests/fake-escl.py, under qemu (tests/escl-test.sh runs both):
 *   escl_test OUTDIR       (OUTDIR/ports: "HTTP_PORT MDNS_PORT")
 * Capabilities, settings, a scan from the glass and from the feeder (with
 * the scanner warming up), an empty feeder, no scanner there, cancelling,
 * and finding the scanner by mDNS. */
#include "escl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char caps_xml[] =
	"<?xml version='1.0'?><scan:ScannerCapabilities xmlns:scan='s' xmlns:pwg='p'>"
	"<pwg:Version>2.5</pwg:Version><pwg:MakeAndModel>Brand X 100</pwg:MakeAndModel>"
	"<scan:Platen><scan:PlatenInputCaps><scan:MaxWidth>2550</scan:MaxWidth><scan:MaxHeight>3508</scan:MaxHeight>"
	"<scan:SettingProfiles><scan:SettingProfile><scan:ColorModes><scan:ColorMode>RGB24</scan:ColorMode>"
	"<scan:ColorMode>Grayscale8</scan:ColorMode></scan:ColorModes>"
	"<scan:DocumentFormats><pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat></scan:DocumentFormats>"
	"<scan:SupportedResolutions><scan:DiscreteResolutions>"
	"<scan:DiscreteResolution><scan:XResolution>300</scan:XResolution></scan:DiscreteResolution>"
	"<scan:DiscreteResolution><scan:XResolution>100</scan:XResolution></scan:DiscreteResolution>"
	"<scan:DiscreteResolution><scan:XResolution>600</scan:XResolution></scan:DiscreteResolution>"
	"</scan:DiscreteResolutions></scan:SupportedResolutions></scan:SettingProfile></scan:SettingProfiles>"
	"</scan:PlatenInputCaps></scan:Platen></scan:ScannerCapabilities>";

static long longest_step;   /* microseconds */

static long usec(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000000L + tv.tv_usec;
}

/* Runs a job to its end (or 30 s); counts pages and keeps the first. */
static int run(escl_job *j, int *pages, unsigned char **first, size_t *first_len, int cancel_after)
{
	int r, i;
	*pages = 0;
	if (first)
		*first = NULL;
	for (i = 0; i < 30000; i++) {
		long t0 = usec(), t;
		r = escl_step(j);
		t = usec() - t0;
		if (t > longest_step)
			longest_step = t;
		if (r == ESCL_PAGE) {
			size_t len;
			const char *mime;
			unsigned char *p = escl_take_page(j, &len, &mime);
			(*pages)++;
			if (first && !*first) {
				*first = p;
				*first_len = len;
			} else {
				free(p);
			}
			CHECK(!strcmp(mime, "image/jpeg"), "page type %s", mime);
			if (cancel_after && *pages == cancel_after)
				escl_cancel(j);
			continue;
		}
		if (r != ESCL_BUSY)
			return r;
		usleep(1000);
	}
	return -99;
}

static int file_has(const char *path, const char *text)
{
	FILE *f = fopen(path, "rb");
	char buf[8192];
	size_t n;
	if (!f)
		return 0;
	n = fread(buf, 1, sizeof buf - 1, f);
	buf[n] = 0;
	fclose(f);
	return strstr(buf, text) != NULL;
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "tests/out/escl";
	char path[300], settings[300], deleted[300];
	int http_port = 0, mdns_port = 0, pages, r;
	unsigned char *first;
	size_t first_len;
	escl_caps c;
	escl_settings s;
	escl_job *j;
	char xml[2048];
	FILE *f;

	snprintf(path, sizeof path, "%s/ports", out);
	f = fopen(path, "r");
	if (!f || fscanf(f, "%d %d", &http_port, &mdns_port) != 2) {
		printf("no pretend scanner (%s)\n", path);
		return 1;
	}
	fclose(f);
	snprintf(settings, sizeof settings, "%s/settings.xml", out);
	snprintf(deleted, sizeof deleted, "%s/deleted", out);

	printf("Capabilities\n");
	CHECK(escl_parse_caps(caps_xml, strlen(caps_xml), &c) == 0, "parsed");
	CHECK(!strcmp(c.model, "Brand X 100"), "model '%s'", c.model);
	CHECK(!strcmp(c.version, "2.5"), "version '%s'", c.version);
	CHECK(c.has[ESCL_PLATEN] && !c.has[ESCL_FEEDER] && !c.duplex, "glass only");
	CHECK(c.max_w[ESCL_PLATEN] == 2550 && c.max_h[ESCL_PLATEN] == 3508, "size %d x %d",
		c.max_w[0], c.max_h[0]);
	CHECK(c.nres == 3 && c.res[0] == 100 && c.res[1] == 300 && c.res[2] == 600, "resolutions in order");
	CHECK(c.rgb && c.gray && !c.bw && c.jpeg && !c.pdf, "modes and formats");
	CHECK(escl_parse_caps("<html>no</html>", 15, &c) == -1, "not a scanner");

	printf("Settings\n");
	escl_parse_caps(caps_xml, strlen(caps_xml), &c);
	memset(&s, 0, sizeof s);
	s.source = ESCL_PLATEN;
	s.color = ESCL_GRAY;
	s.dpi = 250;
	s.width = 2480;
	s.height = 9999;
	CHECK(escl_settings_xml(&s, &c, xml, sizeof xml) > 0, "made");
	CHECK(strstr(xml, "<scan:XResolution>300<") != NULL, "250 dpi becomes the nearest, 300");
	CHECK(strstr(xml, "<pwg:Width>2480<") && strstr(xml, "<pwg:Height>3508<"), "height kept to the glass");
	CHECK(strstr(xml, "Grayscale8") && strstr(xml, "<pwg:InputSource>Platen<"), "grey, glass");
	CHECK(strstr(xml, "<pwg:Version>2.5<") && strstr(xml, "DocumentFormatExt>image/jpeg"), "version, format");
	CHECK(!strstr(xml, "Duplex"), "no duplex on the glass");
	CHECK(!c.bright && !c.contrast, "no brightness or contrast offered");
	s.brightness = 50;
	escl_settings_xml(&s, &c, xml, sizeof xml);
	CHECK(!strstr(xml, "Brightness"), "so none is sent");
	s.brightness = 0;

	printf("Scanning from the glass\n");
	memset(&s, 0, sizeof s);
	s.source = ESCL_PLATEN;
	s.dpi = 300;
	j = escl_job_new("127.0.0.1", http_port, "eSCL", &s);
	CHECK(j != NULL, "job: %s", escl_last_error());
	if (j) {
		r = run(j, &pages, &first, &first_len, 0);
		CHECK(r == ESCL_DONE, "done (%d: %s)", r, escl_error(j));
		CHECK(pages == 1, "one page (%d)", pages);
		CHECK(first && first_len > 10000 && !memcmp(first, "\xff\xd8\xff\xe0" "FAKE JPEG PAGE 1 ", 21),
			"the page's bytes (%lu)", (unsigned long)first_len);
		CHECK(!strcmp(escl_job_caps(j)->model, "Folio Test Scanner 9000"), "caps read (chunked): '%s'",
			escl_job_caps(j)->model);
		CHECK(file_has(settings, "<pwg:InputSource>Platen<") && file_has(settings, "XResolution>300<") &&
			file_has(settings, "<pwg:Width>2550<"), "settings sent: glass, 300 dpi, whole width");
		CHECK(escl_job_caps(j)->bright && escl_job_caps(j)->bright_max == 2000 &&
			escl_job_caps(j)->bright_normal == 1000 && !escl_job_caps(j)->contrast, "brightness 0-2000, no contrast");
		CHECK(!file_has(settings, "Brightness"), "brightness 0: the scanner's own, not sent");
		free(first);
		escl_job_free(j);
	}

	printf("Brightness and contrast\n");
	s.brightness = 50;
	s.contrast = -30;
	j = escl_job_new("127.0.0.1", http_port, "eSCL", &s);
	if (j) {
		r = run(j, &pages, NULL, NULL, 0);
		CHECK(r == ESCL_DONE, "done (%s)", escl_error(j));
		CHECK(file_has(settings, "<scan:Brightness>1500<"), "half way to the brightest: 1500");
		CHECK(!file_has(settings, "Contrast>"), "contrast left out: the scanner can't");
		escl_job_free(j);
	}
	s.brightness = -100;
	j = escl_job_new("127.0.0.1", http_port, "eSCL", &s);
	if (j) {
		run(j, &pages, NULL, NULL, 0);
		CHECK(file_has(settings, "<scan:Brightness>0<"), "the darkest: 0");
		escl_job_free(j);
	}
	s.brightness = s.contrast = 0;

	printf("Scanning from the feeder\n");
	s.source = ESCL_FEEDER;
	s.duplex = 1;
	s.dpi = 600;
	j = escl_job_new("127.0.0.1", http_port, "/eSCL/", &s);
	if (j) {
		r = run(j, &pages, &first, &first_len, 0);
		CHECK(r == ESCL_DONE, "done (%d: %s)", r, escl_error(j));
		CHECK(pages == 2, "two pages (%d)", pages);
		CHECK(!strcmp(escl_status(j), "Scanned 2 pages"), "status '%s'", escl_status(j));
		CHECK(file_has(settings, "<pwg:InputSource>Feeder<") && file_has(settings, "<scan:Duplex>true<") &&
			file_has(settings, "<pwg:Height>4200<"), "settings sent: feeder, both sides, its length");
		free(first);
		escl_job_free(j);
	}

	printf("An empty feeder\n");
	s.duplex = 0;
	j = escl_job_new("127.0.0.1", http_port, "empty", &s);
	if (j) {
		r = run(j, &pages, NULL, NULL, 0);
		CHECK(r == ESCL_ERROR && strstr(escl_error(j), "no paper"), "says so (%d: %s)", r, escl_error(j));
		escl_job_free(j);
	}

	printf("No scanner there\n");
	j = escl_job_new("127.0.0.1", 1, "eSCL", &s);
	if (j) {
		r = run(j, &pages, NULL, NULL, 0);
		CHECK(r == ESCL_ERROR && strstr(escl_error(j), "connect"), "can't connect (%s)", escl_error(j));
		escl_job_free(j);
	}
	CHECK(escl_job_new("no-such-host.invalid", 80, "eSCL", &s) == NULL, "an unknown name: %s", escl_last_error());

	printf("Cancelling\n");
	j = escl_job_new("127.0.0.1", http_port, "eSCL", &s);
	if (j) {
		r = run(j, &pages, NULL, NULL, 1);
		CHECK(r == ESCL_DONE && pages == 1, "stopped after one page (%d, %d)", r, pages);
		CHECK(file_has(deleted, "/eSCL/ScanJobs/job"), "the job was deleted");
		escl_job_free(j);
	}

	printf("Never waiting, even on a blocking socket\n");
	{
		char slow[300];
		FILE *sf;
		snprintf(slow, sizeof slow, "%s/slow", out);
		sf = fopen(slow, "w");
		if (sf)
			fclose(sf);
		escl_test_blocking(1);
		s.source = ESCL_PLATEN;
		longest_step = 0;
		j = escl_job_new("127.0.0.1", http_port, "eSCL", &s);
		if (j) {
			r = run(j, &pages, NULL, NULL, 0);
			CHECK(r == ESCL_DONE && pages == 1, "scanned (%d, %d: %s)", r, pages, escl_error(j));
			/* The page took 1.5 s to come; no step may have waited for it. */
			CHECK(longest_step < 400000, "the longest step took %ld ms", longest_step / 1000);
			escl_job_free(j);
		}
		escl_test_blocking(0);
		remove(slow);
	}

	printf("Finding scanners\n");
	{
		unsigned addrs[2] = { 0x7f000001u, 0x7f000001u };
		escl_find *fd = escl_find_new(addrs, 2, mdns_port, 50);
		const escl_found *found;
		int n, i;
		CHECK(fd != NULL, "started");
		for (i = 0; fd && i < 5000 && escl_find_step(fd); i++)
			usleep(1000);
		n = fd ? escl_find_results(fd, &found) : 0;
		CHECK(n == 1, "one scanner, asked twice (%d)", n);
		if (n >= 1) {
			CHECK(!strcmp(found[0].name, "Folio Test Scanner"), "name '%s'", found[0].name);
			CHECK(!strcmp(found[0].host, "127.0.0.1"), "address '%s'", found[0].host);
			CHECK(found[0].port == http_port, "port %d", found[0].port);
			CHECK(!strcmp(found[0].root, "eSCL"), "root '%s'", found[0].root);
		}
		escl_find_free(fd);
	}
	CHECK(escl_local_address(0x7f000001u) == 0x7f000001u, "local address towards 127.0.0.1");

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
