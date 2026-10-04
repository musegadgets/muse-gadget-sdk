/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mg_json.h"

#include <errno.h>
#include <string.h>

struct cur {
	char *p;
	char *end;
};

static void skip_ws(struct cur *c)
{
	while (c->p < c->end &&
	       (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
		c->p++;
	}
}

static int hex4(const char *s, unsigned int *out)
{
	unsigned int v = 0;

	for (int i = 0; i < 4; i++) {
		char ch = s[i];

		v <<= 4;
		if (ch >= '0' && ch <= '9') {
			v |= (unsigned int)(ch - '0');
		} else if (ch >= 'a' && ch <= 'f') {
			v |= (unsigned int)(ch - 'a' + 10);
		} else if (ch >= 'A' && ch <= 'F') {
			v |= (unsigned int)(ch - 'A' + 10);
		} else {
			return -EINVAL;
		}
	}
	*out = v;
	return 0;
}

/* At the opening quote. Decodes in place from just after it; *out points at
 * the decoded, NUL-terminated text. */
static int parse_string(struct cur *c, char **out, size_t *out_len)
{
	char *w;

	if (c->p >= c->end || *c->p != '"') {
		return -EINVAL;
	}
	c->p++;
	*out = w = c->p;
	while (c->p < c->end) {
		unsigned char ch = (unsigned char)*c->p;

		if (ch == '"') {
			*w = '\0'; /* w <= c->p: the closing quote's cell at the latest */
			*out_len = (size_t)(w - *out);
			c->p++;
			return 0;
		}
		if (ch < 0x20) {
			return -EINVAL;
		}
		if (ch != '\\') {
			*w++ = (char)ch;
			c->p++;
			continue;
		}
		if (c->end - c->p < 2) {
			return -EINVAL;
		}
		char e = c->p[1];

		c->p += 2;
		switch (e) {
		case '"':
		case '\\':
		case '/':
			*w++ = e;
			break;
		case 'b':
			*w++ = '\b';
			break;
		case 'f':
			*w++ = '\f';
			break;
		case 'n':
			*w++ = '\n';
			break;
		case 'r':
			*w++ = '\r';
			break;
		case 't':
			*w++ = '\t';
			break;
		case 'u': {
			unsigned int u, lo;

			if (c->end - c->p < 4 || hex4(c->p, &u)) {
				return -EINVAL;
			}
			c->p += 4;
			if (u >= 0xDC00 && u <= 0xDFFF) {
				return -EINVAL; /* lone low surrogate */
			}
			if (u >= 0xD800 && u <= 0xDBFF) {
				if (c->end - c->p < 6 || c->p[0] != '\\' || c->p[1] != 'u' ||
				    hex4(&c->p[2], &lo) || lo < 0xDC00 || lo > 0xDFFF) {
					return -EINVAL;
				}
				c->p += 6;
				u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
			}
			if (u == 0) {
				return -EINVAL;
			}
			/* At most 4 bytes out for 6 or 12 read: stays behind c->p. */
			if (u < 0x80) {
				*w++ = (char)u;
			} else if (u < 0x800) {
				*w++ = (char)(0xC0 | (u >> 6));
				*w++ = (char)(0x80 | (u & 0x3F));
			} else if (u < 0x10000) {
				*w++ = (char)(0xE0 | (u >> 12));
				*w++ = (char)(0x80 | ((u >> 6) & 0x3F));
				*w++ = (char)(0x80 | (u & 0x3F));
			} else {
				*w++ = (char)(0xF0 | (u >> 18));
				*w++ = (char)(0x80 | ((u >> 12) & 0x3F));
				*w++ = (char)(0x80 | ((u >> 6) & 0x3F));
				*w++ = (char)(0x80 | (u & 0x3F));
			}
			break;
		}
		default:
			return -EINVAL;
		}
	}
	return -EINVAL;
}

static bool is_digit(char ch)
{
	return ch >= '0' && ch <= '9';
}

static int parse_number(struct cur *c, int64_t *num, bool *is_int)
{
	bool neg = false, frac = false, big = false;
	uint64_t v = 0;

	if (c->p < c->end && *c->p == '-') {
		neg = true;
		c->p++;
	}
	if (c->p >= c->end || !is_digit(*c->p)) {
		return -EINVAL;
	}
	if (*c->p == '0') {
		c->p++;
	} else {
		while (c->p < c->end && is_digit(*c->p)) {
			if (v > (UINT64_C(1) << 62) / 10) {
				big = true;
			} else {
				v = v * 10 + (uint64_t)(*c->p - '0');
			}
			c->p++;
		}
	}
	if (c->p < c->end && *c->p == '.') {
		frac = true;
		c->p++;
		if (c->p >= c->end || !is_digit(*c->p)) {
			return -EINVAL;
		}
		while (c->p < c->end && is_digit(*c->p)) {
			c->p++;
		}
	}
	if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
		frac = true;
		c->p++;
		if (c->p < c->end && (*c->p == '+' || *c->p == '-')) {
			c->p++;
		}
		if (c->p >= c->end || !is_digit(*c->p)) {
			return -EINVAL;
		}
		while (c->p < c->end && is_digit(*c->p)) {
			c->p++;
		}
	}
	*is_int = !frac && !big;
	*num = *is_int ? (neg ? -(int64_t)v : (int64_t)v) : 0;
	return 0;
}

