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

#include "mg_codec.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <sbc.h>
#if defined(CONFIG_MG_LC3)
#include <lc3.h>
#endif

/* SBC per mgcommands.h: 16 kHz mono, 16 blocks, 8 subbands, loudness. */
static const struct sbc_frame sbc_cfg = {
	.msbc = false,
	.freq = SBC_FREQ_16K,
	.mode = SBC_MODE_MONO,
	.bam = SBC_BAM_LOUDNESS,
	.nblocks = 16,
	.nsubbands = 8,
	.bitpool = CONFIG_MG_SBC_BITPOOL,
};

static uint8_t cur_type;
static sbc_t sbc;
#if defined(CONFIG_MG_LC3)
static LC3_ENCODER_MEM_T(10000, 16000) lc3_mem;
static lc3_encoder_t lc3_enc;
#endif

int mg_codec_format(uint8_t type, struct mg_audio_format *fmt)
{
	memset(fmt, 0, sizeof(*fmt));
	fmt->type = type;
	fmt->sample_rate = MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ;
	fmt->channels = MG_AUDIO_DEFAULT_CHANNELS;
	switch (type) {
	case mg_data_type_audio_sbc:
		fmt->frame_samples = sbc_cfg.nblocks * sbc_cfg.nsubbands;
		fmt->frame_bytes = sbc_get_frame_size(&sbc_cfg);
		fmt->frame_us = fmt->frame_samples * 1000000U / fmt->sample_rate;
		return 0;
#if defined(CONFIG_MG_LC3)
	case mg_data_type_audio_lc3:
		fmt->frame_samples = 160;
		fmt->frame_bytes = CONFIG_MG_LC3_FRAME_BYTES;
		fmt->frame_us = MG_LC3_DEFAULT_FRAME_US;
		return 0;
#endif
	default:
		return -ENOTSUP;
	}
}

int mg_codec_open(uint8_t type)
{
	switch (type) {
	case mg_data_type_audio_sbc:
		sbc_reset(&sbc);
		break;
#if defined(CONFIG_MG_LC3)
	case mg_data_type_audio_lc3:
		lc3_enc = lc3_setup_encoder(MG_LC3_DEFAULT_FRAME_US, 16000, 0, &lc3_mem);
		if (lc3_enc == NULL) {
			return -EIO;
		}
		break;
#endif
	default:
		return -ENOTSUP;
	}
	cur_type = type;
	return 0;
}

int mg_codec_encode(const int16_t *pcm, uint8_t *out, size_t room)
{
	switch (cur_type) {
	case mg_data_type_audio_sbc: {
		unsigned int n = sbc_get_frame_size(&sbc_cfg);

		if (room < n) {
			return -ENOMEM;
		}
		if (sbc_encode(&sbc, pcm, 1, NULL, 0, &sbc_cfg, out, n) < 0) {
			return -EIO;
		}
		return n;
	}
#if defined(CONFIG_MG_LC3)
	case mg_data_type_audio_lc3:
		if (room < CONFIG_MG_LC3_FRAME_BYTES) {
			return -ENOMEM;
		}
		if (lc3_encode(lc3_enc, LC3_PCM_FORMAT_S16, pcm, 1, CONFIG_MG_LC3_FRAME_BYTES, out) < 0) {
			return -EIO;
		}
		return CONFIG_MG_LC3_FRAME_BYTES;
#endif
	default:
		return -EINVAL;
	}
}

size_t mg_codec_change_data_type(const struct mg_audio_format *fmt, uint8_t *out)
{
	out[0] = mg_command_change_data_type;
	out[1] = fmt->type;
	if (fmt->sample_rate == 0) {
		return 2; /* non-audio types carry no format */
	}
	sys_put_le16(fmt->sample_rate, &out[2]);
	out[4] = fmt->channels;
	sys_put_le16(fmt->frame_bytes, &out[5]);
	sys_put_le16(fmt->frame_us, &out[7]);
	return 9;
}
