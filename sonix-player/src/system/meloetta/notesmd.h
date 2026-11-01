#ifndef NOTESMD_H
#define NOTESMD_H

#include <stdbool.h>
#include <stddef.h>

// The small piece of Markdown the Notes page draws (Meloetta fork). No LVGL in
// here, so the desktop test can run it on its own.
//
// A note is read a line at a time. Every line is one block, except a fenced
// code block, which runs to its closing fence. Lines are not joined into
// paragraphs the way CommonMark joins them: a note typed on the player has a
// line break where Enter was pressed, and that is where it should stay.

typedef enum {
	MD_BLANK = 0,
	MD_PARA,
	MD_H1,
	MD_H2,
	MD_H3,	   // ### and deeper
	MD_BULLET, // - * +
	MD_NUMBER, // 1. 2)
	MD_TASK,   // - [ ] / - [x]
	MD_QUOTE,  // >
	MD_CODE,   // ``` ... ```, text is the inside, fences excluded
	MD_RULE,   // --- *** ___
} md_kind_t;

typedef struct {
	md_kind_t kind;
	int indent;		 // list nesting: leading spaces / 2, capped at 4
	int number;		 // MD_NUMBER: the number written
	bool checked;	 // MD_TASK
	size_t check_at; // MD_TASK: offset in the note of the ' ' or 'x' inside [ ]
	size_t text;	 // offset of the block's text (markers stripped)
	size_t len;		 // its length in bytes
	size_t next;	 // where the following block starts
} md_block_t;

// Reads the block that starts at `at` in `s` (length `n`). False at the end.
bool md_next_block(const char *s, size_t n, size_t at, md_block_t *out);

// Inline styles a run of text can carry.
#define MD_BOLD 1
#define MD_ITALIC 2
#define MD_CODE_SPAN 4
#define MD_STRIKE 8
#define MD_LINK 16

// Walks `s`/`n` (one block's text) and calls `emit` for each run of text with
// its styles: **bold**, __bold__, *italic*, _italic_ (only at word edges, so
// snake_case stays as typed), `code`, ~~strike~~, [text](url) (the text only).
// A backslash keeps the next character as it is. A marker with no partner is
// shown as typed.
typedef void (*md_emit_t)(const char *text, size_t len, int styles, void *user);
void md_inline(const char *s, size_t n, md_emit_t emit, void *user);

// The heading a note's first line gives it ("# Shopping" -> "Shopping"), or the
// first non-blank line's text. Copied into `out`; false when the note is empty.
bool md_title(const char *s, size_t n, char *out, size_t size);

#endif /* NOTESMD_H */
