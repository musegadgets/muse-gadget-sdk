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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_config.h"
#include "mg_resample.h"
#include "sbc.h"
#if MG_WITH_LC3
#include "lc3.h"
#endif

/*
 * Audio playback (mg_command_stream_audio): the client's encoded frames go
 * into a byte ring (mg_play_t, filled from BLE writes); a playback task pops
 * whole frames and decodes them (mg_dec_t) to the speaker's 16 kHz mono.
 * No locking here: the caller serializes mg_play_* calls; mg_dec_t belongs
 * to the playback task alone.
 */

#define MG_PLAY_OUT_RATE 16000
#define MG_PLAY_MAX_BITRATE_KBPS 256
#define MG_PLAY_MAX_CHANNELS 1
#define MG_PLAY_PREBUFFER_US 100000        /* start (and restart after an underrun) with this much */
#define MG_PLAY_STARVE_END_US 10000000     /* starved this long: the device ends the stream */
#define MG_PLAY_MAX_POP 1024               /* bytes of frames per mg_play_pop() */
#define MG_PLAY_MAX_OUT 2048               /* samples per mg_dec_run() */

typedef struct {
    uint8_t codec;           /* mg_data_type_t */
    uint32_t rate;           /* Hz */
    uint8_t channels;
    uint16_t bitrate_kbps;   /* 0: not given */
    uint16_t frame_bytes;    /* LC3: bytes per frame; others: 0 */
} mg_play_params_t;

typedef enum {
    MG_PLAY_IDLE,
    MG_PLAY_PREBUFFER,       /* collecting MG_PLAY_PREBUFFER_US (silence plays meanwhile) */
    MG_PLAY_PLAYING,
    MG_PLAY_DRAINING,        /* stop_streaming keep: playing out what's left */
    MG_PLAY_DONE,            /* over: the playback task notifies stop_streaming (done_keep says which) */
} mg_play_state_t;

typedef struct {
    uint32_t bytes_received;    /* all Data bytes since start_streaming, as buffer_update reports */
    uint32_t bytes_dropped;     /* writes that didn't fit, or weren't whole frames */
    uint32_t writes_dropped;
    uint32_t bad_frames;        /* writes rejected as not whole frames of the stream's format */
    uint32_t frames_in;
    uint32_t frames_popped;
    uint32_t underruns;         /* times it ran dry while playing */
    uint32_t silence_us;        /* silence played for underruns and prebuffering */
} mg_play_stats_t;

typedef struct {
    uint32_t size;
    uint8_t *buf;               /* allocated at start, freed by mg_play_release */
    uint32_t head, len;
    uint32_t buffered_us;       /* audio held */
    mg_play_state_t state;
    mg_play_params_t params;
    uint32_t generation;        /* bumped by every start and stop */
    uint32_t stream;            /* bumped by every start: the playback task starts over on a change */
    bool done_keep;
    bool starving;
    uint32_t starve_us;
    mg_play_stats_t st;
} mg_play_t;

void mg_play_init(mg_play_t *p, uint32_t buffer_bytes);

/* Capabilities, the default first. */
size_t mg_play_codecs(uint8_t *out, size_t cap);
size_t mg_play_rates(uint32_t *out, size_t cap);

/*
 * Fills the omitted parameters (codec is required) and checks them:
 * 0 if playable, else mg_error_code_invalid_value.
 */
uint16_t mg_play_check(mg_play_params_t *params);

/* Starts a stream (restarting any). False if the buffer can't be allocated. */
bool mg_play_start(mg_play_t *p, const mg_play_params_t *params);
/* Frees the buffer once idle. */
void mg_play_release(mg_play_t *p);
bool mg_play_active(const mg_play_t *p);
uint32_t mg_play_available(const mg_play_t *p);
uint32_t mg_play_received(const mg_play_t *p);

/* One Data write: whole frames. Returns true if it went into the buffer. */
bool mg_play_write(mg_play_t *p, const uint8_t *data, size_t len);
/* stop_streaming: keep plays out the buffer (DRAINING, then DONE); drop empties it (IDLE). */
void mg_play_stop(mg_play_t *p, bool keep);

/*
 * The playback task's side: pops whole frames worth at least `chunk_us`
 * (or what there is) into `out`, and returns their bytes; 0 means play
 * `chunk_us` of silence (prebuffering or an underrun), which it counts.
 */
size_t mg_play_pop(mg_play_t *p, uint8_t *out, size_t cap, uint32_t chunk_us);

/* Duration of `len` bytes of whole frames in the stream's format, or -1 if they aren't. */
int32_t mg_play_frames_us(const mg_play_params_t *params, const uint8_t *data, size_t len, uint32_t *frames);

/* ---- decoding ---------------------------------------------------------- */

typedef struct {
    mg_play_params_t params;
    sbc_t sbc;
#if MG_WITH_LC3
    lc3_decoder_t lc3;
    lc3_decoder_mem_16k_t lc3_mem;
#endif
    mg_resample_t rs;
    int16_t pcm[MG_RESAMPLE_MAX_IN];
    /* Stats for the stream. */
    uint32_t frames, bad, samples_out;
    int32_t peak;
    uint64_t sum_sq;
} mg_dec_t;

bool mg_dec_open(mg_dec_t *d, const mg_play_params_t *params);
void mg_dec_close(mg_dec_t *d);
/* Decodes whole frames to MG_PLAY_OUT_RATE mono; returns samples written (<= cap). */
size_t mg_dec_run(mg_dec_t *d, const uint8_t *data, size_t len, int16_t *out, size_t cap);

/* The level of what reached the speaker's codec: muted samples count as zeros. */
typedef struct {
    uint64_t sum_sq;
    int32_t peak;
    uint32_t samples;
    uint32_t muted;             /* samples written with the speaker off */
} mg_level_t;

void mg_level_add(mg_level_t *l, const int16_t *pcm, size_t n, bool muted);

/*
 * The per-stream stats the device logs on the Nordic UART mirror, as three
 * lines ("stream end (<how>): ...", "stream bytes: ..." and "stream level:
 * decoded ... speaker ...[, speaker off]"; the mirror takes lines of up to
 * 160 characters). `spk` is what reached the codec (NULL: the decoded audio,
 * unmuted). tools/mg_ble_client.py parses them.
 */
int mg_play_format_stats(char *out, size_t cap, const char *how, const mg_play_params_t *params,
                         const mg_play_stats_t *st, const mg_dec_t *d, const mg_level_t *spk);
