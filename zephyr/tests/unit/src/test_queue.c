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

#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_queue.h"

#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
#define QUEUE_PART PARTITION_ID(slot1_partition)
#define QTRAILER   CONFIG_MG_QUEUE_SLOT1_TRAILER
#else
#define QUEUE_PART PARTITION_ID(audio_queue_partition)
#define QTRAILER   0
#endif

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	zassert_equal(mg_queue_clear(), 0);
}

static void fill(uint8_t *b, size_t n, uint8_t seed)
{
	for (size_t i = 0; i < n; i++) {
		b[i] = (uint8_t)(seed + i * 7);
	}
}

ZTEST(queue, test_empty)
{
	struct mg_queue_status st;
	struct mg_clip_info ci;

	zassert_true(mg_queue_ready());
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 0);
	zassert_equal(st.used_bytes, 0);
	zassert_equal(st.capacity_bytes, 0x69000 - QTRAILER);
	zassert_equal(mg_queue_clip_info(0, &ci), -ENOENT);
}

ZTEST(queue, test_clips_roundtrip)
{
	static uint8_t a[1000], b[333], rd[1000];
	struct mg_clip_info ci;
	struct mg_queue_status st;

	fill(a, sizeof(a), 1);
	fill(b, sizeof(b), 99);

	/* Clip 0 appended in frame-sized pieces, clip 1 in one go. */
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 100), 0);
	zassert_true(mg_queue_recording());
	for (size_t off = 0; off < sizeof(a); off += 40) {
		zassert_equal(mg_queue_append(&a[off], MIN(40, sizeof(a) - off)), 0);
	}
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 1), -EBUSY);
	zassert_equal(mg_queue_end(), 0);
	zassert_false(mg_queue_recording());
	zassert_equal(mg_queue_begin(mg_data_type_audio_lc3, 5000), 0);
	zassert_equal(mg_queue_append(b, sizeof(b)), 0);
	zassert_equal(mg_queue_end(), 0);

	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 2);
	zassert_true(st.used_bytes >= sizeof(a) + sizeof(b) + 64);

	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.codec, mg_data_type_audio_sbc);
	zassert_equal(ci.start_ms, 100);
	zassert_equal(ci.bytes, sizeof(a));
	zassert_equal(mg_queue_read(0, 0, rd, sizeof(a)), 0);
	zassert_mem_equal(rd, a, sizeof(a));
	zassert_equal(mg_queue_read(0, 990, rd, 10), 0);
	zassert_mem_equal(rd, &a[990], 10);
	zassert_equal(mg_queue_read(0, 995, rd, 10), -EINVAL, "past the end");

	zassert_equal(mg_queue_clip_info(1, &ci), 0);
	zassert_equal(ci.codec, mg_data_type_audio_lc3);
	zassert_equal(ci.start_ms, 5000);
	zassert_equal(ci.bytes, sizeof(b));
	zassert_equal(mg_queue_read(1, 0, rd, sizeof(b)), 0);
	zassert_mem_equal(rd, b, sizeof(b));

	/* Survives a reboot (re-scan). */
	zassert_equal(mg_queue_init(), 0);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 2);
	zassert_equal(mg_queue_read(1, 0, rd, sizeof(b)), 0);
	zassert_mem_equal(rd, b, sizeof(b));

	/* Appending after a re-scan continues after the last clip. */
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 7), 0);
	zassert_equal(mg_queue_append(a, 60), 0);
	zassert_equal(mg_queue_end(), 0);
	zassert_equal(mg_queue_clip_info(2, &ci), 0);
	zassert_equal(ci.bytes, 60);
	zassert_equal(mg_queue_read(0, 0, rd, sizeof(a)), 0);
	zassert_mem_equal(rd, a, sizeof(a), "earlier clip intact");
}

ZTEST(queue, test_empty_clip)
{
	struct mg_clip_info ci;

	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 1), 0);
	zassert_equal(mg_queue_end(), 0);
	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.bytes, 0);
}

static bool rewritable(void)
{
	const struct flash_area *fa;

	zassert_equal(flash_area_open(QUEUE_PART, &fa), 0);
	return flash_get_parameters(flash_area_get_device(fa))->caps.no_explicit_erase;
}

ZTEST(queue, test_recovers_unfinished_clip)
{
	static uint8_t a[5000], rd[5000];
	struct mg_clip_info ci;
	struct mg_queue_status st;

	fill(a, sizeof(a), 3);
	/* A reset mid-recording: the clip never closes. On flash everything up
	 * to the first erased block survives (19 whole 256-byte buffers); on
	 * RRAM the length saved every 16 buffers does. */
	uint32_t want = rewritable() ? 16 * 256 : 19 * 256;

	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 42), 0);
	zassert_equal(mg_queue_append(a, sizeof(a)), 0);
	zassert_equal(mg_queue_init(), 0); /* "reboot" without mg_queue_end() */

	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 1);
	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.start_ms, 42);
	zassert_equal(ci.bytes, want, "recovered %u", ci.bytes);
	zassert_equal(mg_queue_read(0, 0, rd, ci.bytes), 0);
	zassert_mem_equal(rd, a, ci.bytes);

	/* The recovered length was committed: a second scan agrees, and the
	 * next clip goes after it. */
	zassert_equal(mg_queue_init(), 0);
	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.bytes, want);
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 43), 0);
	zassert_equal(mg_queue_append(a, 100), 0);
	zassert_equal(mg_queue_end(), 0);
	zassert_equal(mg_queue_init(), 0);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 2);
}

