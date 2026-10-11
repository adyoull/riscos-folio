/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The Bookshelf's data: the books on it and the folders it watches, kept in
 * a small text file (<Choices$Write>.Folio.Shelf). No RISC OS calls here, so
 * it's tested on Linux (tests/shelf_test.c). */
#ifndef SHELFDATA_H
#define SHELFDATA_H

#include <stddef.h>

typedef struct {
	char path[256];
	char title[128];          /* UTF-8; the leaf name until the book is read */
	char author[96];
	long added, read;         /* seconds since 1970; read 0 = never opened */
	int page, pages;          /* where the reader got to (pages 0 = unknown) */
	/* Not saved. */
	int missing;
	int cover;                /* front end's cover state */
	int *cover_area;
} shelf_book;

typedef struct {
	shelf_book *books;
	int n, cap;
	char (*folders)[256];
	int nfolders, folder_cap;
	int sort;                 /* SHELF_SORT_* */
} shelf;

enum { SHELF_SORT_RECENT, SHELF_SORT_TITLE, SHELF_SORT_AUTHOR };

void shelf_init(shelf *s);
void shelf_free(shelf *s);
/* 0, or -1 if the file couldn't be read (the shelf is left empty). */
int shelf_load(shelf *s, const char *file);
int shelf_save(const shelf *s, const char *file);

/* The book's index, or -1. Paths compare without regard to case (FileCore). */
int shelf_find(const shelf *s, const char *path);
/* Adds a book (or returns the one already there); -1 if out of memory. */
int shelf_add(shelf *s, const char *path, const char *title, long now);
void shelf_remove(shelf *s, int i);
void shelf_sort(shelf *s, int sort);

int shelf_find_folder(const shelf *s, const char *path);
int shelf_add_folder(shelf *s, const char *path);
void shelf_remove_folder(shelf *s, int i);
/* Is path inside a watched folder (or the folder itself)? */
int shelf_in_folder(const char *path, const char *folder);

/* Does a file of this name and RISC OS filetype look like something Folio
 * reads? (PDF &ADF, the EPUB type if known (-1 if not), or a /pdf, /epub,
 * /fb2, /mobi, /cbz, /xps, /oxps extension.) */
int shelf_is_document(const char *leaf, int filetype, int epub_type);

/* The leaf name of a RISC OS path, without a /ext. */
void shelf_leaf_title(const char *path, char *out, size_t size);

#endif
