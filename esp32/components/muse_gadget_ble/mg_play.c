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

#include "mg_play.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mgcommands.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}
#else
#define big_alloc malloc
#endif

#define LC3_FRAME_US 10000
#define LC3_MIN_BYTES 20
#define LC3_MAX_BYTES 400

static const uint32_t RATES[] = { 16000, 8000, 32000, 44100, 48000 };

void mg_play_init(mg_play_t *p, uint32_t buffer_bytes)
{
    memset(p, 0, sizeof(*p));
    p->size = buffer_bytes;
}

size_t mg_play_codecs(uint8_t *out, size_t cap)
{
    size_t n = 0;
    const uint8_t all[] = {
        mg_data_type_audio_sbc,
#if MG_WITH_LC3
        mg_data_type_audio_lc3,
#endif
        mg_data_type_audio_pcm,
    };
    for (size_t i = 0; i < sizeof(all) && n < cap; i++) {
        out[n++] = all[i];
    }
    return n;
}

size_t mg_play_rates(uint32_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < sizeof(RATES) / sizeof(RATES[0]) && n < cap; i++) {
        out[n++] = RATES[i];
    }
    return n;
}

static bool rate_ok(uint32_t rate)
{
    for (size_t i = 0; i < sizeof(RATES) / sizeof(RATES[0]); i++) {
        if (RATES[i] == rate) {
            return true;
        }
    }
    return false;
}

static enum sbc_freq sbc_freq_of(uint32_t rate)
{
    switch (rate) {
    case 16000: return SBC_FREQ_16K;
    case 32000: return SBC_FREQ_32K;
    case 44100: return SBC_FREQ_44K1;
    case 48000: return SBC_FREQ_48K;
    default: return (enum sbc_freq)-1;
    }
}

uint16_t mg_play_check(mg_play_params_t *pp)
{
    if (!pp->rate) {
        pp->rate = RATES[0];
    }
    if (!pp->channels) {
        pp->channels = 1;
    }
    if (!rate_ok(pp->rate) || pp->channels > MG_PLAY_MAX_CHANNELS
        || pp->bitrate_kbps > MG_PLAY_MAX_BITRATE_KBPS) {
        return mg_error_code_invalid_value;
    }
    switch (pp->codec) {
    case mg_data_type_audio_sbc:
        pp->frame_bytes = 0;   /* self-describing */
        return pp->rate == 8000 ? mg_error_code_invalid_value : 0;   /* SBC has no 8 kHz */
    case mg_data_type_audio_pcm:
        pp->frame_bytes = 0;
        /* 16-bit mono: 8 and 16 kHz fit the maximum bitrate. */
        return pp->rate * 16 / 1000 <= MG_PLAY_MAX_BITRATE_KBPS ? 0 : mg_error_code_invalid_value;
#if MG_WITH_LC3
    case mg_data_type_audio_lc3:
        if (pp->rate != 16000) {
            return mg_error_code_invalid_value;   /* this build decodes 16 kHz, 10 ms only */
        }
        if (!pp->frame_bytes) {
            pp->frame_bytes = pp->bitrate_kbps ? (uint16_t)(pp->bitrate_kbps * LC3_FRAME_US / 8000)
                                               : MG_LC3_DEFAULT_FRAME_BYTES;
        }
        return pp->frame_bytes >= LC3_MIN_BYTES && pp->frame_bytes <= LC3_MAX_BYTES ? 0 : mg_error_code_invalid_value;
#endif
    default:
        return mg_error_code_invalid_value;
    }
}

