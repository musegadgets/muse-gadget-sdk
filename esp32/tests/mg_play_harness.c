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
 * Playback (components/muse_gadget_ble/mg_play.c, mg_resample.c) on the host:
 * the resampler's frequency response and aliasing at every supported rate,
 * the ring's flow control, prebuffer, underruns, drain and drop, and the
 * decoder end to end on SBC, PCM and LC3 streams made with the real encoders.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_play.h"
#include "mg_resample.h"
#include "mgcommands.h"
#include "sbc.h"

static int failures, checks;
#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                           \
        }                                                                         \
    } while (0)

/* Amplitude of `f` in x (Goertzel), as a fraction of full scale. */
static double tone(const int16_t *x, size_t n, double f, double rate)
{
    double k = 2 * cos(2 * M_PI * f / rate), s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        double s0 = x[i] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double p = s1 * s1 + s2 * s2 - k * s1 * s2;
    return 2 * sqrt(p > 0 ? p : 0) / n / 32768.0;
}

static void sine(int16_t *x, size_t n, double f, double rate, double amp)
{
    for (size_t i = 0; i < n; i++) {
        x[i] = (int16_t)lrint(amp * 32767 * sin(2 * M_PI * f * i / rate));
    }
}

/* Resamples a whole signal in odd-sized chunks. Returns output samples. */
static size_t resample_all(uint32_t in_rate, const int16_t *in, size_t n, int16_t *out, size_t cap)
{
    mg_resample_t r;
    CHECK(mg_resample_open(&r, in_rate, 16000));
    static const size_t steps[] = { 7, 100, 1024, 333, 1 };
    size_t i = 0, o = 0, s = 0;
    while (i < n) {
        size_t k = steps[s++ % 5];
        k = k < n - i ? k : n - i;
        o += mg_resample_run(&r, in + i, k, out + o, cap - o);
        i += k;
    }
    mg_resample_close(&r);
    return o;
}

static void test_resampler(void)
{
    static const uint32_t rates[] = { 8000, 16000, 32000, 44100, 48000 };
    static int16_t in[48000], out[20000];
    for (size_t ri = 0; ri < 5; ri++) {
        uint32_t rate = rates[ri];
        sine(in, rate, 1000, rate, 0.5);
        size_t o = resample_all(rate, in, rate, out, 20000);
        /* One second in is one second out, less the filter's delay at most. */
        CHECK(o <= 16001 && o >= 15900);
        double a = tone(out + 2000, o - 4000, 1000, 16000);
        double off = tone(out + 2000, o - 4000, 1300, 16000);
        printf("resample %u->16000: %zu samples, 1 kHz at %.3f (want 0.5), 1.3 kHz %.4f, %zu B of tables\n", rate, o, a,
               off, mg_resample_bytes(rate, 16000));
        CHECK(fabs(a - 0.5) < 0.02);
        CHECK(off < 0.005);
        /* A 3 kHz tone passes too (inside 0.92 of 4 kHz for the 8 kHz input). */
        sine(in, rate, 3000, rate, 0.5);
        o = resample_all(rate, in, rate, out, 20000);
        double a3 = tone(out + 2000, o - 4000, 3000, 16000);
        CHECK(a3 > (rate == 8000 ? 0.3 : 0.47) && a3 < 0.52);
    }
    /* Aliasing: tones above 8 kHz must not fold back into the output. */
    static const struct { uint32_t rate; double f, alias; } al[] = {
        { 48000, 12000, 4000 }, { 32000, 11000, 5000 }, { 44100, 10000, 6000 }, { 48000, 9500, 6500 },
    };
    for (size_t i = 0; i < sizeof(al) / sizeof(al[0]); i++) {
        sine(in, al[i].rate, al[i].f, al[i].rate, 0.5);
        size_t o = resample_all(al[i].rate, in, al[i].rate, out, 20000);
        double a = tone(out + 2000, o - 4000, al[i].alias, 16000);
        printf("alias %u Hz tone at %u -> %.0f Hz: %.1f dB\n", (unsigned)al[i].f, al[i].rate, al[i].alias,
               20 * log10(a / 0.5 + 1e-12));
        CHECK(20 * log10(a / 0.5 + 1e-12) < -50);
    }
    /* Upsampling 8 kHz: the image at 16 - 1 = 15 kHz doesn't exist at 16 kHz, but 7 kHz images of 1 kHz do. */
    sine(in, 8000, 1000, 8000, 0.5);
    size_t o = resample_all(8000, in, 8000, out, 20000);
    CHECK(20 * log10(tone(out + 2000, o - 4000, 7000, 16000) / 0.5 + 1e-12) < -50);
}

