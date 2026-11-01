#include "notesmd.h"

#include <ctype.h>
#include <string.h>

static size_t line_end(const char *s, size_t n, size_t at) {
	const char *nl = memchr(s + at, '\n', n - at);
	return nl ? (size_t)(nl - s) : n;
}

// The end of a line with a Windows line ending trimmed off.
static size_t trim_cr(const char *s, size_t from, size_t e) {
	while (e > from && (s[e - 1] == '\r' || s[e - 1] == ' ' || s[e - 1] == '\t')) {
		e--;
	}
	return e;
}

// Leading blanks, counted the way an indent is meant: a tab as two levels.
static size_t skip_indent(const char *s, size_t at, size_t e, int *spaces) {
	int sp = 0;
	while (at < e && (s[at] == ' ' || s[at] == '\t')) {
		sp += s[at] == '\t' ? 4 : 1;
		at++;
	}
	*spaces = sp;
	return at;
}

static bool is_fence(const char *s, size_t p, size_t e, char *which) {
	if (e - p >= 3 && (s[p] == '`' || s[p] == '~') && s[p + 1] == s[p] && s[p + 2] == s[p]) {
		*which = s[p];
		return true;
	}
	return false;
}

static bool is_rule(const char *s, size_t p, size_t e) {
	char c = s[p];
	if (c != '-' && c != '*' && c != '_') {
		return false;
	}
	int count = 0;
	for (size_t i = p; i < e; i++) {
		if (s[i] == c) {
			count++;
		} else if (s[i] != ' ') {
			return false;
		}
	}
	return count >= 3;
}

bool md_next_block(const char *s, size_t n, size_t at, md_block_t *out) {
	if (!s || at >= n) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	size_t raw_end = line_end(s, n, at);
	size_t e = trim_cr(s, at, raw_end);
	out->next = raw_end < n ? raw_end + 1 : n;

	int spaces = 0;
	size_t p = skip_indent(s, at, e, &spaces);
	out->indent = spaces / 2 > 4 ? 4 : spaces / 2;

	if (p >= e) {
		out->kind = MD_BLANK;
		out->text = p;
		return true;
	}

	char fence;
	if (is_fence(s, p, e, &fence)) {
		// Everything up to the closing fence, as typed. A fence never closed runs
		// to the end of the note, which is what every renderer does.
		size_t body = out->next;
		size_t q = body;
		while (q < n) {
			size_t qe = line_end(s, n, q);
			int sp;
			size_t qp = skip_indent(s, q, qe, &sp);
			char f2;
			if (is_fence(s, qp, qe, &f2) && f2 == fence) {
				out->kind = MD_CODE;
				out->text = body;
				out->len = q > body ? q - 1 - body : 0;
				out->next = qe < n ? qe + 1 : n;
				return true;
			}
			q = qe < n ? qe + 1 : n;
		}
		out->kind = MD_CODE;
		out->text = body;
		out->len = n > body ? n - body : 0;
		out->next = n;
		return true;
	}

	// A picture on a line of its own.
	if (s[p] == '!' && p + 1 < e && s[p + 1] == '[' && s[e - 1] == ')') {
		const char *mid = NULL;
		for (size_t i = p + 2; i + 1 < e; i++) {
			if (s[i] == ']' && s[i + 1] == '(') {
				mid = s + i;
				break;
			}
		}
		if (mid) {
			size_t close = (size_t)(mid - s);
			out->kind = MD_IMAGE;
			out->text = p + 2;
			out->len = close - (p + 2);
			out->src = close + 2;
			out->src_len = (e - 1) - out->src;
			// "file.png "title"" -- the title is dropped.
			const char *sp = memchr(s + out->src, ' ', out->src_len);
			if (sp) {
				out->src_len = (size_t)(sp - (s + out->src));
			}
			return true;
		}
	}

	// A table: this line and every following one that starts with a bar.
	if (s[p] == '|') {
		size_t q = out->next;
		while (q < n) {
			size_t qe = line_end(s, n, q);
			int sp;
			size_t qp = skip_indent(s, q, qe, &sp);
			if (qp >= qe || s[qp] != '|') {
				break;
			}
			q = qe < n ? qe + 1 : n;
		}
		out->kind = MD_TABLE;
		out->text = at;
		out->len = (q > at && s[q - 1] == '\n' ? q - 1 : q) - at;
		out->next = q;
		return true;
	}

	if (s[p] == '#') {
		size_t h = p;
		while (h < e && s[h] == '#') {
			h++;
		}
		size_t level = h - p;
		if (level <= 6 && (h == e || s[h] == ' ')) {
			out->kind = level == 1 ? MD_H1 : level == 2 ? MD_H2 : MD_H3;
			while (h < e && s[h] == ' ') {
				h++;
			}
			// "## Title ##" closes with hashes some people type.
			size_t te = e;
			while (te > h && s[te - 1] == '#') {
				te--;
			}
			te = trim_cr(s, h, te);
			out->text = h;
			out->len = te - h;
			return true;
		}
	}

	if (is_rule(s, p, e)) {
		out->kind = MD_RULE;
		out->text = p;
		return true;
	}

	if (s[p] == '>') {
		size_t q = p + 1;
		if (q < e && s[q] == ' ') {
			q++;
		}
		out->kind = MD_QUOTE;
		out->text = q;
		out->len = e - q;
		return true;
	}

	if ((s[p] == '-' || s[p] == '*' || s[p] == '+') && p + 1 < e && s[p + 1] == ' ') {
		size_t q = p + 2;
		while (q < e && s[q] == ' ') {
			q++;
		}
		if (q + 2 < e && s[q] == '[' && s[q + 2] == ']' &&
			(s[q + 1] == ' ' || s[q + 1] == 'x' || s[q + 1] == 'X') && (q + 3 >= e || s[q + 3] == ' ')) {
			out->kind = MD_TASK;
			out->checked = s[q + 1] != ' ';
			out->check_at = q + 1;
			q += 3;
			while (q < e && s[q] == ' ') {
				q++;
			}
		} else {
			out->kind = MD_BULLET;
		}
		out->text = q;
		out->len = e - q;
		return true;
	}

	if (isdigit((unsigned char)s[p])) {
		size_t q = p;
		int num = 0;
		while (q < e && q - p < 9 && isdigit((unsigned char)s[q])) {
			num = num * 10 + (s[q] - '0');
			q++;
		}
		if (q + 1 < e && (s[q] == '.' || s[q] == ')') && s[q + 1] == ' ') {
			q += 2;
			while (q < e && s[q] == ' ') {
				q++;
			}
			out->kind = MD_NUMBER;
			out->number = num;
			out->text = q;
			out->len = e - q;
			return true;
		}
	}

	out->kind = MD_PARA;
	out->text = p;
	out->len = e - p;
	return true;
}