static int literal(struct cur *c, const char *word)
{
	size_t n = strlen(word);

	if ((size_t)(c->end - c->p) < n || memcmp(c->p, word, n) != 0) {
		return -EINVAL;
	}
	c->p += n;
	return 0;
}

static int parse_value(struct cur *c, struct mg_json_field *f, int depth);

/* An object or array below the top level: checked, not kept. */
static int skip_container(struct cur *c, int depth)
{
	struct mg_json_field tmp;
	char close = *c->p == '{' ? '}' : ']';
	bool obj = close == '}';

	if (depth > MG_JSON_MAX_DEPTH) {
		return -E2BIG;
	}
	c->p++;
	skip_ws(c);
	if (c->p < c->end && *c->p == close) {
		c->p++;
		return 0;
	}
	for (;;) {
		int err;

		if (obj) {
			char *k;
			size_t kl;

			skip_ws(c);
			err = parse_string(c, &k, &kl);
			if (err) {
				return err;
			}
			skip_ws(c);
			if (c->p >= c->end || *c->p != ':') {
				return -EINVAL;
			}
			c->p++;
		}
		err = parse_value(c, &tmp, depth);
		if (err) {
			return err;
		}
		skip_ws(c);
		if (c->p < c->end && *c->p == ',') {
			c->p++;
			continue;
		}
		if (c->p < c->end && *c->p == close) {
			c->p++;
			return 0;
		}
		return -EINVAL;
	}
}

static int parse_value(struct cur *c, struct mg_json_field *f, int depth)
{
	char *s;

	skip_ws(c);
	if (c->p >= c->end) {
		return -EINVAL;
	}
	memset(&f->type, 0, sizeof(*f) - offsetof(struct mg_json_field, type));
	switch (*c->p) {
	case '"':
		f->type = MG_JSON_STRING;
		if (parse_string(c, &s, &f->str_len)) {
			return -EINVAL;
		}
		f->str = s;
		return 0;
	case '{':
	case '[':
		f->type = MG_JSON_NESTED;
		return skip_container(c, depth + 1);
	case 't':
		f->type = MG_JSON_TRUE;
		return literal(c, "true");
	case 'f':
		f->type = MG_JSON_FALSE;
		return literal(c, "false");
	case 'n':
		f->type = MG_JSON_NULL;
		return literal(c, "null");
	default:
		f->type = MG_JSON_NUMBER;
		return parse_number(c, &f->num, &f->num_is_int);
	}
}