ZTEST(queue, test_clear_forgets_old_clips)
{
	static uint8_t a[3000];
	struct mg_queue_status st;
	struct mg_clip_info ci;

	fill(a, sizeof(a), 9);
	for (int i = 0; i < 3; i++) {
		zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, i), 0);
		zassert_equal(mg_queue_append(a, sizeof(a)), 0);
		zassert_equal(mg_queue_end(), 0);
	}
	zassert_equal(mg_queue_clear(), 0);
	/* One short clip over the old ones: a rescan must not find the stale
	 * clips behind it. */
	zassert_equal(mg_queue_begin(mg_data_type_audio_lc3, 99), 0);
	zassert_equal(mg_queue_append(a, 80), 0);
	zassert_equal(mg_queue_end(), 0);
	zassert_equal(mg_queue_init(), 0);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 1);
	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.bytes, 80);
	zassert_equal(ci.codec, mg_data_type_audio_lc3);
	/* And an empty queue after clear stays empty across a reboot. */
	zassert_equal(mg_queue_clear(), 0);
	zassert_equal(mg_queue_init(), 0);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 0);
}

ZTEST(queue, test_full)
{
	static uint8_t frame[240];
	struct mg_queue_status st;
	struct mg_clip_info ci;
	int err = 0;
	uint32_t appended = 0;

	fill(frame, sizeof(frame), 5);
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 0), 0);
	while ((err = mg_queue_append(frame, sizeof(frame))) == 0) {
		appended += sizeof(frame);
	}
	zassert_equal(err, -ENOSPC);
	zassert_equal(mg_queue_append(frame, 1), -ENOSPC, "stays full");
	zassert_equal(mg_queue_end(), 0);
	mg_queue_get_status(&st);
	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.bytes, appended, "whole frames only");
	zassert_true(st.used_bytes <= st.capacity_bytes);
	/* What's left is less than one more frame. */
	if (mg_queue_begin(mg_data_type_audio_sbc, 0) == 0) {
		zassert_equal(mg_queue_append(frame, sizeof(frame)), -ENOSPC);
		zassert_equal(mg_queue_end(), 0);
	}

	zassert_equal(mg_queue_clear(), 0);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 0);
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 0), 0, "room again");
	zassert_equal(mg_queue_end(), 0);
}

ZTEST(queue, test_clear_while_recording)
{
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 0), 0);
	zassert_equal(mg_queue_clear(), -EBUSY);
	zassert_equal(mg_queue_end(), 0);
}

#if defined(CONFIG_MG_QUEUE_IN_SLOT1)

/* MCUboot's image header magic, and what it reads at the end of a slot. */
#define IMAGE_MAGIC 0x96f3b83dU

static const struct flash_area *slot(void)
{
	const struct flash_area *fa;

	zassert_equal(flash_area_open(QUEUE_PART, &fa), 0);
	return fa;
}

static bool region_blank(const struct flash_area *fa, uint32_t off, uint32_t len)
{
	uint8_t b[256];
	uint8_t e = flash_area_erased_val(fa);

	for (uint32_t done = 0; done < len; done += sizeof(b)) {
		uint32_t n = MIN(sizeof(b), len - done);

		zassert_equal(flash_area_read(fa, off + done, b, n), 0);
		for (uint32_t i = 0; i < n; i++) {
			if (b[i] != e) {
				return false;
			}
		}
	}
	return true;
}

static uint32_t word_at(const struct flash_area *fa, uint32_t off)
{
	uint8_t b[4];

	zassert_equal(flash_area_read(fa, off, b, 4), 0);
	return sys_get_le32(b);
}

ZTEST(queue, test_slot_policy)
{
	/* (upload_active, upgrade_pending, running_confirmed) */
	zassert_false(mg_queue_slot_needed_by_ota(false, false, true), "nothing for MCUboot");
	zassert_true(mg_queue_slot_needed_by_ota(true, false, true), "an upload is running");
	zassert_true(mg_queue_slot_needed_by_ota(false, true, true), "test/permanent/revert pending");
	zassert_true(mg_queue_slot_needed_by_ota(false, false, false), "running image on test");
	zassert_true(mg_queue_slot_needed_by_ota(true, true, false));
}

/* Filled to the brim, the queue never writes the image magic at offset 0
 * nor anything in the trailer MCUboot reads. */
