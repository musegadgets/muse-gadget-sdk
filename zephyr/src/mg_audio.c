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

#include "mg_audio.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mg_core.h"
#include "mg_hpf.h"
#include "mg_led.h"
#include "mg_session.h"
#include "mg_settings.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_AUDIO_QUEUE)
#include "mg_queue.h"
#endif

LOG_MODULE_REGISTER(mg_audio, CONFIG_MG_LOG_LEVEL);

#define STACK_SIZE 6144
#define MAX_CHUNK  244

struct request {
	enum mg_audio_activity want;
	uint8_t codec;
	bool ptt;
	uint16_t clip;
	uint32_t gen;
};

static K_MUTEX_DEFINE(lock);
static K_SEM_DEFINE(wake, 0, 1);
static struct request req;
static struct mg_audio_format cur_fmt;
static atomic_t gain_q8;

static int16_t block[MG_AUDIO_BLOCK_SAMPLES];
static int16_t acc[MG_AUDIO_BLOCK_SAMPLES + MG_CODEC_MAX_FRAME_SAMPLES];
static uint8_t frame[MG_CODEC_MAX_FRAME_BYTES];
static uint8_t chunk[MAX_CHUNK];

static struct k_work idle_work;

static void idle_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	mg_core_audio_idle();
}

static uint32_t submit(enum mg_audio_activity want, uint8_t codec, bool ptt, uint16_t clip)
{
	uint32_t gen;

	k_mutex_lock(&lock, K_FOREVER);
	req.want = want;
	req.codec = codec;
	req.ptt = ptt;
	req.clip = clip;
	gen = ++req.gen;
	k_mutex_unlock(&lock);
	k_sem_give(&wake);
	return gen;
}

static bool current(uint32_t gen)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool same = req.gen == gen;

	k_mutex_unlock(&lock);
	return same;
}

int mg_audio_start_live(uint8_t codec, bool ptt)
{
	struct mg_audio_format fmt;

	if (mg_codec_format(codec, &fmt)) {
		return -ENOTSUP;
	}
	submit(MG_AUDIO_LIVE, codec, ptt, 0);
	return 0;
}

int mg_audio_start_recording(uint8_t codec)
{
	struct mg_audio_format fmt;

	if (!IS_ENABLED(CONFIG_MG_AUDIO_QUEUE) || mg_codec_format(codec, &fmt)) {
		return -ENOTSUP;
	}
	submit(MG_AUDIO_RECORDING, codec, false, 0);
	return 0;
}

int mg_audio_read_clip(uint16_t index)
{
	if (!IS_ENABLED(CONFIG_MG_AUDIO_QUEUE)) {
		return -ENOTSUP;
	}
	submit(MG_AUDIO_CLIP, 0, false, index);
	return 0;
}

void mg_audio_stop(void)
{
	submit(MG_AUDIO_IDLE, 0, false, 0);
}

enum mg_audio_activity mg_audio_activity(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	enum mg_audio_activity a = req.want;

	k_mutex_unlock(&lock);
	return a;
}

void mg_audio_current_format(struct mg_audio_format *fmt)
{
	k_mutex_lock(&lock, K_FOREVER);
	*fmt = cur_fmt;
	k_mutex_unlock(&lock);
	if (fmt->type == mg_data_type_unknown) {
		(void)mg_codec_format(mg_settings_get()->audio_codec, fmt);
	}
}

void mg_audio_set_gain(uint8_t gain)
{
	/* gain / 12.5 in Q8. */
	atomic_set(&gain_q8, (atomic_val_t)gain * 2048 / 100);
}

static struct mg_hpf hpf;

static void apply_gain(int16_t *pcm, size_t n)
{
	int32_t g = (int32_t)atomic_get(&gain_q8);

	for (size_t i = 0; i < n; i++) {
		int32_t v = (pcm[i] * g) >> 8;

		pcm[i] = (int16_t)CLAMP(v, INT16_MIN, INT16_MAX);
	}
}

static void announce(const struct mg_audio_format *fmt)
{
	uint8_t buf[9];
	size_t n = mg_codec_change_data_type(fmt, buf);

	k_mutex_lock(&lock, K_FOREVER);
	cur_fmt = *fmt;
	k_mutex_unlock(&lock);
	(void)mg_session_send_control(buf, n);
}

static size_t frames_per_chunk(const struct mg_audio_format *fmt)
{
	size_t room = MIN(mg_session_data_room(), sizeof(chunk));
	size_t by_time = MAX(1, CONFIG_MG_AUDIO_MAX_PACKET_MS * 1000 / fmt->frame_us);

	return MIN(room / fmt->frame_bytes, by_time);
}