int32_t mg_play_frames_us(const mg_play_params_t *pp, const uint8_t *d, size_t len, uint32_t *frames)
{
    uint64_t us = 0;
    uint32_t n = 0;
    switch (pp->codec) {
    case mg_data_type_audio_sbc: {
        size_t off = 0;
        while (off < len) {
            struct sbc_frame f;
            if (len - off < SBC_PROBE_SIZE || sbc_probe(d + off, &f) != 0 || f.mode != SBC_MODE_MONO
                || (!f.msbc && f.freq != sbc_freq_of(pp->rate)) || (f.msbc && pp->rate != 16000)) {
                return -1;
            }
            size_t fs = sbc_get_frame_size(&f);
            if (!fs || fs > len - off) {
                return -1;
            }
            us += (uint64_t)f.nblocks * f.nsubbands * 1000000u / pp->rate;
            off += fs;
            n++;
        }
        break;
    }
    case mg_data_type_audio_lc3:
        if (!pp->frame_bytes || len % pp->frame_bytes) {
            return -1;
        }
        n = (uint32_t)(len / pp->frame_bytes);
        us = (uint64_t)n * LC3_FRAME_US;
        break;
    case mg_data_type_audio_pcm:
        if (len % 2) {
            return -1;
        }
        n = (uint32_t)(len / 2);
        us = (uint64_t)n * 1000000u / pp->rate;
        break;
    default:
        return -1;
    }
    if (frames) {
        *frames = n;
    }
    return (int32_t)us;
}

bool mg_play_start(mg_play_t *p, const mg_play_params_t *params)
{
    if (!p->buf && !(p->buf = big_alloc(p->size))) {
        return false;
    }
    p->params = *params;
    p->head = p->len = p->buffered_us = 0;
    p->state = MG_PLAY_PREBUFFER;
    p->generation++;
    p->stream++;
    p->done_keep = false;
    p->starving = false;
    p->starve_us = 0;
    memset(&p->st, 0, sizeof(p->st));
    return true;
}

void mg_play_release(mg_play_t *p)
{
    if (p->state == MG_PLAY_IDLE) {
        free(p->buf);
        p->buf = NULL;
    }
}

bool mg_play_active(const mg_play_t *p)
{
    return p->state != MG_PLAY_IDLE;
}

uint32_t mg_play_available(const mg_play_t *p)
{
    return p->state == MG_PLAY_PREBUFFER || p->state == MG_PLAY_PLAYING ? p->size - p->len : 0;
}

uint32_t mg_play_received(const mg_play_t *p)
{
    return p->st.bytes_received;
}

bool mg_play_write(mg_play_t *p, const uint8_t *data, size_t len)
{
    if (p->state != MG_PLAY_PREBUFFER && p->state != MG_PLAY_PLAYING) {
        return false;   /* not streaming, or draining after stop: ignored */
    }
    p->st.bytes_received += (uint32_t)len;
    uint32_t frames = 0;
    int32_t us = mg_play_frames_us(&p->params, data, len, &frames);
    if (us < 0 || len > p->size - p->len) {
        /* Dropped whole, so the frames that follow still line up. */
        if (us < 0) {
            p->st.bad_frames++;
        }
        p->st.bytes_dropped += (uint32_t)len;
        p->st.writes_dropped++;
        return false;
    }
    uint32_t tail = (p->head + p->len) % p->size;
    size_t first = p->size - tail < len ? p->size - tail : len;
    memcpy(p->buf + tail, data, first);
    memcpy(p->buf, data + first, len - first);
    p->len += (uint32_t)len;
    p->buffered_us += (uint32_t)us;
    p->st.frames_in += frames;
    return true;
}

void mg_play_stop(mg_play_t *p, bool keep)
{
    if (p->state == MG_PLAY_IDLE) {
        return;
    }
    p->generation++;
    if (keep && p->len) {
        p->state = MG_PLAY_DRAINING;
        return;
    }
    p->head = p->len = p->buffered_us = 0;
    p->state = keep ? MG_PLAY_DONE : MG_PLAY_IDLE;
    p->done_keep = keep;
}

static void peek(const mg_play_t *p, uint32_t off, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = p->buf[(p->head + off + i) % p->size];
    }
}

/* Bytes and duration of the frame at the ring's front. */
static bool front_frame(const mg_play_t *p, uint32_t off, size_t *bytes, uint32_t *us)
{
    const mg_play_params_t *pp = &p->params;
    if (off >= p->len) {
        return false;
    }
    switch (pp->codec) {
    case mg_data_type_audio_sbc: {
        uint8_t h[SBC_PROBE_SIZE];
        struct sbc_frame f;
        if (p->len - off < SBC_PROBE_SIZE) {
            return false;
        }
        peek(p, off, h, sizeof(h));
        if (sbc_probe(h, &f) != 0) {
            return false;
        }
        *bytes = sbc_get_frame_size(&f);
        *us = (uint32_t)((uint64_t)f.nblocks * f.nsubbands * 1000000u / pp->rate);
        break;
    }
    case mg_data_type_audio_lc3:
        *bytes = pp->frame_bytes;
        *us = LC3_FRAME_US;
        break;
    default: {
        /* PCM: 10 ms at a time. */
        size_t b = pp->rate / 100 * 2;
        *bytes = p->len - off < b ? (p->len - off) & ~1u : b;
        *us = (uint32_t)((uint64_t)(*bytes / 2) * 1000000u / pp->rate);
        break;
    }
    }
    return *bytes && *bytes <= p->len - off;
}

