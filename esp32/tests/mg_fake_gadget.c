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
 * A gadget without a radio, for tools/mg_ble_client.py's tests: the
 * firmware's protocol (mg_proto.c) and playback (mg_play.c) behind a few
 * functions ctypes can call. fake_step() is 20 ms of the device: the
 * worker's buffer_update tick and the playback task popping and decoding a
 * chunk, and at the end of a stream the stats lines it logs on the Nordic
 * UART mirror. Its speaker_volume starts at 70; 0 mutes it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mg_play.h"
#include "mg_proto.h"

typedef void (*fake_cb)(int ch, const uint8_t *data, int len);

static fake_cb s_notify, s_nus;
static mg_proto_t s_p;
static mg_play_t s_pl;
static mg_dec_t s_dec;
static bool s_dec_open;
static uint32_t s_stream, s_now;
static mg_play_stats_t s_st;
static mg_level_t s_spk;
static uint8_t s_volume = 70;

static uint8_t f_vol(void *ctx)
{
    (void)ctx;
    return s_volume;
}

static void f_set_vol(void *ctx, uint8_t v)
{
    (void)ctx;
    s_volume = v;
}

static int s_assistant = -1;

static void f_assistant(void *ctx, uint8_t st)
{
    (void)ctx;
    s_assistant = st;
}

/* The token proof key Link setup left (fake_set_proof_key), and the clears asked for. */
static uint8_t s_k[MG_TOKEN_PROOF_KEY_LEN];
static bool s_have_k;
static int s_clears;
static uint32_t s_rng = 0x2545F491u;

void fake_set_proof_key(const uint8_t *k)
{
    s_have_k = k != NULL;
    if (k) {
        memcpy(s_k, k, sizeof(s_k));
    }
}

int fake_proof_clears(void)
{
    return s_clears;
}

static bool f_proof_key(void *ctx, uint8_t k[MG_TOKEN_PROOF_KEY_LEN])
{
    (void)ctx;
    memcpy(k, s_k, sizeof(s_k));
    return s_have_k;
}

static void f_proof_clear(void *ctx)
{
    (void)ctx;
    s_clears++;
    s_have_k = false;
}

/* Not a CSPRNG: a fake device's nonces only have to differ. */
static bool f_random(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        s_rng ^= s_rng << 13;
        s_rng ^= s_rng >> 17;
        s_rng ^= s_rng << 5;
        out[i] = (uint8_t)s_rng;
    }
    return true;
}

/* The last assistant_state the board got, or -1. */
int fake_assistant(void)
{
    return s_assistant;
}

static bool f_notify(void *ctx, mg_ch_t ch, const uint8_t *d, size_t n)
{
    (void)ctx;
    s_notify((int)ch, d, (int)n);
    return true;
}

static uint32_t f_now(void *ctx)
{
    (void)ctx;
    return s_now;
}

void fake_init(int buffer, int mtu, fake_cb notify, fake_cb nus)
{
    s_notify = notify;
    s_nus = nus;
    mg_play_init(&s_pl, (uint32_t)buffer);
    /* Like the Waveshare C6: a display and a speaker, no vibration motor, no queue. */
    mg_proto_config_t cfg = { .play = &s_pl, .assistant = true };
    mg_proto_ops_t ops = { .notify = f_notify, .now_ms = f_now, .speaker_volume = f_vol,
                           .set_speaker_volume = f_set_vol, .assistant_state = f_assistant,
                           .random = f_random, .proof_key = f_proof_key, .proof_clear = f_proof_clear };
    mg_proto_init(&s_p, &cfg, &ops, NULL);
    mg_proto_connect(&s_p);
    mg_proto_set_mtu(&s_p, (uint16_t)mtu);
    mg_proto_subscribe(&s_p, MG_CH_CONTROL, true);
    mg_proto_subscribe(&s_p, MG_CH_DATA, true);
}

void fake_write(int ch, const uint8_t *d, int n)
{
    mg_proto_write(&s_p, (mg_ch_t)ch, d, (size_t)n);
}

/* Push-to-talk takes the audio path (claim 1) or gives it back (0). */
void fake_claim(int claim)
{
    mg_proto_claim_audio(&s_p, claim != 0);
}

static void stats_to_nus(const char *how)
{
    char text[400];
    int n = mg_play_format_stats(text, sizeof(text), how, &s_pl.params, &s_st, &s_dec, &s_spk);
    s_nus(0, (const uint8_t *)text, n);
}

void fake_step(void)
{
    static uint8_t frames[MG_PLAY_MAX_POP];
    static int16_t pcm[MG_PLAY_MAX_OUT];
    s_now += 20;
    mg_proto_stream_tick(&s_p);
    if (s_pl.state == MG_PLAY_IDLE) {
        if (s_dec_open) {
            mg_dec_close(&s_dec);
            s_dec_open = false;
        }
        mg_play_release(&s_pl);
        return;
    }
    if (!s_dec_open || s_pl.stream != s_stream) {
        s_dec_open = mg_dec_open(&s_dec, &s_pl.params);
        s_stream = s_pl.stream;
        memset(&s_spk, 0, sizeof(s_spk));
    }
    if (s_pl.state == MG_PLAY_DONE) {
        s_st = s_pl.st;
        bool keep = s_pl.done_keep;
        mg_proto_stream_finished(&s_p);
        stats_to_nus(keep ? "keep" : "starved");
        mg_dec_close(&s_dec);
        s_dec_open = false;
        return;
    }
    size_t n = mg_play_pop(&s_pl, frames, sizeof(frames), 20000);
    s_st = s_pl.st;
    if (n) {
        size_t o = mg_dec_run(&s_dec, frames, n, pcm, MG_PLAY_MAX_OUT);
        mg_level_add(&s_spk, pcm, o, s_volume == 0);   /* what reaches the codec */
    }
}
