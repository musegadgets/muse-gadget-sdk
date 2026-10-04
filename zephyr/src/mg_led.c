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

#include "mg_led.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mg_led, CONFIG_MG_LOG_LEVEL);

#if DT_NODE_EXISTS(DT_ALIAS(led0))
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAVE_LED 1
#else
#define HAVE_LED 0
#endif

/* Each pattern alternates on/off durations (ms); 0 ends it and it repeats.
 * A single entry means solid on. */
struct pattern {
	const char *name;
	uint16_t ms[8];
};

static const struct pattern pat_advertising = {"advertising", {30, 1970, 0}};
static const struct pattern pat_connecting = {"app connected, not ready", {500, 1500, 0}};
static const struct pattern pat_ready = {"ready", {20, 3980, 0}};
static const struct pattern pat_pairing = {"pairing mode", {60, 140, 60, 1740, 0}};
static const struct pattern pat_listening = {"listening", {1, 0}};
static const struct pattern pat_recording = {"recording offline", {100, 100, 0}};
static const struct pattern pat_pending = {"confirm pairing", {50, 50, 0}};
static const struct pattern pat_thinking = {"thinking", {150, 350, 0}};
static const struct pattern pat_responding = {"responding", {400, 100, 0}};
static const struct pattern pat_done = {"done", {60, 60, 60, 60, 60, 60, 0}};
static const struct pattern pat_error = {"error", {900, 100, 0}};

#define DONE_MS    720
#define ERROR_MS   3000
#define TURN_MS    60000

static atomic_t flags;
static atomic_t link = MG_LED_LINK_NEVER;
static atomic_t face = MG_LED_FACE_IDLE;
static int64_t face_until;
static const struct pattern *pat;
static const struct pattern *logged;
static int step;
static int64_t buzz_until;
static struct k_work_delayable tick;

static const char *const FACE_NAMES[] = {"idle", "thinking", "responding", "done", "error"};
static const char *const LINK_NAMES[] = {"never connected", "disconnected", "connecting",
					 "app connected", "ready"};

const char *mg_led_link_name(enum mg_led_link l)
{
	return l < ARRAY_SIZE(LINK_NAMES) ? LINK_NAMES[l] : "?";
}

static void out(bool on)
{
#if HAVE_LED
	gpio_pin_set_dt(&led, on);
#else
	ARG_UNUSED(on);
#endif
}

static const struct pattern *pick(void)
{
	atomic_val_t f = atomic_get(&flags);

	if (f & MG_LED_PAIR_PENDING) {
		return &pat_pending;
	}
	if (f & MG_LED_RECORDING) {
		return &pat_recording;
	}
	if (f & MG_LED_STREAMING) {
		return &pat_listening;
	}
	if (f & MG_LED_PAIRING_MODE) {
		return &pat_pairing;
	}
	switch ((enum mg_led_face)atomic_get(&face)) {
	case MG_LED_FACE_THINKING:
		return &pat_thinking;
	case MG_LED_FACE_RESPONDING:
		return &pat_responding;
	case MG_LED_FACE_DONE:
		return &pat_done;
	case MG_LED_FACE_ERROR:
		return &pat_error;
	default:
		break;
	}
	switch ((enum mg_led_link)atomic_get(&link)) {
	case MG_LED_LINK_READY:
		return &pat_ready;
	case MG_LED_LINK_CONNECTING:
	case MG_LED_LINK_SESSION:
		return &pat_connecting;
	default:
		return &pat_advertising;
	}
}

static void tick_fn(struct k_work *w)
{
	int64_t now = k_uptime_get();

	ARG_UNUSED(w);
	if (face_until && now >= face_until) {
		enum mg_led_face was = atomic_get(&face);

		face_until = 0;
		atomic_set(&face, MG_LED_FACE_IDLE);
		if (was == MG_LED_FACE_THINKING || was == MG_LED_FACE_RESPONDING) {
			LOG_INF("mg.face: idle (no assistant_state for %d s)", TURN_MS / 1000);
		} else {
			LOG_INF("mg.face: idle");
		}
	}
	if (buzz_until > now) {
		out(true);
		k_work_reschedule(&tick, K_MSEC(buzz_until - now));
		return;
	}
	const struct pattern *p = pick();

	if (p != pat) {
		pat = p;
		step = 0;
		if (p != logged) {
			logged = p;
			LOG_INF("mg.led: %s", p->name);
		}
	}
	int64_t next;

	if (pat->ms[1] == 0) {
		out(true); /* solid */
		next = face_until ? face_until - now : -1;
	} else {
		if (pat->ms[step] == 0) {
			step = 0;
		}
		out((step & 1) == 0);
		next = pat->ms[step];
		step++;
		if (face_until && face_until - now < next) {
			next = face_until - now;
		}
	}
	if (next >= 0) {
		k_work_reschedule(&tick, K_MSEC(MAX(next, 1)));
	}
}

static void kick(void)
{
	pat = NULL; /* restart the pattern */
	k_work_reschedule(&tick, K_NO_WAIT);
}

void mg_led_set(uint32_t flag, bool on)
{
	if (flag == MG_LED_STREAMING && on && !(atomic_get(&flags) & MG_LED_STREAMING)) {
		LOG_INF("mg.face: listening");
	}
	if (on) {
		atomic_or(&flags, flag);
	} else {
		atomic_and(&flags, ~flag);
	}
	kick();
}

void mg_led_set_link(enum mg_led_link l)
{
	atomic_set(&link, l);
	kick();
}

void mg_led_set_face(enum mg_led_face f, const char *why)
{
	int64_t now = k_uptime_get();

	atomic_set(&face, f);
	switch (f) {
	case MG_LED_FACE_THINKING:
	case MG_LED_FACE_RESPONDING:
		face_until = now + TURN_MS;
		break;
	case MG_LED_FACE_DONE:
		face_until = now + DONE_MS;
		break;
	case MG_LED_FACE_ERROR:
		face_until = now + ERROR_MS;
		break;
	default:
		face_until = 0;
		break;
	}
	if (why) {
		LOG_INF("mg.face: %s (%s)", FACE_NAMES[f], why);
	} else {
		LOG_INF("mg.face: %s", FACE_NAMES[f]);
	}
	kick();
}

enum mg_led_face mg_led_face(void)
{
	return atomic_get(&face);
}

void mg_led_buzz(uint16_t freq_hz, uint16_t duration_ms, uint8_t volume)
{
	/* No buzzer on this board: the LED lights for the buzz duration.
	 * Frequency and volume are accepted and stored but have no effect. */
	ARG_UNUSED(freq_hz);
	if (volume == 0) {
		return;
	}
	buzz_until = k_uptime_get() + duration_ms;
	k_work_reschedule(&tick, K_NO_WAIT);
}

void mg_led_init(void)
{
	k_work_init_delayable(&tick, tick_fn);
#if HAVE_LED
	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
#endif
	k_work_reschedule(&tick, K_NO_WAIT);
}
