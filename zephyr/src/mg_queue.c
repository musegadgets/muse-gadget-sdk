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

#include "mg_queue.h"

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(mg_queue, CONFIG_MG_LOG_LEVEL);

#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
/* MCUboot's secondary slot, shared with OTA (mg_queue.h). */
#define QUEUE_ID PARTITION_ID(slot1_partition)
#define TRAILER  CONFIG_MG_QUEUE_SLOT1_TRAILER
#else
#define QUEUE_ID PARTITION_ID(audio_queue_partition)
#define TRAILER  0
#endif

#define MAGIC_OPEN   0x3151474dU /* "MGQ1" */
#define MAGIC_COMMIT 0x4351474dU /* "MGQC" */
#define MAGIC_PARTIAL 0x5051474dU /* "MGQP": length so far (overwritable media) */
#define PARTIAL_EVERY 16 /* write-buffer flushes between partial length updates */
#define HALF         16U
#define HDR          (2 * HALF)
#define WBUF         256U

struct clip {
	uint32_t hdr; /* offset of the header */
	uint32_t bytes;
	uint32_t start_ms;
	uint8_t codec;
};

static const struct flash_area *fa;
static uint32_t cap;     /* bytes the queue may use: the partition less the slot trailer */
static bool ota_owned;   /* OTA has the slot: the queue is unavailable */
static bool foreign;     /* the slot holds something else (an image) until reclaimed */
static K_MUTEX_DEFINE(lock);
static struct clip clips[CONFIG_MG_AUDIO_QUEUE_MAX_CLIPS];
static uint16_t nclips;
static uint32_t tail;  /* next free, aligned offset */
static uint32_t align; /* write alignment, at least HALF */
static uint8_t erased;
/* RRAM and the like: rewritable in place, no erase needed. */
static bool overwrite;

/* Recording state. */
static bool rec;
static bool rec_full;
static struct clip cur;
static uint32_t cur_crc;
static uint8_t wbuf[WBUF];
static size_t wlen;
static uint32_t wpos; /* where wbuf goes */
static uint32_t flushes;

static bool all_erased(const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (b[i] != erased) {
			return false;
		}
	}
	return true;
}

static int write_half2(uint32_t hdr, uint32_t bytes, uint32_t crc, uint32_t magic)
{
	uint8_t h[HALF];

	memset(h, 0, sizeof(h));
	sys_put_le32(bytes, &h[0]);
	sys_put_le32(crc, &h[4]);
	sys_put_le32(magic, &h[8]);
	return flash_area_write(fa, hdr + HALF, h, sizeof(h));
}

/* On overwritable media nothing past the last clip is erased, so the
 * header slot after it is blanked to end the scan there. */
static void blank_next_header(uint32_t off)
{
	if (overwrite && off + HDR <= cap) {
		(void)flash_area_flatten(fa, off, HDR);
	}
}

/* A clip whose second half never got written: its data runs up to the first
 * fully erased alignment block. */
static uint32_t recover_length(uint32_t data)
{
	uint8_t b[64];
	uint32_t off = data;
	size_t step = MIN((size_t)align, sizeof(b));
	while (off + step <= cap) {
		if (flash_area_read(fa, off, b, step) || all_erased(b, step)) {
			break;
		}
		off += step;
	}
	return off - data;
}

static int scan(void)
{
	uint8_t h[HDR];
	uint32_t off = 0;

	nclips = 0;
	while (off + HDR <= cap) {
		if (flash_area_read(fa, off, h, sizeof(h))) {
			return -EIO;
		}
		if (all_erased(h, HALF)) {
			break;
		}
		if (sys_get_le32(&h[0]) != MAGIC_OPEN) {
			LOG_WRN("queue: bad header at 0x%x, ignoring the rest", off);
			break;
		}
		struct clip c = {
			.hdr = off,
			.codec = h[4],
			.start_ms = sys_get_le32(&h[8]),
		};

		uint32_t magic = sys_get_le32(&h[HALF + 8]);

		if (magic == MAGIC_COMMIT) {
			c.bytes = sys_get_le32(&h[HALF]);
		} else if (magic == MAGIC_PARTIAL || all_erased(&h[HALF], HALF)) {
			/* Interrupted by a reset: the length saved so far on RRAM, or
			 * up to the first erased block on flash. */
			c.bytes = magic == MAGIC_PARTIAL ? sys_get_le32(&h[HALF])
							 : recover_length(off + HDR);
			LOG_WRN("queue: recovered unfinished clip at 0x%x (%u bytes)", off, c.bytes);
			(void)write_half2(off, c.bytes, 0, MAGIC_COMMIT);
			blank_next_header(ROUND_UP(off + HDR + c.bytes, align));
		} else {
			LOG_WRN("queue: bad header at 0x%x, ignoring the rest", off);
			break;
		}
		if (c.bytes > cap - off - HDR) {
			LOG_WRN("queue: clip at 0x%x too long, ignoring the rest", off);
			break;
		}
		if (nclips == ARRAY_SIZE(clips)) {
			break;
		}
		clips[nclips++] = c;
		off = ROUND_UP(off + HDR + c.bytes, align);
	}
	tail = off;
	return 0;
}

