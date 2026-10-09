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


/*
 * Bench console (CONFIG_MG_BENCH): single-key commands on the console UART,
 * like the ESP32 bench, so the gadget can be driven from a test rig:
 *
 *   d / u   talk button down / up, through the real button path
 *   s       status: link, push-to-talk, battery, image, offline clips
 *   ?       this list
 *
 * and one line command, as on the ESP32's console:
 *
 *   >pair.confirm   confirm a pending Muse Link setup as the button would,
 *                   answering "@pair.confirm confirmed" or "none" (never
 *                   push-to-talk); "@pair.pending" says one is waiting
 */

#include <string.h>

#include <zephyr/console/console.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "mg_app.h"
#include "mg_button.h"
#include "mg_core.h"
#include "mg_session.h"
#include "mg_settings.h"
#include "mg_setup.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_AUDIO_QUEUE)
#include "mg_queue.h"
#endif
#if defined(CONFIG_MG_DFU)
#include "mg_ota.h"
#endif

static void status(void)
{
	const struct mg_settings *st = mg_settings_get();
	const char *link = !mg_core_is_connected() ? "disconnected"
			   : !mg_core_is_ready()    ? "connecting"
			   : st->push_to_talk_enabled ? "ready"
						     : "app connected";

	printk("bench: link %s, ptt %s, setup %s, token proof %s, session %s\n", link,
	       st->push_to_talk_enabled ? "on" : "off", mg_setup_store_complete() ? "done" : "none",
	       mg_token_proof_matched() ? "matched" : "none",
	       mg_session_authenticated() ? "authenticated" : "none");
	printk("bench: battery %d mV, %u%%\n", mg_app_battery_mv(), mg_app_battery_percent());
#if defined(CONFIG_MG_DFU)
	printk("bench: image %s, %s%s\n", mg_app_version(),
	       mg_ota_confirmed() ? "confirmed" : "on test (not confirmed)",
	       mg_ota_upload_active() ? ", upload in progress" : "");
#else
	printk("bench: image %s\n", mg_app_version());
#endif
#if defined(CONFIG_MG_AUDIO_QUEUE)
	struct mg_queue_status q;

	mg_queue_get_status(&q);
	printk("bench: queue %s (%s), %u clips, %u of %u bytes\n",
	       st->audio_queue_enabled ? "on" : "off", mg_queue_owner(), q.clip_count, q.used_bytes,
	       q.capacity_bytes);
#endif
}

#define BENCH_HELP "bench: keys d (talk down), u (talk up), s (status); line >pair.confirm\n"

/* Setup state lives on the app work queue. */
static void confirm_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	bool confirmed = mg_setup_confirm_pending() && mg_setup_button();

	printk("@pair.confirm %s\n", confirmed ? "confirmed" : "none");
}

static K_WORK_DEFINE(confirm_work, confirm_fn);

/* The rest of a '>' line; an unknown or too long one is reported. */
static void line_command(void)
{
	char line[24];
	size_t len = 0;
	bool whole = true;

	for (;;) {
		int ch = console_getchar();

		if (ch == '\n') {
			break;
		}
		if (ch == '\r') {
			continue;
		}
		if (len < sizeof(line) - 1) {
			line[len++] = (char)ch;
		} else {
			whole = false;
		}
	}
	line[len] = '\0';
	if (whole && strcmp(line, "pair.confirm") == 0) {
		k_work_submit_to_queue(mg_app_wq(), &confirm_work);
	} else {
		printk("@error unknown command\n");
	}
}

static void bench_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	(void)console_init();
	printk(BENCH_HELP);
	for (;;) {
		int ch = console_getchar();

		switch (ch) {
		case 'd':
			printk("bench: talk down\n");
			mg_button_inject(true);
			break;
		case 'u':
			printk("bench: talk up\n");
			mg_button_inject(false);
			break;
		case 's':
			status();
			break;
		case '>':
			line_command();
			break;
		case '?':
		case 'h':
			printk(BENCH_HELP);
			break;
		default:
			break;
		}
	}
}

K_THREAD_STACK_DEFINE(bench_stack, 1536);
static struct k_thread bench_thread;

void mg_bench_init(void)
{
	k_thread_create(&bench_thread, bench_stack, K_THREAD_STACK_SIZEOF(bench_stack), bench_fn,
			NULL, NULL, NULL, K_PRIO_PREEMPT(9), 0, K_NO_WAIT);
	k_thread_name_set(&bench_thread, "mg_bench");
}