int mg_json_parse(char *buf, size_t len, struct mg_json_obj *out)
{
	struct cur c = {.p = buf, .end = buf + len};

	out->n = 0;
	skip_ws(&c);
	if (c.p >= c.end || *c.p != '{') {
		return -EINVAL;
	}
	c.p++;
	skip_ws(&c);
	if (c.p < c.end && *c.p == '}') {
		c.p++;
	} else {
		for (;;) {
			struct mg_json_field f;
			char *k;
			size_t kl;
			int err;

			skip_ws(&c);
			if (parse_string(&c, &k, &kl)) {
				return -EINVAL;
			}
			skip_ws(&c);
			if (c.p >= c.end || *c.p != ':') {
				return -EINVAL;
			}
			c.p++;
			err = parse_value(&c, &f, 1);
			if (err) {
				return err;
			}
			f.key = k;
			if (mg_json_get(out, k) != NULL) {
				return -EINVAL; /* duplicate member */
			}
			if (out->n >= MG_JSON_MAX_FIELDS) {
				return -E2BIG;
			}
			out->f[out->n++] = f;
			skip_ws(&c);
			if (c.p < c.end && *c.p == ',') {
				c.p++;
				continue;
			}
			if (c.p < c.end && *c.p == '}') {
				c.p++;
				break;
			}
			return -EINVAL;
		}
	}
	skip_ws(&c);
	return c.p == c.end ? 0 : -EINVAL;
}

const struct mg_json_field *mg_json_get(const struct mg_json_obj *o, const char *key)
{
	for (size_t i = 0; i < o->n; i++) {
		if (strcmp(o->f[i].key, key) == 0) {
			return &o->f[i];
		}
	}
	return NULL;
}

const char *mg_json_str(const struct mg_json_obj *o, const char *key)
{
	const struct mg_json_field *f = mg_json_get(o, key);

	return f && f->type == MG_JSON_STRING ? f->str : NULL;
}

/* ---- writer ---- */

static void put(struct mg_jw *w, const char *s, size_t n)
{
	if (w->err || w->len + n + 1 > w->cap) {
		w->err = true;
		return;
	}
	memcpy(&w->buf[w->len], s, n);
	w->len += n;
}

static void put_escaped(struct mg_jw *w, const char *s)
{
	put(w, "\"", 1);
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		char esc[8];

		if (ch == '"' || ch == '\\') {
			esc[0] = '\\';
			esc[1] = (char)ch;
			put(w, esc, 2);
		} else if (ch < 0x20) {
			static const char hex[] = "0123456789abcdef";

			memcpy(esc, "\\u00", 4);
			esc[4] = hex[ch >> 4];
			esc[5] = hex[ch & 0xF];
			put(w, esc, 6);
		} else {
			put(w, (const char *)&ch, 1);
		}
	}
	put(w, "\"", 1);
}

static void key(struct mg_jw *w, const char *k)
{
	if (w->len > 1) {
		put(w, ",", 1);
	}
	put_escaped(w, k);
	put(w, ":", 1);
}

void mg_jw_begin(struct mg_jw *w, char *buf, size_t cap)
{
	w->buf = buf;
	w->cap = cap;
	w->len = 0;
	w->err = cap == 0;
	put(w, "{", 1);
}

void mg_jw_str(struct mg_jw *w, const char *k, const char *val)
{
	key(w, k);
	put_escaped(w, val ? val : "");
}

void mg_jw_int(struct mg_jw *w, const char *k, long long val)
{
	char num[24];
	size_t i = sizeof(num);
	unsigned long long u = val < 0 ? 0ULL - (unsigned long long)val : (unsigned long long)val;

	do {
		num[--i] = (char)('0' + u % 10);
		u /= 10;
	} while (u);
	if (val < 0) {
		num[--i] = '-';
	}
	key(w, k);
	put(w, &num[i], sizeof(num) - i);
}

int mg_jw_end(struct mg_jw *w)
{
	put(w, "}", 1);
	if (w->err) {
		if (w->cap) {
			w->buf[0] = '\0';
		}
		return -ENOSPC;
	}
	w->buf[w->len] = '\0';
	return (int)w->len;
}
