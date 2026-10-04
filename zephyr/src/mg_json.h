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

#ifndef MG_JSON_H_
#define MG_JSON_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A small strict JSON reader for the flat objects of Muse Link setup
 * (pairing_client_hello, pairing_encrypted, provision_v2, ...), and a writer
 * for the flat objects the device sends. Portable C, no allocation.
 *
 * The reader accepts one JSON object (RFC 8259) and nothing else around it
 * but whitespace. It exposes the object's top-level members; a member whose
 * value is an object or array is checked (nesting at most MG_JSON_MAX_DEPTH
 * deep) and reported as MG_JSON_NESTED. Strings are decoded in place (the
 * escapes, \uXXXX with surrogate pairs to UTF-8) and NUL-terminated there,
 * so the buffer must stay alive while the fields are used. It refuses
 * duplicate member names, raw control characters and \u0000 in strings,
 * lone surrogates, and more than MG_JSON_MAX_FIELDS members.
 */

#define MG_JSON_MAX_FIELDS 24
#define MG_JSON_MAX_DEPTH  32

enum mg_json_type {
	MG_JSON_STRING,
	MG_JSON_NUMBER,
	MG_JSON_TRUE,
	MG_JSON_FALSE,
	MG_JSON_NULL,
	MG_JSON_NESTED,
};

struct mg_json_field {
	const char *key;
	enum mg_json_type type;
	const char *str; /* MG_JSON_STRING: decoded, NUL-terminated */
	size_t str_len;
	int64_t num;     /* MG_JSON_NUMBER with num_is_int */
	bool num_is_int; /* no fraction or exponent, and fits */
};

struct mg_json_obj {
	struct mg_json_field f[MG_JSON_MAX_FIELDS];
	size_t n;
};

/* 0, or -EINVAL (not one valid object), -E2BIG (too many members or too
 * deep). Modifies buf[0..len). */
int mg_json_parse(char *buf, size_t len, struct mg_json_obj *out);

const struct mg_json_field *mg_json_get(const struct mg_json_obj *o, const char *key);
/* The member's string value, or NULL if it is absent or not a string. */
const char *mg_json_str(const struct mg_json_obj *o, const char *key);

/* Writer: {"k":"v","n":1}. Strings are escaped. */
struct mg_jw {
	char *buf;
	size_t cap;
	size_t len;
	bool err;
};

void mg_jw_begin(struct mg_jw *w, char *buf, size_t cap);
void mg_jw_str(struct mg_jw *w, const char *key, const char *val);
void mg_jw_int(struct mg_jw *w, const char *key, long long val);
/* Closes the object and NUL-terminates; the length, or -ENOSPC. */
int mg_jw_end(struct mg_jw *w);

#endif