static uint32_t silence(mg_play_t *p, uint32_t chunk_us)
{
    p->st.silence_us += chunk_us;
    p->starve_us += chunk_us;
    if (p->starve_us >= MG_PLAY_STARVE_END_US) {
        /* The client stopped sending without stop_streaming: end it (drop). */
        p->head = p->len = p->buffered_us = 0;
        p->state = MG_PLAY_DONE;
        p->done_keep = false;
        p->generation++;
    }
    return 0;
}

size_t mg_play_pop(mg_play_t *p, uint8_t *out, size_t cap, uint32_t chunk_us)
{
    switch (p->state) {
    case MG_PLAY_PREBUFFER:
        if (p->buffered_us < MG_PLAY_PREBUFFER_US && p->len < p->size / 2) {
            return silence(p, chunk_us);
        }
        p->state = MG_PLAY_PLAYING;
        break;
    case MG_PLAY_PLAYING:
    case MG_PLAY_DRAINING:
        break;
    default:
        return 0;
    }
    size_t n = 0;
    uint32_t got_us = 0;
    while (got_us < chunk_us) {
        size_t fb;
        uint32_t us;
        if (!front_frame(p, 0, &fb, &us) || n + fb > cap) {
            break;
        }
        peek(p, 0, out + n, fb);
        p->head = (p->head + (uint32_t)fb) % p->size;
        p->len -= (uint32_t)fb;
        p->buffered_us = p->buffered_us > us ? p->buffered_us - us : 0;
        n += fb;
        got_us += us;
        p->st.frames_popped++;
    }
    if (n) {
        p->starving = false;
        p->starve_us = 0;
        return n;
    }
    if (p->state == MG_PLAY_DRAINING) {
        p->state = MG_PLAY_DONE;
        p->done_keep = true;
        p->head = p->len = p->buffered_us = 0;
        return 0;
    }
    /* Ran dry while playing: silence until it has the prebuffer again. */
    if (!p->starving) {
        p->starving = true;
        p->st.underruns++;
    }
    p->state = MG_PLAY_PREBUFFER;
    return silence(p, chunk_us);
}

/* ---- decoding ---------------------------------------------------------- */

bool mg_dec_open(mg_dec_t *d, const mg_play_params_t *params)
{
    memset(d, 0, sizeof(*d));
    d->params = *params;
    sbc_reset(&d->sbc);
#if MG_WITH_LC3
    if (params->codec == mg_data_type_audio_lc3) {
        d->lc3 = lc3_setup_decoder(LC3_FRAME_US, 16000, 0, &d->lc3_mem);
        if (!d->lc3) {
            return false;
        }
    }
#endif
    return mg_resample_open(&d->rs, params->rate, MG_PLAY_OUT_RATE);
}

void mg_dec_close(mg_dec_t *d)
{
    mg_resample_close(&d->rs);
}

static size_t emit(mg_dec_t *d, const int16_t *pcm, size_t n, int16_t *out, size_t cap)
{
    size_t o = mg_resample_run(&d->rs, pcm, n, out, cap);
    for (size_t i = 0; i < o; i++) {
        int32_t v = out[i] < 0 ? -out[i] : out[i];
        d->peak = v > d->peak ? v : d->peak;
        d->sum_sq += (uint64_t)((int32_t)out[i] * out[i]);
    }
    d->samples_out += (uint32_t)o;
    return o;
}

