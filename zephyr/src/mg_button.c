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

#include "mg_button.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mg_core.h"
#include "mg_transport.h"

LOG_MODULE_REGISTER(mg_button, CONFIG_MG_LOG_LEVEL);

/* Button events are queued so none is lost between work runs. */
K_MSGQ_DEFINE(button_q, sizeof(uint8_t), 8, 1);
static struct k_work button_work;

static void button_fn(struct k_work *w)
{
	uint8_t pressed;

	ARG_UNUSED(w);
	while (k_msgq_get(&button_q, &pressed, K_NO_WAIT) == 0) {
		LOG_INF("button %s", pressed ? "down" : "up");
		mg_core_button(pressed);
	}
}

void mg_button_inject(bool pressed)
{
	uint8_t v = pressed;

	if (k_msgq_put(&button_q, &v, K_NO_WAIT) == 0) {
		k_work_submit_to_queue(mg_app_wq(), &button_work);
	}
}

static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type == INPUT_EV_KEY && evt->code == INPUT_KEY_ENTER) {
		mg_button_inject(evt->value != 0);
	}
}

INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

bool mg_button_held_at_boot(void)
{
#if DT_NODE_EXISTS(DT_ALIAS(sw0))
	static const struct gpio_dt_spec sw = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

	if (!gpio_is_ready_dt(&sw)) {
		return false;
	}
	(void)gpio_pin_configure_dt(&sw, GPIO_INPUT);
	return gpio_pin_get_dt(&sw) == 1;
#else
	return false;
#endif
}

void mg_button_init(void)
{
	k_work_init(&button_work, button_fn);
}
