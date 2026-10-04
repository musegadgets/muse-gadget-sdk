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


#include "mg_ota.h"

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>

#include "mg_app.h"
#include "mg_connparam.h"
#include "mg_audio.h"
#include "mg_session.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
#include "mg_queue.h"
#endif

LOG_MODULE_REGISTER(mg_ota, CONFIG_MG_LOG_LEVEL);

static volatile bool upload_active;

static const char *swap_name(int t)
{
	switch (t) {
	case BOOT_SWAP_TYPE_NONE: return "none";
	case BOOT_SWAP_TYPE_TEST: return "test";
	case BOOT_SWAP_TYPE_PERM: return "permanent";
	case BOOT_SWAP_TYPE_REVERT: return "revert";
	default: return "unknown";
	}
}

bool mg_ota_confirmed(void)
{
	return boot_is_img_confirmed();
}

bool mg_ota_upload_active(void)
{
	return upload_active;
}

#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
static bool slot_needed(void)
{
	int swap = mcuboot_swap_type();

	return mg_queue_slot_needed_by_ota(upload_active, swap != BOOT_SWAP_TYPE_NONE,
					   boot_is_img_confirmed());
}

/* Recording or a clip download must not touch the slot any more. */
static void stop_audio_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	enum mg_audio_activity a = mg_audio_activity();

	if (a == MG_AUDIO_RECORDING || a == MG_AUDIO_CLIP) {
		mg_audio_stop();
	}
}

static K_WORK_DEFINE(stop_audio_work, stop_audio_fn);
#endif

static enum mgmt_cb_return on_img(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
				  uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(prev);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(size);

	switch (event) {
	case MGMT_EVT_OP_IMG_MGMT_DFU_STARTED:
		/* Runs before img_mgmt writes the first chunk. */
		mg_connparam_busy(MG_XFER_DFU);
		upload_active = true;
		LOG_INF("firmware upload started");
#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
		(void)mg_queue_hand_to_ota();
		k_work_submit_to_queue(mg_app_wq(), &stop_audio_work);
#endif
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED:
		/* Finished or abandoned; the slot stays OTA's until a reset. */
		upload_active = false;
		LOG_INF("firmware upload stopped");
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_PENDING:
		LOG_INF("firmware update pending: it installs at the next reset");
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_CONFIRMED:
		LOG_INF("firmware image confirmed");
		break;
	default:
		break;
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback img_cb = {
	.callback = on_img,
	.event_id = MGMT_EVT_OP_IMG_MGMT_ALL,
};

/*
 * Every SMP request, before its handler runs. An image group request (an
 * upload chunk, or the list) keeps the link listening on every connection
 * event: each one restarts the hold, CONFIG_MG_CONN_LATENCY_HOLD_MS.
 */
static enum mgmt_cb_return on_cmd(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
				  uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(prev);
	ARG_UNUSED(group);
	const struct mgmt_evt_op_cmd_arg *cmd = data;

	ARG_UNUSED(rc);
	ARG_UNUSED(abort_more);
	if (size >= sizeof(*cmd) && cmd->group == MGMT_GROUP_ID_IMAGE) {
		/* Uploads, and the image list a client reads just before one, so
		 * latency is already off when the first chunk arrives. */
		mg_connparam_busy(MG_XFER_DFU);
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback cmd_cb = {
	.callback = on_cmd,
	.event_id = MGMT_EVT_OP_CMD_RECV,
};

void mg_ota_init(void)
{
	int swap = mcuboot_swap_type();
	bool confirmed = boot_is_img_confirmed();

	LOG_INF("image %s, %s; MCUboot swap: %s", mg_app_version(),
		confirmed ? "confirmed" : "on test", swap_name(swap));
	mgmt_callback_register(&img_cb);
	mgmt_callback_register(&cmd_cb);
#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
	if (slot_needed()) {
		/* An image MCUboot may install or swap back sits in the slot. */
		(void)mg_queue_hand_to_ota();
	} else if (!mg_queue_ready()) {
		/* An old image, an abandoned upload or older data: MCUboot is done with it. */
		(void)mg_queue_reclaim();
	}
#endif
}

void mg_ota_healthy(void)
{
	if (IS_ENABLED(CONFIG_MG_BENCH_NO_CONFIRM) && !boot_is_img_confirmed()) {
		/* Bench build for the revert test: MCUboot swaps the previous image
		 * back at the next reset. */
		LOG_WRN("image %s left unconfirmed (CONFIG_MG_BENCH_NO_CONFIRM): the next reset "
			"reverts to the previous image",
			mg_app_version());
	} else if (!boot_is_img_confirmed()) {
		int err = boot_write_img_confirmed();

		LOG_INF("image %s confirmed (%d): it stays after the next reset", mg_app_version(),
			err);
	}
#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
	if (!mg_queue_ready() && !slot_needed()) {
		(void)mg_queue_reclaim();
	}
#endif
}