/* SBC frames of a 1 kHz tone, mono, at `rate`. Returns bytes; *fbytes the frame size. */
static size_t sbc_tone(uint32_t rate, int bitpool, uint8_t *out, size_t cap, size_t *fbytes, double secs)
{
    sbc_t s;
    struct sbc_frame f = { 0 };
    f.freq = rate == 16000 ? SBC_FREQ_16K : rate == 32000 ? SBC_FREQ_32K : rate == 44100 ? SBC_FREQ_44K1 : SBC_FREQ_48K;
    f.mode = SBC_MODE_MONO;
    f.bam = SBC_BAM_LOUDNESS;
    f.nblocks = 16;
    f.nsubbands = 8;
    f.bitpool = bitpool;
    sbc_reset(&s);
    size_t fb = sbc_get_frame_size(&f), n = 0;
    *fbytes = fb;
    int16_t pcm[128];
    size_t total = (size_t)(secs * rate);
    for (size_t t = 0; t + 128 <= total && n + fb <= cap; t += 128) {
        for (int i = 0; i < 128; i++) {
            pcm[i] = (int16_t)lrint(0.5 * 32767 * sin(2 * M_PI * 1000.0 * (t + i) / rate));
        }
        CHECK(sbc_encode(&s, pcm, 1, NULL, 0, &f, out + n, (unsigned)fb) == 0);
        n += fb;
    }
    return n;
}

static void test_check(void)
{
    mg_play_params_t p = { .codec = mg_data_type_audio_sbc };
    CHECK(mg_play_check(&p) == 0 && p.rate == 16000 && p.channels == 1);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_sbc, .rate = 44100 };
    CHECK(mg_play_check(&p) == 0);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_sbc, .rate = 8000 };
    CHECK(mg_play_check(&p) != 0);   /* SBC has no 8 kHz */
    p = (mg_play_params_t){ .codec = mg_data_type_audio_sbc, .rate = 22050 };
    CHECK(mg_play_check(&p) != 0);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_sbc, .channels = 2 };
    CHECK(mg_play_check(&p) != 0);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_sbc, .bitrate_kbps = 400 };
    CHECK(mg_play_check(&p) != 0);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_pcm, .rate = 16000 };
    CHECK(mg_play_check(&p) == 0);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_pcm, .rate = 48000 };
    CHECK(mg_play_check(&p) != 0);   /* 768 kb/s */
    p = (mg_play_params_t){ .codec = mg_data_type_audio_opus };
    CHECK(mg_play_check(&p) != 0);
#if MG_WITH_LC3
    p = (mg_play_params_t){ .codec = mg_data_type_audio_lc3 };
    CHECK(mg_play_check(&p) == 0 && p.frame_bytes == 40);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_lc3, .bitrate_kbps = 64 };
    CHECK(mg_play_check(&p) == 0 && p.frame_bytes == 80);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_lc3, .rate = 48000 };
    CHECK(mg_play_check(&p) != 0);
#else
    p = (mg_play_params_t){ .codec = mg_data_type_audio_lc3 };
    CHECK(mg_play_check(&p) != 0);
