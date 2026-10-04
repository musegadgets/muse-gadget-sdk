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

#include "mg_battery.h"

#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mg_battery, CONFIG_MG_LOG_LEVEL);

/* VBAT on /zephyr,user io-channels, behind a divider of
 * CONFIG_MG_BATTERY_DIVIDER_RATIO. */
static const struct adc_dt_spec vbat = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

/* Single-cell LiPo, resting voltage to charge (mV, %). */
static const struct {
	uint16_t mv;
	uint8_t pct;
} curve[] = {
	{4150, 100}, {4050, 90}, {3970, 80}, {3900, 70}, {3840, 60}, {3790, 50},
	{3750, 40},  {3710, 30}, {3670, 20}, {3610, 10}, {3500, 5},  {3300, 0},
};

uint8_t mg_battery_percent_from_mv(int mv)
{
	if (mv >= curve[0].mv) {
		return 100;
	}
	for (size_t i = 1; i < ARRAY_SIZE(curve); i++) {
		if (mv >= curve[i].mv) {
			int span = curve[i - 1].mv - curve[i].mv;

			return curve[i].pct +
			       (curve[i - 1].pct - curve[i].pct) * (mv - curve[i].mv) / span;
		}
	}
	return 0;
}

int mg_battery_read_mv(void)
{
	int16_t raw;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	int err;

	if (!adc_is_ready_dt(&vbat)) {
		return -ENODEV;
	}
	err = adc_channel_setup_dt(&vbat);
	err = err ?: adc_sequence_init_dt(&vbat, &seq);
	err = err ?: adc_read_dt(&vbat, &seq);
	if (err) {
		return err;
	}
	int32_t mv = raw;

	err = adc_raw_to_millivolts_dt(&vbat, &mv);
	if (err) {
		return err;
	}
	return mv * CONFIG_MG_BATTERY_DIVIDER_RATIO;
}
