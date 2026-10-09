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
 * BabbleSim entry points built into the gadget image (CONFIG_MG_BSIM_TESTS).
 * The app runs unchanged; each test only presses the button at the right
 * moments, the way a person would. tests/bsim/central checks the results.
 */

#include <zephyr/kernel.h>

#include "babblekit/testcase.h"
#include "bstests.h"

#include "mg_button.h"
#include "mg_core.h"
#include "mg_session.h"
#include "mg_settings.h"
#include "mg_setup.h"

enum script {
	SCRIPT_PTT,
	SCRIPT_OFFLINE,
	SCRIPT_SETUP,
	SCRIPT_PAIR,
};

static enum script script;

static void press(int ms)
{
	mg_button_inject(true);
	k_sleep(K_MSEC(ms));
	mg_button_inject(false);
}

static void wait_for(bool (*cond)(void))
{
	while (!cond()) {
		k_sleep(K_MSEC(20));
	}
}

static bool ready_with_ptt(void)
{
	return mg_core_is_ready() && mg_settings_get()->push_to_talk_enabled;
}

static bool setup_confirm_pending(void)
{
	return mg_setup_confirm_pending();
}

static bool pair_pending(void)
{
	return mg_session_pair_pending();
}

static bool queue_on_and_alone(void)
{
	return mg_settings_get()->audio_queue_enabled && !mg_core_is_ready();
}

static void script_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	switch (script) {
	case SCRIPT_PTT:
		/* The central enables push-to-talk once connected; one utterance. */
		wait_for(ready_with_ptt);
		k_sleep(K_MSEC(1000));
		press(1500);
		TEST_PASS("pressed push-to-talk for 1.5 s");
		break;
	case SCRIPT_OFFLINE:
		/* Once the central has turned the queue on and left: two utterances. */
		wait_for(queue_on_and_alone);
		k_sleep(K_MSEC(500));
		press(1200);
		k_sleep(K_MSEC(500));
		press(800);
		TEST_PASS("recorded two offline utterances");
		break;
	case SCRIPT_SETUP:
		/* Confirm Muse Link setup on the device, like a user would. */
		wait_for(setup_confirm_pending);
		k_sleep(K_MSEC(300));
		press(100);
		TEST_PASS("pressed the button to confirm Link setup");
		break;
	case SCRIPT_PAIR:
		/* Accept the pairing request on the device, like a user would. */
		wait_for(pair_pending);
		k_sleep(K_MSEC(300));
		press(100);
		TEST_PASS("pressed the button to accept pairing");
		break;
	}
}

K_THREAD_STACK_DEFINE(script_stack, 2048);
static struct k_thread script_thread;

static void start(void)
{
	bst_result = In_progress;
	k_thread_create(&script_thread, script_stack, K_THREAD_STACK_SIZEOF(script_stack), script_fn,
			NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
}

static void start_ptt(void)
{
	script = SCRIPT_PTT;
	start();
}

static void start_offline(void)
{
	script = SCRIPT_OFFLINE;
	start();
}

static void start_setup(void)
{
	script = SCRIPT_SETUP;
	start();
}

static void start_pair(void)
{
	script = SCRIPT_PAIR;
	start();
}

static const struct bst_test_instance tests[] = {
	{
		.test_id = "gadget_ptt",
		.test_descr = "Gadget: one push-to-talk utterance once a client is ready",
		.test_fake_ddriver_postkernel_f = start_ptt,
	},
	{
		.test_id = "gadget_offline",
		.test_descr = "Gadget: two utterances into the offline queue",
		.test_fake_ddriver_postkernel_f = start_offline,
	},
	{
		.test_id = "gadget_setup",
		.test_descr = "Gadget: confirm Muse Link setup with the button",
		.test_fake_ddriver_postkernel_f = start_setup,
	},
	{
		.test_id = "gadget_pair",
		.test_descr = "Gadget: accept a pairing request with the button",
		.test_fake_ddriver_postkernel_f = start_pair,
	},
	BSTEST_END_MARKER,
};

static struct bst_test_list *install(struct bst_test_list *list)
{
	return bst_add_tests(list, tests);
}

bst_test_install_t test_installers[] = {install, NULL};