#endif
    uint8_t c[4];
    size_t nc = mg_play_codecs(c, 4);
    CHECK(nc == (MG_WITH_LC3 ? 3u : 2u) && c[0] == mg_data_type_audio_sbc && c[nc - 1] == mg_data_type_audio_pcm);
}

static void test_ring(void)
{
    static uint8_t sbc[64 * 1024];
    size_t fb;
    size_t n = sbc_tone(16000, 26, sbc, sizeof(sbc), &fb, 2.0);
    CHECK(fb == 60 && n == 250 * 60);
    mg_play_t p;
    mg_play_init(&p, 4096);
    CHECK(!mg_play_active(&p) && mg_play_available(&p) == 0);
    CHECK(!mg_play_write(&p, sbc, 60));   /* not streaming */
    mg_play_params_t pp = { .codec = mg_data_type_audio_sbc };
    CHECK(mg_play_check(&pp) == 0 && mg_play_start(&p, &pp));
    CHECK(mg_play_available(&p) == 4096 && mg_play_received(&p) == 0);
    uint8_t out[MG_PLAY_MAX_POP];
    /* Prebuffering: silence until 100 ms (13 frames of 8 ms) are in. */
    size_t off = 0;
    for (int i = 0; i < 4; i++) {   /* 12 frames */
        CHECK(mg_play_write(&p, sbc + off, 180));
        off += 180;
        CHECK(mg_play_pop(&p, out, sizeof(out), 20000) == 0);
    }
    CHECK(p.state == MG_PLAY_PREBUFFER && p.st.silence_us == 80000 && p.st.underruns == 0);
    CHECK(mg_play_available(&p) == 4096 - 720 && mg_play_received(&p) == 720);
    CHECK(mg_play_write(&p, sbc + off, 60));
    off += 60;
    size_t got = mg_play_pop(&p, out, sizeof(out), 20000);
    CHECK(got == 180 && memcmp(out, sbc, 180) == 0 && p.state == MG_PLAY_PLAYING);   /* 3 frames: 24 ms */
    /* A write that isn't whole frames, or doesn't fit, is dropped whole but counted as received. */
    CHECK(!mg_play_write(&p, sbc + off, 90));
    CHECK(p.st.bad_frames == 1 && p.st.bytes_dropped == 90 && mg_play_received(&p) == 870);
    uint32_t before = mg_play_available(&p);
    static uint8_t junk[4096];
    memcpy(junk, sbc, sizeof(junk) / 60 * 60);
    CHECK(!mg_play_write(&p, junk, 4080));
    CHECK(mg_play_available(&p) == before && p.st.writes_dropped == 2);
    /* A 32 kHz frame in a 16 kHz stream is rejected. */
    size_t fb32;
    static uint8_t sbc32[1024];
    sbc_tone(32000, 26, sbc32, sizeof(sbc32), &fb32, 0.05);
    CHECK(!mg_play_write(&p, sbc32, fb32) && p.st.bad_frames == 2);
    /* Pops follow the writes in order, across the ring's wrap. */
    size_t pos = 180;
    for (int round = 0; round < 40; round++) {
        if (mg_play_available(&p) >= 240 && off + 240 <= n) {
            CHECK(mg_play_write(&p, sbc + off, 240));
            off += 240;
        }
        got = mg_play_pop(&p, out, sizeof(out), 20000);
        if (got) {
            CHECK(memcmp(out, sbc + pos, got) == 0);
            pos += got;
        }
    }
    CHECK(p.st.underruns == 0);
    /* Starve it: an underrun, then silence until the prebuffer is back. */
    while (mg_play_pop(&p, out, sizeof(out), 20000)) {
    }
    CHECK(p.st.underruns == 1 && p.state == MG_PLAY_PREBUFFER);
    CHECK(mg_play_pop(&p, out, sizeof(out), 20000) == 0 && p.st.underruns == 1);
    /* stop keep: plays out what's left, then DONE with keep. */
    CHECK(mg_play_write(&p, sbc, 120));
    mg_play_stop(&p, true);
    CHECK(p.state == MG_PLAY_DRAINING && mg_play_available(&p) == 0 && !mg_play_write(&p, sbc, 60));
    CHECK(mg_play_pop(&p, out, sizeof(out), 20000) == 120);   /* no prebuffer while draining */
    CHECK(mg_play_pop(&p, out, sizeof(out), 20000) == 0 && p.state == MG_PLAY_DONE && p.done_keep);
    /* stop drop empties at once. */
    uint32_t gen = p.generation;
    CHECK(mg_play_start(&p, &pp) && p.generation != gen && mg_play_received(&p) == 0);
    CHECK(mg_play_write(&p, sbc, 600));
    mg_play_stop(&p, false);
    CHECK(p.state == MG_PLAY_IDLE && p.len == 0);
    mg_play_release(&p);
    CHECK(p.buf == NULL);
    /* A client that goes quiet without stop_streaming: the device ends it after 10 s. */
    CHECK(mg_play_start(&p, &pp));
    for (int i = 0; i < 499; i++) {
        mg_play_pop(&p, out, sizeof(out), 20000);
    }
    CHECK(p.state == MG_PLAY_PREBUFFER);
    mg_play_pop(&p, out, sizeof(out), 20000);
    CHECK(p.state == MG_PLAY_DONE && !p.done_keep);
    p.state = MG_PLAY_IDLE;
    mg_play_release(&p);
}