size_t mg_dec_run(mg_dec_t *d, const uint8_t *data, size_t len, int16_t *out, size_t cap)
{
    size_t off = 0, o = 0;
    const mg_play_params_t *pp = &d->params;
    while (off < len) {
        size_t n = 0, used = 0;
        switch (pp->codec) {
        case mg_data_type_audio_sbc: {
            struct sbc_frame f;
            if (len - off < SBC_PROBE_SIZE || sbc_probe(data + off, &f) != 0) {
                d->bad++;
                return o;
            }
            used = sbc_get_frame_size(&f);
            n = (size_t)f.nblocks * f.nsubbands;
            if (!used || used > len - off || n > MG_RESAMPLE_MAX_IN
                || sbc_decode(&d->sbc, data + off, (unsigned)used, &f, d->pcm, 1, NULL, 0) != 0) {
                d->bad++;
                return o;
            }
            break;
        }
#if MG_WITH_LC3
        case mg_data_type_audio_lc3:
            used = pp->frame_bytes;
            n = 160;
            if (used > len - off || lc3_decode(d->lc3, data + off, (int)used, LC3_PCM_FORMAT_S16, d->pcm, 1) < 0) {
                d->bad++;
                return o;
            }
            break;
#endif
        case mg_data_type_audio_pcm:
            used = len - off;
            if (used > sizeof(d->pcm)) {
                used = sizeof(d->pcm);
            }
            used &= ~(size_t)1;
            n = used / 2;
            for (size_t i = 0; i < n; i++) {
                d->pcm[i] = (int16_t)(data[off + 2 * i] | data[off + 2 * i + 1] << 8);
            }
            break;
        default:
            return o;
        }
        off += used;
        d->frames++;
        o += emit(d, d->pcm, n, out + o, cap - o);
    }
    return o;
}

void mg_level_add(mg_level_t *l, const int16_t *pcm, size_t n, bool muted)
{
    l->samples += (uint32_t)n;
    if (muted) {
        l->muted += (uint32_t)n;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        l->peak = v > l->peak ? v : l->peak;
        l->sum_sq += (uint64_t)((int32_t)pcm[i] * pcm[i]);
    }
}

static double db(double amp)
{
    double v = 20 * log10(amp / 32768.0 + 1e-9);
    return v < -120 ? -120 : v;
}

int mg_play_format_stats(char *out, size_t cap, const char *how, const mg_play_params_t *pp, const mg_play_stats_t *st,
                         const mg_dec_t *d, const mg_level_t *spk)
{
    const char *codec = pp->codec == mg_data_type_audio_sbc ? "sbc" : pp->codec == mg_data_type_audio_lc3 ? "lc3" : "pcm";
    double dec_rms = d->samples_out ? sqrt((double)d->sum_sq / d->samples_out) : 0;
    mg_level_t same = { .sum_sq = d->sum_sq, .peak = d->peak, .samples = d->samples_out };
    if (!spk) {
        spk = &same;
    }
    double spk_rms = spk->samples ? sqrt((double)spk->sum_sq / spk->samples) : 0;
    char off[40] = "";
    if (spk->muted && spk->muted >= spk->samples) {
        snprintf(off, sizeof(off), ", speaker off");
    } else if (spk->muted) {
        snprintf(off, sizeof(off), ", speaker off %lu ms", (unsigned long)(spk->muted / (MG_PLAY_OUT_RATE / 1000)));
    }
    /* Three lines: the Nordic UART mirror takes lines of up to 160 characters. */
    return snprintf(out, cap,
                    "stream end (%s): %s %lu Hz, %lu frames in, %lu decoded (%lu bad), %lu underruns, %lu ms silence\n"
                    "stream bytes: %lu received (%lu dropped), %lu ms played\n"
                    "stream level: decoded rms %.1f peak %.1f dBFS, speaker rms %.1f peak %.1f dBFS%s",
                    how, codec, (unsigned long)pp->rate, (unsigned long)st->frames_in, (unsigned long)d->frames,
                    (unsigned long)d->bad, (unsigned long)st->underruns, (unsigned long)(st->silence_us / 1000),
                    (unsigned long)st->bytes_received, (unsigned long)st->bytes_dropped,
                    (unsigned long)(d->samples_out / (MG_PLAY_OUT_RATE / 1000)), db(dec_rms), db(d->peak), db(spk_rms),
                    db(spk->peak), off);
}