int mg_queue_init(void)
{
	int err = flash_area_open(QUEUE_ID, &fa);

	if (err) {
		LOG_ERR("queue: no partition (%d)", err);
		fa = NULL;
		return err;
	}
	const struct device *dev = flash_area_get_device(fa);
	const struct flash_parameters *fp = flash_get_parameters(dev);

	erased = fp->erase_value;
	overwrite = fp->caps.no_explicit_erase;
	cap = fa->fa_size > TRAILER + 4 * HDR ? fa->fa_size - TRAILER : 0;
	if (!cap) {
		LOG_ERR("queue: partition too small");
		fa = NULL;
		return -ENOSPC;
	}
	align = MAX(HALF, flash_get_write_block_size(dev));
	if (align > HALF * 4 || (WBUF % align) != 0) {
		LOG_ERR("queue: write block %u unsupported", align);
		fa = NULL;
		return -ENOTSUP;
	}
	k_mutex_lock(&lock, K_FOREVER);
	rec = false;
	ota_owned = false;
	foreign = false;
	err = scan();
	if (IS_ENABLED(CONFIG_MG_QUEUE_IN_SLOT1) && nclips == 0) {
		/* Not a queue and not blank: an image, or what was there before. */
		uint8_t h[HALF];

		foreign = flash_area_read(fa, 0, h, sizeof(h)) != 0 ||
			  (!all_erased(h, sizeof(h)) && sys_get_le32(&h[0]) != MAGIC_OPEN);
	}
	k_mutex_unlock(&lock);
	if (foreign) {
		LOG_INF("queue: the slot holds something else; it waits to be reclaimed");
	} else {
		LOG_INF("queue: %u clips, %u of %u bytes used", nclips, tail, cap);
	}
	return err;
}

bool mg_queue_present(void)
{
	return fa != NULL;
}

bool mg_queue_ready(void)
{
	return fa != NULL && !ota_owned && !foreign;
}

const char *mg_queue_owner(void)
{
	return !fa ? "none" : ota_owned ? "OTA" : foreign ? "other data" : "clips";
}

bool mg_queue_slot_needed_by_ota(bool upload_active, bool upgrade_pending, bool running_confirmed)
{
	return upload_active || upgrade_pending || !running_confirmed;
}