static double snr_db(const int16_t *ref, const int16_t *x, size_t n, int max_lag)
{
    double best = -100;
    for (int lag = 0; lag < max_lag; lag++) {
        double s = 0, e = 0;
        for (size_t i = 500; i + lag < n; i++) {
            double d = (double)x[i + lag] - ref[i];
            s += (double)ref[i] * ref[i];
            e += d * d;
        }
        double v = 10 * log10(s / (e + 1e-9));
        best = v > best ? v : best;
    }
    return best;
}

/* Plays a whole stream through ring and decoder, as the playback task does. */
static size_t play_stream(const mg_play_params_t *pp0, const uint8_t *data, size_t n, size_t write, int16_t *pcm,
                          size_t cap, mg_play_stats_t *st, uint32_t *frames)
{
    mg_play_params_t pp = *pp0;
    CHECK(mg_play_check(&pp) == 0);
    static mg_play_t p;
    mg_play_init(&p, 16384);
    CHECK(mg_play_start(&p, &pp));
    static mg_dec_t d;
    CHECK(mg_dec_open(&d, &pp));
    size_t off = 0, o = 0;
    uint8_t buf[MG_PLAY_MAX_POP];
    bool stopped = false;
    while (p.state != MG_PLAY_DONE && o < cap) {
        while (off < n && mg_play_available(&p) >= write) {
            size_t w = n - off < write ? n - off : write;
            CHECK(mg_play_write(&p, data + off, w));
            off += w;
        }
        if (off >= n && !stopped) {
            mg_play_stop(&p, true);
            stopped = true;
        }
        size_t got = mg_play_pop(&p, buf, sizeof(buf), 20000);
        if (got) {
            o += mg_dec_run(&d, buf, got, pcm + o, cap - o);
        } else if (p.state != MG_PLAY_DONE) {
            size_t z = 320 < cap - o ? 320 : cap - o;
            memset(pcm + o, 0, z * sizeof(int16_t));   /* silence, as the speaker would get */
            o += z;
        }
    }
    CHECK(p.state == MG_PLAY_DONE && p.done_keep);
    *st = p.st;
    *frames = d.frames;
    CHECK(d.bad == 0);
    mg_dec_close(&d);
    p.state = MG_PLAY_IDLE;
    mg_play_release(&p);
    return o;
}

