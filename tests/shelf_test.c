/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Bookshelf data tests (wimp/shelfdata.c): adding, finding, removing,
 * sorting, folders, document names, and the file round trip.
 *   shelf_test   (writes tests/out/shelf) */
#include "shelfdata.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	shelf s, t;
	char title[64];
	int i, a, b, c;

	shelf_init(&s);
	a = shelf_add(&s, "ADFS::HD4.$.Books.Dune/epub", NULL, 100);
	b = shelf_add(&s, "ADFS::HD4.$.Books.Alpha\xa0Guide/pdf", "Alpha Guide", 200);
	c = shelf_add(&s, "ADFS::HD4.$.Papers.Zebra", "Zebra\tStripes\nTwo", 300);
	CHECK(a == 0 && b == 1 && c == 2 && s.n == 3, "add: %d %d %d n %d", a, b, c, s.n);
	CHECK(!strcmp(s.books[0].title, "Dune"), "leaf title '%s'", s.books[0].title);
	CHECK(!strcmp(s.books[2].title, "Zebra Stripes Two"), "tabs in title '%s'", s.books[2].title);
	CHECK(shelf_add(&s, "adfs::hd4.$.books.dune/EPUB", NULL, 999) == 0 && s.n == 3, "added twice (case)");
	CHECK(shelf_find(&s, "ADFS::HD4.$.Papers.Zebra") == 2, "find");
	CHECK(shelf_find(&s, "ADFS::HD4.$.Papers.Zebr") == -1, "find prefix");

	shelf_leaf_title("SDFS::RISCOSpi.$.My\xa0" "Book/pdf", title, sizeof title);
	CHECK(!strcmp(title, "My Book"), "hard space and extension: '%s'", title);
	shelf_leaf_title("Notes", title, sizeof title);
	CHECK(!strcmp(title, "Notes"), "plain leaf: '%s'", title);

	/* Sorting. */
	strcpy(s.books[0].author, "Herbert");
	strcpy(s.books[2].author, "Anon");
	s.books[1].read = 500;         /* Alpha Guide read most recently */
	shelf_sort(&s, SHELF_SORT_RECENT);
	CHECK(!strcmp(s.books[0].title, "Alpha Guide") && !strcmp(s.books[1].title, "Zebra Stripes Two") &&
		!strcmp(s.books[2].title, "Dune"), "recent: %s, %s, %s", s.books[0].title, s.books[1].title,
		s.books[2].title);
	shelf_sort(&s, SHELF_SORT_TITLE);
	CHECK(!strcmp(s.books[0].title, "Alpha Guide") && !strcmp(s.books[1].title, "Dune"), "title order");
	shelf_sort(&s, SHELF_SORT_AUTHOR);
	CHECK(!strcmp(s.books[0].author, "Anon") && !strcmp(s.books[1].author, "Herbert") &&
		!s.books[2].author[0], "author order (none last): %s, %s, '%s'", s.books[0].author,
		s.books[1].author, s.books[2].author);

	/* Folders. */
	CHECK(shelf_add_folder(&s, "ADFS::HD4.$.Books") == 0, "folder");
	CHECK(shelf_add_folder(&s, "adfs::hd4.$.books") == 0 && s.nfolders == 1, "folder twice");
	CHECK(shelf_add_folder(&s, "ADFS::HD4.$.Papers") == 1, "second folder");
	CHECK(shelf_in_folder("ADFS::HD4.$.Books.Dune/epub", "ADFS::HD4.$.Books"), "in folder");
	CHECK(shelf_in_folder("ADFS::HD4.$.Books.SF.Dune/epub", "ADFS::HD4.$.Books"), "in subfolder");
	CHECK(!shelf_in_folder("ADFS::HD4.$.Bookshelf.X", "ADFS::HD4.$.Books"), "prefix isn't inside");

	/* Documents. */
	CHECK(shelf_is_document("Report", 0xadf, -1), "PDF type");
	CHECK(shelf_is_document("Book", 0x18f, 0x18f), "EPUB type");
	CHECK(shelf_is_document("Book/EPUB", 0xfff, -1), "EPUB extension");
	CHECK(shelf_is_document("comic/cbz", 0xa91, -1), "CBZ extension");
	CHECK(!shelf_is_document("Picture/png", 0xb60, -1), "PNG isn't a document");
	CHECK(!shelf_is_document("README", 0xfff, -1), "text isn't a document");

	/* File round trip. */
	mkdir("tests/out", 0777);
	s.books[0].page = 41;
	s.books[0].pages = 120;
	s.sort = SHELF_SORT_TITLE;
	CHECK(shelf_save(&s, "tests/out/shelf") == 0, "save");
	shelf_init(&t);
	CHECK(shelf_load(&t, "tests/out/shelf") == 0, "load");
	CHECK(t.n == 3 && t.nfolders == 2 && t.sort == SHELF_SORT_TITLE, "loaded %d books %d folders sort %d",
		t.n, t.nfolders, t.sort);
	for (i = 0; i < 3 && i < t.n; i++) {
		CHECK(!strcmp(t.books[i].path, s.books[i].path) && !strcmp(t.books[i].title, s.books[i].title) &&
			!strcmp(t.books[i].author, s.books[i].author) && t.books[i].added == s.books[i].added &&
			t.books[i].read == s.books[i].read && t.books[i].page == s.books[i].page &&
			t.books[i].pages == s.books[i].pages, "book %d differs", i);
	}
	CHECK(t.nfolders == 2 && !strcmp(t.folders[1], "ADFS::HD4.$.Papers"), "folders differ");
	shelf_free(&t);
	shelf_init(&t);
	CHECK(shelf_load(&t, "tests/out/no-such-shelf") == -1 && t.n == 0, "missing file");

	/* Removing (sorted by author: Zebra (Anon), Dune (Herbert), Alpha Guide). */
	shelf_remove(&s, 0);
	CHECK(s.n == 2 && shelf_find(&s, "ADFS::HD4.$.Papers.Zebra") == -1 &&
		shelf_find(&s, "ADFS::HD4.$.Books.Dune/epub") == 0, "remove");
	shelf_remove_folder(&s, 0);
	CHECK(s.nfolders == 1 && !strcmp(s.folders[0], "ADFS::HD4.$.Papers"), "remove folder");
	shelf_free(&s);
	shelf_free(&t);

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