// ---------------------------------------------------------------------------
// inline
// ---------------------------------------------------------------------------

static bool word_char(char c) { return isalnum((unsigned char)c) || (unsigned char)c >= 0x80; }

// The next `mark` (one or two characters) after `from` that can close a run:
// not right after a blank.
static size_t find_closer(const char *s, size_t n, size_t from, const char *mark, size_t mlen) {
	for (size_t j = from; j + mlen <= n; j++) {
		if (s[j] == '\\') {
			j++;
			continue;
		}
		if (memcmp(s + j, mark, mlen) == 0 && j > from && s[j - 1] != ' ') {
			// A single '*' that is half of a "**" is not the closer of an italic.
			if (mlen == 1 && ((j + 1 < n && s[j + 1] == mark[0]) || s[j - 1] == mark[0])) {
				continue;
			}
			if (mark[0] == '_' && j + mlen < n && word_char(s[j + mlen])) {
				continue;
			}
			return j;
		}
	}
	return (size_t)-1;
}

void md_inline(const char *s, size_t n, md_emit_t emit, void *user) {
	int styles = 0;
	size_t run = 0;
	size_t i = 0;

#define FLUSH(upto)                                                                                                    \
	do {                                                                                                               \
		if ((upto) > run) {                                                                                            \
			emit(s + run, (upto) - run, styles, user);                                                                 \
		}                                                                                                              \
	} while (0)

	while (i < n) {
		char c = s[i];

		if (c == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
			FLUSH(i);
			run = i + 1;
			i += 2;
			continue;
		}

		if (c == '`') {
			const char *close = memchr(s + i + 1, '`', n - i - 1);
			if (close) {
				FLUSH(i);
				size_t start = i + 1, len = (size_t)(close - s) - start;
				if (len) {
					emit(s + start, len, styles | MD_CODE_SPAN, user);
				}
				i = (size_t)(close - s) + 1;
				run = i;
				continue;
			}
		}

		if (c == '[') {
			const char *rb = memchr(s + i + 1, ']', n - i - 1);
			if (rb && (size_t)(rb - s) + 1 < n && rb[1] == '(') {
				size_t after = (size_t)(rb - s) + 2;
				const char *rp = memchr(s + after, ')', n - after);
				if (rp) {
					FLUSH(i);
					size_t start = i + 1, len = (size_t)(rb - s) - start;
					if (len) {
						emit(s + start, len, styles | MD_LINK, user);
					}
					i = (size_t)(rp - s) + 1;
					run = i;
					continue;
				}
			}
		}

		// Two-character markers first, so "**" is never read as two italics.
		if ((c == '*' || c == '_' || c == '~') && i + 1 < n && s[i + 1] == c) {
			int flag = c == '~' ? MD_STRIKE : MD_BOLD;
			char mark[3] = {c, c, 0};
			if (styles & flag) {
				FLUSH(i);
				styles &= ~flag;
				i += 2;
				run = i;
				continue;
			}
			bool edge = c != '_' || i == 0 || !word_char(s[i - 1]);
			if (edge && i + 2 < n && s[i + 2] != ' ' && find_closer(s, n, i + 2, mark, 2) != (size_t)-1) {
				FLUSH(i);
				styles |= flag;
				i += 2;
				run = i;
				continue;
			}
			if (c == '~') {
				i += 2;
				continue;
			}
		}

		if (c == '*' || c == '_') {
			char mark[2] = {c, 0};
			if (styles & MD_ITALIC) {
				if (c != '_' || i + 1 >= n || !word_char(s[i + 1])) {
					FLUSH(i);
					styles &= ~MD_ITALIC;
					i++;
					run = i;
					continue;
				}
			} else {
				bool edge = c != '_' || i == 0 || !word_char(s[i - 1]);
				if (edge && i + 1 < n && s[i + 1] != ' ' && find_closer(s, n, i + 1, mark, 1) != (size_t)-1) {
					FLUSH(i);
					styles |= MD_ITALIC;
					i++;
					run = i;
					continue;
				}
			}
		}

		i++;
	}
	FLUSH(n);
#undef FLUSH
}