static void test_decode(void)
{
    static uint8_t data[256 * 1024];
    static int16_t pcm[64000], ref[48000];
    mg_play_stats_t st;
    uint32_t frames;
    static const uint32_t rates[] = { 16000, 32000, 44100, 48000 };
    for (size_t i = 0; i < 4; i++) {
        size_t fb;
        size_t n = sbc_tone(rates[i], rates[i] == 16000 ? 26 : 35, data, sizeof(data), &fb, 1.0);
        mg_play_params_t pp = { .codec = mg_data_type_audio_sbc, .rate = rates[i] };
        size_t write = fb * (240 / fb ? 240 / fb : 1);
        size_t o = play_stream(&pp, data, n, write, pcm, 64000, &st, &frames);
        /* Skip the prebuffer's silence and the codec delay; then it's a 1 kHz tone at half scale. */
        size_t start = 1600 + 800;
        double a = tone(pcm + start, 8000, 1000, 16000);
        printf("SBC %u Hz: %u frames in, %u decoded, %zu samples out, 1 kHz at %.3f, %u underruns, %u bytes\n",
               rates[i], st.frames_in, frames, o, a, st.underruns, st.bytes_received);
        CHECK(frames == n / fb && st.frames_in == frames && st.bytes_received == n && st.underruns == 0);
        CHECK(fabs(a - 0.5) < 0.05);
        CHECK(tone(pcm + start, 8000, 1500, 16000) < 0.01);
    }
    /* PCM at 8 kHz, upsampled. */
    sine(ref, 8000, 440, 8000, 0.3);
    for (int i = 0; i < 8000; i++) {
        data[2 * i] = (uint8_t)ref[i];
        data[2 * i + 1] = (uint8_t)(ref[i] >> 8);
    }
    mg_play_params_t pp = { .codec = mg_data_type_audio_pcm, .rate = 8000 };
    size_t o = play_stream(&pp, data, 16000, 244, pcm, 64000, &st, &frames);
    printf("PCM 8 kHz: %zu samples out, %u frames, 440 Hz at %.3f, %u dropped, %u bad\n", o, frames,
           tone(pcm + 3000, 8000, 440, 16000), st.bytes_dropped, st.bad_frames);
    CHECK(o >= 15990 && fabs(tone(pcm + 3000, 8000, 440, 16000) - 0.3) < 0.02);
    /* PCM at 16 kHz is sample exact after the prebuffer. */
    sine(ref, 16000, 700, 16000, 0.4);
    for (int i = 0; i < 16000; i++) {
        data[2 * i] = (uint8_t)ref[i];
        data[2 * i + 1] = (uint8_t)(ref[i] >> 8);
    }
    pp = (mg_play_params_t){ .codec = mg_data_type_audio_pcm, .rate = 16000 };
    o = play_stream(&pp, data, 32000, 244, pcm, 64000, &st, &frames);
    CHECK(o == 16000 && snr_db(ref, pcm, 16000, 2000) > 80);   /* all buffered at once: no prebuffer silence */
#if MG_WITH_LC3
    {
        static lc3_encoder_mem_16k_t mem;
        lc3_encoder_t e = lc3_setup_encoder(10000, 16000, 0, &mem);
        sine(ref, 16000, 1000, 16000, 0.5);
        size_t n = 0;
        for (int t = 0; t + 160 <= 16000; t += 160) {
            CHECK(lc3_encode(e, LC3_PCM_FORMAT_S16, ref + t, 1, 40, data + n) == 0);
            n += 40;
        }
        pp = (mg_play_params_t){ .codec = mg_data_type_audio_lc3 };
        o = play_stream(&pp, data, n, 200, pcm, 64000, &st, &frames);
        double a = tone(pcm + 3000, 8000, 1000, 16000);
        printf("LC3: %u frames decoded, 1 kHz at %.3f\n", frames, a);
        CHECK(frames == 100 && fabs(a - 0.5) < 0.05);
    }
#endif
}

int main(void)
{
    test_resampler();
    test_check();
    test_ring();
    test_decode();
    if (failures) {
        fprintf(stderr, "FAIL: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASS mg_play (%d checks)\n", checks);
    return 0;
}