ZTEST(queue, test_slot_never_looks_like_an_image)
{
	static uint8_t frame[240];
	const struct flash_area *fa = slot();
	struct mg_queue_status st;

	zassert_equal(mg_queue_reclaim(), 0);
	zassert_true(region_blank(fa, fa->fa_size - QTRAILER, QTRAILER));
	mg_queue_get_status(&st);
	zassert_equal(st.capacity_bytes, fa->fa_size - QTRAILER);
	fill(frame, sizeof(frame), 0x3d); /* bytes of the magic, for good measure */
	for (int clip = 0; clip < 3; clip++) {
		zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, clip), 0);
		zassert_not_equal(word_at(fa, 0), IMAGE_MAGIC);
		while (mg_queue_append(frame, sizeof(frame)) == 0) {
		}
		zassert_equal(mg_queue_end(), 0);
		zassert_equal(word_at(fa, 0), 0x3151474dU, "a clip header (MGQ1) at 0");
	}
	mg_queue_get_status(&st);
	zassert_true(st.used_bytes > st.capacity_bytes - 2 * sizeof(frame));
	zassert_true(region_blank(fa, fa->fa_size - QTRAILER, QTRAILER), "trailer untouched");
	/* Clearing (instant on RRAM) leaves blank cells at 0, never the magic. */
	zassert_equal(mg_queue_clear(), 0);
	zassert_not_equal(word_at(fa, 0), IMAGE_MAGIC);
	zassert_true(region_blank(fa, fa->fa_size - QTRAILER, QTRAILER));
	flash_area_close(fa);
}

/* The slot changes hands: clips -> OTA -> (reboot with an image) -> reclaimed. */
ZTEST(queue, test_slot_ownership)
{
	static uint8_t a[2000], img[512];
	const struct flash_area *fa = slot();
	struct mg_queue_status st;
	struct mg_clip_info ci;

	zassert_equal(mg_queue_reclaim(), 0);
	fill(a, sizeof(a), 7);
	for (int i = 0; i < 2; i++) {
		zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, i), 0);
		zassert_equal(mg_queue_append(a, sizeof(a)), 0);
		zassert_equal(mg_queue_end(), 0);
	}
	/* A third clip is recording when the upload starts. */
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 9), 0);
	zassert_equal(mg_queue_append(a, 100), 0);
	zassert_equal(mg_queue_hand_to_ota(), 3, "two clips and the open one dropped");
	zassert_false(mg_queue_ready());
	zassert_true(mg_queue_present());
	zassert_false(mg_queue_recording());
	zassert_equal(strcmp(mg_queue_owner(), "OTA"), 0);
	zassert_equal(mg_queue_append(a, 100), -EINVAL, "recording stopped");
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 1), -EBUSY);
	zassert_equal(mg_queue_clear(), -EBUSY);
	zassert_equal(mg_queue_clip_info(0, &ci), -ENOENT);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count + st.used_bytes + st.capacity_bytes, 0, "shown as unavailable");
	zassert_equal(mg_queue_hand_to_ota(), 0, "nothing more to drop");

	/* img_mgmt writes an image, and MCUboot's pending-upgrade magic. */
	memset(img, 0xa5, sizeof(img));
	sys_put_le32(IMAGE_MAGIC, img);
	zassert_equal(flash_area_write(fa, 0, img, sizeof(img)), 0);
	memset(img, 0x77, 16);
	zassert_equal(flash_area_write(fa, fa->fa_size - 16, img, 16), 0);

	/* Reboot: the scan finds an image, not clips, and leaves it alone. */
	zassert_equal(mg_queue_init(), 0);
	zassert_false(mg_queue_ready());
	zassert_equal(strcmp(mg_queue_owner(), "other data"), 0);
	zassert_equal(word_at(fa, 0), IMAGE_MAGIC, "untouched while MCUboot may need it");
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 1), -EBUSY);

	/* MCUboot is done (no swap pending, image confirmed): reclaim. */
	zassert_false(mg_queue_slot_needed_by_ota(false, false, true));
	zassert_equal(mg_queue_reclaim(), 0);
	zassert_true(mg_queue_ready());
	zassert_not_equal(word_at(fa, 0), IMAGE_MAGIC);
	zassert_true(region_blank(fa, fa->fa_size - QTRAILER, QTRAILER), "trailer blanked");
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 0);
	zassert_equal(st.capacity_bytes, fa->fa_size - QTRAILER);

	/* Clips again, kept across a reboot. */
	zassert_equal(mg_queue_begin(mg_data_type_audio_lc3, 77), 0);
	zassert_equal(mg_queue_append(a, 400), 0);
	zassert_equal(mg_queue_end(), 0);
	zassert_equal(mg_queue_init(), 0);
	zassert_true(mg_queue_ready());
	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.bytes, 400);
	zassert_equal(ci.start_ms, 77);
	flash_area_close(fa);
}

#endif

ZTEST_SUITE(queue, NULL, setup, before, NULL, NULL);