typedef struct {
	char *out;
	size_t size;
	size_t used;
} plain_t;

static void plain_emit(const char *text, size_t len, int styles, void *user) {
	(void)styles;
	plain_t *p = user;
	if (p->used + 1 >= p->size) {
		return;
	}
	size_t room = p->size - 1 - p->used;
	if (len > room) {
		len = room;
	}
	memcpy(p->out + p->used, text, len);
	p->used += len;
	p->out[p->used] = 0;
}

bool md_title(const char *s, size_t n, char *out, size_t size) {
	if (!size) {
		return false;
	}
	out[0] = 0;
	md_block_t b;
	size_t at = 0;
	while (md_next_block(s, n, at, &b)) {
		at = b.next;
		if (b.kind == MD_BLANK || b.kind == MD_RULE || b.kind == MD_CODE || b.kind == MD_TABLE || b.kind == MD_IMAGE || !b.len) {
			continue;
		}
		plain_t p = {out, size, 0};
		md_inline(s + b.text, b.len, plain_emit, &p);
		return out[0] != 0;
	}
	return false;
}

int md_table_row(const char *s, size_t end, size_t at, size_t *cell, size_t *cell_len, int max, size_t *next) {
	if (at >= end) {
		return -1;
	}
	size_t e = line_end(s, end, at);
	*next = e < end ? e + 1 : end;
	e = trim_cr(s, at, e);
	int sp;
	size_t p = skip_indent(s, at, e, &sp);
	if (p < e && s[p] == '|') {
		p++;
	}
	if (e > p && s[e - 1] == '|' && !(e - p >= 2 && s[e - 2] == '\\')) {
		e--;
	}
	// The header's underline: only bars, dashes, colons and blanks.
	bool rule = true;
	for (size_t i = p; i < e; i++) {
		if (!strchr("|-: ", s[i])) {
			rule = false;
			break;
		}
	}
	if (rule) {
		return 0;
	}
	int count = 0;
	size_t c = p;
	for (size_t i = p; i <= e && count < max; i++) {
		if (i == e || (s[i] == '|' && !(i > p && s[i - 1] == '\\'))) {
			size_t a = c, b = i;
			while (a < b && s[a] == ' ') {
				a++;
			}
			while (b > a && s[b - 1] == ' ') {
				b--;
			}
			cell[count] = a;
			cell_len[count] = b - a;
			count++;
			c = i + 1;
		}
	}
	return count;
}
