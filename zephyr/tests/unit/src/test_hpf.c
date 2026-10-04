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


#include <math.h>
#include <stdlib.h>

#include <zephyr/ztest.h>

#include "mg_hpf.h"

#define RATE 16000

static float rms(const int16_t *x, size_t n)
{
	double s = 0;

	for (size_t i = 0; i < n; i++) {
		s += (double)x[i] * x[i];
	}
	return (float)sqrt(s / n);
}

/* A DC step (the PDM mic's offset jumping by 1000 counts mid-capture)
 * settles within 50 ms. */
ZTEST(hpf, test_dc_step_settles)
{
	static int16_t x[RATE / 5]; /* 200 ms in 10 ms blocks */
	struct mg_hpf f;

	mg_hpf_init(&f, 30, RATE);
	for (size_t i = 0; i < ARRAY_SIZE(x); i++) {
		x[i] = i < RATE / 50 ? 0 : 1000; /* step at 20 ms */
	}
	for (size_t off = 0; off < ARRAY_SIZE(x); off += 160) {
		mg_hpf_run(&f, &x[off], 160);
	}
	zassert_true(x[RATE / 50] > 900, "the step itself passes (%d)", x[RATE / 50]);
	/* 50 ms after the step: within a count of zero, and stays there. */
	for (size_t i = RATE / 50 + RATE / 20; i < ARRAY_SIZE(x); i++) {
		zassert_true(abs(x[i]) <= 1, "sample %u is %d", (unsigned int)i, x[i]);
	}
}

/* Primed at the start of a capture: a mic sitting at -1100 starts at zero,
 * with no settling transient in the first frames. */
ZTEST(hpf, test_primed_start)
{
	static int16_t x[480];
	struct mg_hpf f;

	mg_hpf_init(&f, 30, RATE);
	for (size_t i = 0; i < ARRAY_SIZE(x); i++) {
		x[i] = -1100;
	}
	mg_hpf_run(&f, x, ARRAY_SIZE(x));
	for (size_t i = 0; i < ARRAY_SIZE(x); i++) {
		zassert_equal(x[i], 0, "sample %u is %d", (unsigned int)i, x[i]);
	}
	/* And again after a reset (the next capture), from a new level. */
	mg_hpf_reset(&f);
	for (size_t i = 0; i < ARRAY_SIZE(x); i++) {
		x[i] = 500;
	}
	mg_hpf_run(&f, x, 160);
	zassert_equal(x[0], 0);
	zassert_equal(x[159], 0);
}

/* Speech passes: a 300 Hz tone on a DC offset loses the offset, not the tone
 * (under 0.2 dB), and a 30 Hz corner leaves 1 kHz untouched. */
ZTEST(hpf, test_passes_speech)
{
	static int16_t x[RATE / 2];
	struct mg_hpf f;

	for (unsigned int hz = 300; hz <= 1000; hz += 700) {
		mg_hpf_init(&f, 30, RATE);
		for (size_t i = 0; i < ARRAY_SIZE(x); i++) {
			x[i] = (int16_t)(-1100 + 8000 * sinf(2 * 3.14159265f * hz * i / RATE));
		}
		mg_hpf_run(&f, x, ARRAY_SIZE(x));
		/* Skip the first 100 ms; the rest is the tone alone. */
		const int16_t *t = &x[RATE / 10];
		size_t n = ARRAY_SIZE(x) - RATE / 10;
		long sum = 0;

		for (size_t i = 0; i < n; i++) {
			sum += t[i];
		}
		zassert_true(labs(sum / (long)n) < 20, "DC left: %ld", sum / (long)n);
		float db = 20 * log10f(rms(t, n) / (8000 / sqrtf(2)));

		zassert_true(db > -0.2f && db < 0.2f, "%u Hz: %.2f dB", hz, (double)db);
	}
}

/* 0 Hz: off, samples untouched. */
ZTEST(hpf, test_off)
{
	int16_t x[4] = {-1100, 5, 7, 32767};
	struct mg_hpf f;

	mg_hpf_init(&f, 0, RATE);
	mg_hpf_run(&f, x, 4);
	zassert_equal(x[0], -1100);
	zassert_equal(x[3], 32767);
}

ZTEST_SUITE(hpf, NULL, NULL, NULL, NULL, NULL);