int mg_queue_hand_to_ota(void)
{
	if (!fa) {
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	uint32_t clips_lost = nclips + (rec ? 1 : 0);
	uint32_t bytes_lost = rec ? cur.hdr + HDR + cur.bytes : tail;

	if (!ota_owned && clips_lost) {
		LOG_WRN("queue: firmware update takes the clip store: %u clip(s), %u bytes "
			"dropped (download clips before updating)",
			clips_lost, bytes_lost);
	} else if (!ota_owned) {
		LOG_INF("queue: firmware update takes the clip store (it was empty)");
	}
	rec = false;
	nclips = 0;
	tail = 0;
	ota_owned = true;
	foreign = false;
	k_mutex_unlock(&lock);
	return (int)clips_lost;
}

int mg_queue_reclaim(void)
{
	int err;

	if (!fa) {
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (overwrite) {
		/* RRAM rewrites in place, so only what MCUboot reads needs blanking:
		 * the image header at the start and the trailer at the end. The old
		 * image's bytes in between are inert and get overwritten by clips. */
		err = flash_area_flatten(fa, 0, ROUND_UP(HDR, align));
		if (!err && fa->fa_size > cap) {
			err = flash_area_flatten(fa, cap, fa->fa_size - cap);
		}
	} else {
		/* Flash needs erased cells under every write: the whole slot. */
		err = flash_area_flatten(fa, 0, fa->fa_size);
	}
	if (!err) {
		nclips = 0;
		tail = 0;
		rec = false;
		ota_owned = false;
		foreign = false;
	}
	k_mutex_unlock(&lock);
	LOG_INF("queue: clip store reclaimed from firmware update (%d), %u bytes free", err, cap);
	return err;
}

void mg_queue_get_status(struct mg_queue_status *st)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool ok = fa && !ota_owned && !foreign;

	/* Held by a firmware update: nothing stored, no room. */
	st->used_bytes = ok ? tail : 0;
	st->capacity_bytes = ok ? cap : 0;
	st->clip_count = nclips;
	k_mutex_unlock(&lock);
}

int mg_queue_clip_info(uint16_t index, struct mg_clip_info *ci)
{
	int err = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (index >= nclips) {
		err = -ENOENT;
	} else {
		ci->codec = clips[index].codec;
		ci->start_ms = clips[index].start_ms;
		ci->bytes = clips[index].bytes;
		ci->offset = clips[index].hdr + HDR;
	}
	k_mutex_unlock(&lock);
	return err;
}

int mg_queue_read(uint16_t index, uint32_t off, void *buf, size_t len)
{
	struct mg_clip_info ci;
	int err = mg_queue_clip_info(index, &ci);

	if (err) {
		return err;
	}
	if (off > ci.bytes || len > ci.bytes - off) {
		return -EINVAL;
	}
	return flash_area_read(fa, ci.offset + off, buf, len);
}

bool mg_queue_recording(void)
{
	return rec;
}

int mg_queue_begin(uint8_t codec, uint32_t start_ms)
{
	uint8_t h[HALF];
	int err;

	if (!fa) {
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (rec || ota_owned || foreign) {
		err = -EBUSY;
		goto out;
	}
	if (nclips == ARRAY_SIZE(clips) || tail + HDR + align > cap) {
		err = -ENOSPC;
		goto out;
	}
	memset(h, 0, sizeof(h));
	sys_put_le32(MAGIC_OPEN, &h[0]);
	h[4] = codec;
	sys_put_le32(start_ms, &h[8]);
	err = flash_area_write(fa, tail, h, sizeof(h));
	if (!err && overwrite) {
		/* The second half holds whatever was there: mark it partial. */
		err = write_half2(tail, 0, 0, MAGIC_PARTIAL);
	}
	if (err) {
		goto out;
	}
	cur = (struct clip){.hdr = tail, .codec = codec, .start_ms = start_ms};
	cur_crc = 0;
	wlen = 0;
	wpos = tail + HDR;
	flushes = 0;
	rec = true;
	rec_full = false;
out:
	k_mutex_unlock(&lock);
	return err;
}

static int flush(bool final)
{
	size_t n = final ? ROUND_UP(wlen, align) : wlen;

	if (n == 0) {
		return 0;
	}
	memset(&wbuf[wlen], erased, n - wlen);
	int err = flash_area_write(fa, wpos, wbuf, n);

	wpos += n;
	wlen = 0;
	/* On RRAM, keep the length saved so a reset loses at most a moment. */
	if (!err && !final && overwrite && ++flushes % PARTIAL_EVERY == 0) {
		err = write_half2(cur.hdr, wpos - cur.hdr - HDR, 0, MAGIC_PARTIAL);
	}
	return err;
}

int mg_queue_append(const void *data, size_t len)
{
	const uint8_t *p = data;
	int err = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (!rec) {
		err = -EINVAL;
		goto out;
	}
	/* Whole frames only: a frame that doesn't fit ends the clip's growth. */
	if (rec_full || cur.hdr + HDR + cur.bytes + len > cap) {
		rec_full = true;
		err = -ENOSPC;
		goto out;
	}
	cur_crc = crc32_ieee_update(cur_crc, p, len);
	cur.bytes += len;
	while (len) {
		size_t n = MIN(len, WBUF - wlen);

		memcpy(&wbuf[wlen], p, n);
		wlen += n;
		p += n;
		len -= n;
		if (wlen == WBUF) {
			err = flush(false);
			if (err) {
				goto out;
			}
		}
	}
out:
	k_mutex_unlock(&lock);
	return err;
}

int mg_queue_end(void)
{
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	if (!rec) {
		k_mutex_unlock(&lock);
		return -EINVAL;
	}
	err = flush(true);
	if (!err) {
		err = write_half2(cur.hdr, cur.bytes, cur_crc, MAGIC_COMMIT);
	}
	if (!err) {
		clips[nclips++] = cur;
		tail = ROUND_UP(cur.hdr + HDR + cur.bytes, align);
		blank_next_header(tail);
		LOG_INF("queue: clip %u saved, %u bytes", nclips - 1, cur.bytes);
	}
	rec = false;
	k_mutex_unlock(&lock);
	return err;
}

int mg_queue_clear(void)
{
	int err;

	if (!fa) {
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (rec || ota_owned || foreign) {
		err = -EBUSY;
		goto out;
	}
	uint32_t end = HDR;

	if (!overwrite) {
		/* Flash: erase the pages that were used, so every later write
		 * lands on erased cells. */
		const struct device *dev = flash_area_get_device(fa);
		struct flash_pages_info pi;

		end = tail;
		if (flash_get_page_info_by_offs(dev, fa->fa_off + MAX(end, 1) - 1, &pi) == 0) {
			end = pi.start_offset + pi.size - fa->fa_off;
		}
		end = MIN(ROUND_UP(MAX(end, HDR), align), cap);
	}
	/* RRAM rewrites in place: blanking the first header empties the queue
	 * at once (erasing 700 KB would take minutes while the radio runs). */
	err = flash_area_flatten(fa, 0, end);
	if (!err) {
		nclips = 0;
		tail = 0;
	}
	LOG_INF("queue: cleared (%d)", err);
out:
	k_mutex_unlock(&lock);
	return err;
}