static void run_capture(const struct request *r)
{
	struct mg_audio_format fmt;
	size_t nacc = 0, clen = 0, nframes = 0, fpc = 0;
	uint32_t sent = 0, dropped = 0, stored = 0;
	bool live = r->want == MG_AUDIO_LIVE;
	bool full = false;

	if (mg_codec_format(r->codec, &fmt) || mg_codec_open(r->codec)) {
		LOG_ERR("codec %u unavailable", r->codec);
		return;
	}
	if (live) {
		announce(&fmt);
		mg_led_set(MG_LED_STREAMING, true);
	} else {
#if defined(CONFIG_MG_AUDIO_QUEUE)
		int err = mg_queue_begin(r->codec, k_uptime_get_32());

		if (err) {
			LOG_WRN("queue: can't record (%d)", err);
			return;
		}
#endif
		mg_led_set(MG_LED_RECORDING, true);
	}
	if (mg_audio_src_start()) {
		LOG_ERR("audio source failed to start");
		goto out;
	}
	LOG_INF("%s capture, codec %u", live ? "live" : "offline", r->codec);
	/* The first block primes the DC blocker: no settling step at the start. */
	mg_hpf_reset(&hpf);

	while (current(r->gen)) {
		if (mg_audio_src_read(block, 200)) {
			continue;
		}
		mg_hpf_run(&hpf, block, MG_AUDIO_BLOCK_SAMPLES);
		apply_gain(block, MG_AUDIO_BLOCK_SAMPLES);
		memcpy(&acc[nacc], block, sizeof(block));
		nacc += MG_AUDIO_BLOCK_SAMPLES;

		while (nacc >= fmt.frame_samples) {
			int n = mg_codec_encode(acc, frame, sizeof(frame));

			nacc -= fmt.frame_samples;
			memmove(acc, &acc[fmt.frame_samples], nacc * sizeof(acc[0]));
			if (n <= 0) {
				continue;
			}
			if (!live) {
#if defined(CONFIG_MG_AUDIO_QUEUE)
				if (!full && mg_queue_append(frame, n) == -ENOSPC) {
					LOG_WRN("queue full");
					full = true;
				}
				stored += full ? 0 : n;
#endif
				continue;
			}
			if (nframes == 0) {
				fpc = frames_per_chunk(&fmt);
				if (fpc == 0) {
					dropped++;
					continue; /* MTU too small for one frame */
				}
			}
			memcpy(&chunk[clen], frame, n);
			clen += n;
			if (++nframes == fpc) {
				if (mg_session_send_data(chunk, clen, K_NO_WAIT) == 0) {
					sent++;
				} else {
					dropped++;
				}
				clen = 0;
				nframes = 0;
			}
		}
	}
	mg_audio_src_stop();
	if (live && clen) {
		if (mg_session_send_data(chunk, clen, K_MSEC(100)) == 0) {
			sent++;
		} else {
			dropped++;
		}
	}
	LOG_INF("capture ended: %u chunks sent, %u dropped, %u bytes stored", sent, dropped,
		stored);
out:
	if (live) {
		static const uint8_t stop[] = {mg_command_stop_mic};

		/* stop_mic ends every capture, whichever side started it. */
		mg_led_set(MG_LED_STREAMING, false);
		(void)mg_session_send_control(stop, sizeof(stop));
	} else {
#if defined(CONFIG_MG_AUDIO_QUEUE)
		(void)mg_queue_end();
#endif
		mg_led_set(MG_LED_RECORDING, false);
	}
}

#if defined(CONFIG_MG_AUDIO_QUEUE)
static void run_clip(const struct request *r)
{
	struct mg_clip_info ci;
	struct mg_audio_format fmt = {.type = mg_data_type_audio_queue};
	uint32_t off = 0;

	if (mg_queue_clip_info(r->clip, &ci)) {
		return;
	}
	announce(&fmt);
	LOG_INF("sending clip %u (%u bytes)", r->clip, ci.bytes);
	int64_t t0 = k_uptime_get();
	uint32_t chunks = 0;

	while (off < ci.bytes && current(r->gen)) {
		size_t n = MIN(MIN(mg_session_data_room(), sizeof(chunk)), ci.bytes - off);

		if (n == 0 || mg_queue_read(r->clip, off, chunk, n)) {
			break;
		}
		int err = mg_session_send_data(chunk, n, K_MSEC(200));

		if (err == -ENOMEM) {
			continue; /* congested: retry the same chunk */
		}
		if (err) {
			LOG_WRN("clip download stopped (%d)", err);
			break;
		}
		off += n;
		chunks++;
	}
	uint32_t ms = MAX((uint32_t)(k_uptime_get() - t0), 1U);

	/* Queued, not on the air yet: the notification queue holds most of a clip, so
	 * the bench client times the download itself. */
	LOG_INF("clip %u: %u of %u bytes queued in %u ms (%u notifications)", r->clip, off,
		ci.bytes, ms, chunks);
}
#endif

static void audio_thread(void *a, void *b, void *c)
{
	struct request r;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&wake, K_FOREVER);
		for (;;) {
			k_mutex_lock(&lock, K_FOREVER);
			r = req;
			k_mutex_unlock(&lock);

			switch (r.want) {
			case MG_AUDIO_LIVE:
			case MG_AUDIO_RECORDING:
				run_capture(&r);
				break;
#if defined(CONFIG_MG_AUDIO_QUEUE)
			case MG_AUDIO_CLIP:
				run_clip(&r);
				break;
#endif
			default:
				break;
			}
			/* Finished on its own, or stopped: go idle unless something
			 * new arrived meanwhile. */
			k_mutex_lock(&lock, K_FOREVER);
			bool again = req.gen != r.gen && req.want != MG_AUDIO_IDLE;

			if (!again) {
				req.want = MG_AUDIO_IDLE;
			}
			k_mutex_unlock(&lock);
			if (!again) {
				break;
			}
		}
		k_work_submit_to_queue(mg_app_wq(), &idle_work);
	}
}

K_THREAD_STACK_DEFINE(audio_stack, STACK_SIZE);
static struct k_thread audio_tid;

void mg_audio_init(void)
{
	k_work_init(&idle_work, idle_fn);
	mg_hpf_init(&hpf, CONFIG_MG_MIC_HPF_HZ, 16000);
	k_thread_create(&audio_tid, audio_stack, K_THREAD_STACK_SIZEOF(audio_stack), audio_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
	k_thread_name_set(&audio_tid, "mg_audio");
}
