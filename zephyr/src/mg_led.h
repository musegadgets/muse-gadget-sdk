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


#ifndef MG_LED_H_
#define MG_LED_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

/*
 * The LED shows the state and stands in for the missing buzzer. The
 * highest-priority one that applies picks the pattern: a pairing request,
 * an offline recording, listening (capturing), pairing mode, the assistant
 * state, then the link. Each change is logged ("mg.led: <pattern>") on the
 * console and the Nordic UART mirror.
 */
enum mg_led_flag {
	MG_LED_PAIRING_MODE = BIT(1), /* double blink */
	MG_LED_STREAMING = BIT(2),    /* solid: listening */
	MG_LED_RECORDING = BIT(3),    /* fast blink */
	MG_LED_PAIR_PENDING = BIT(4), /* very fast blink */
};

/* Where the client is (mg_core.c decides; same states as the ESP32 gadgets). */
enum mg_led_link {
	MG_LED_LINK_NEVER,        /* no client since boot, no device tokens */
	MG_LED_LINK_DISCONNECTED, /* advertising */
	MG_LED_LINK_CONNECTING,   /* connected, not subscribed */
	MG_LED_LINK_SESSION,      /* a client's session, push-to-talk off */
	MG_LED_LINK_READY,        /* ...and push-to-talk on */
};

/* The assistant's turn (mg_command_assistant_state, and thinking at stop_mic). */
enum mg_led_face {
	MG_LED_FACE_IDLE,
	MG_LED_FACE_THINKING,   /* until responding, done or error; 60 s at most */
	MG_LED_FACE_RESPONDING, /* until done or error; 60 s at most */
	MG_LED_FACE_DONE,       /* a short happy flourish, then idle */
	MG_LED_FACE_ERROR,      /* 3 s, then idle */
};

void mg_led_set(uint32_t flag, bool on);
void mg_led_set_link(enum mg_led_link link);
/* Logs "mg.face: <name>" with why (may be NULL). */
void mg_led_set_face(enum mg_led_face face, const char *why);
enum mg_led_face mg_led_face(void);
const char *mg_led_link_name(enum mg_led_link link);
/* The push-to-talk activation "buzz": the LED lights for duration_ms. */
void mg_led_buzz(uint16_t freq_hz, uint16_t duration_ms, uint8_t volume);
void mg_led_init(void);

#endif
