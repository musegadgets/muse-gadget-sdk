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

#include "mg_proto.h"

#include <stdlib.h>
#include <string.h>

#include "mg_codec.h"
#include "mg_token_proof.h"
#include "mgcommands.h"

#define MAX_MSG 96   /* largest Control message the device sends */

#define DEFAULT_BUZZ_FREQ_HZ 210
#define DEFAULT_BUZZ_MS 100
#define DEFAULT_BUZZ_VOLUME 80

void mg_settings_defaults(mg_settings_t *s)
{
    *s = (mg_settings_t){
        .buzz_freq_hz = DEFAULT_BUZZ_FREQ_HZ,
        .buzz_ms = DEFAULT_BUZZ_MS,
        .buzz_volume = DEFAULT_BUZZ_VOLUME,
        .codec = mg_data_type_audio_sbc,
        .queue_enabled = false,
    };
}

static uint16_t le16(const uint8_t *b)
{
    return (uint16_t)(b[0] | b[1] << 8);
}

static void put16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
}

static void stream_abort(mg_proto_t *p, bool notify);
static size_t setting_width(const mg_proto_t *p, uint8_t param);
static void proof_reset(mg_proto_t *p);

static bool proof_supported(const mg_proto_t *p)
{
    return p->ops.proof_key && p->ops.proof_clear && p->ops.random;
}

/* ---- sending ------------------------------------------------------------ */

static bool raw(mg_proto_t *p, mg_ch_t ch, const uint8_t *d, size_t n)
{
    return p->connected && p->sub[ch] && p->ops.notify && p->ops.notify(p->ops.ctx, ch, d, n);
}

static bool send_control(mg_proto_t *p, const uint8_t *d, size_t n)
{
    return raw(p, MG_CH_CONTROL, d, n);
}

static bool send_data(mg_proto_t *p, const uint8_t *d, size_t n)
{
    return raw(p, MG_CH_DATA, d, n);
}

static void send_error(mg_proto_t *p, uint8_t cmd, uint16_t code)
{
    uint8_t m[5] = { mg_command_error, cmd, mg_error_data_code, (uint8_t)code, (uint8_t)(code >> 8) };
    send_control(p, m, sizeof(m));
}

static void send_data_type(mg_proto_t *p)
{
    uint8_t m[10];
    size_t n = mg_codec_change_data_type(p->data_type, m);
    send_control(p, m, n);
}

static void send_features(mg_proto_t *p)
{
    uint8_t m[24];
    size_t n = 0;
    m[n++] = mg_command_supported_features;
    /* Commands the device accepts; ones it only notifies (change_data_type, gesture, error) aren't listed. */
    static const uint8_t base[] = {
        mg_command_start_mic, mg_command_stop_mic, mg_command_set_mic_gain, mg_command_request_status,
        mg_command_get_settings, mg_command_set_settings,
    };
    memcpy(m + n, base, sizeof(base));
    n += sizeof(base);
    if (p->cfg.play) {
        m[n++] = mg_command_stream_audio;
    }
    if (p->cfg.queue) {
        m[n++] = mg_command_audio_queue;
    }
    if (p->cfg.assistant) {
        m[n++] = mg_command_assistant_state;
    }
    if (proof_supported(p)) {
        m[n++] = mg_command_token_proof;
    }
    m[n++] = mg_command_device_action;
    send_control(p, m, n);

    n = 0;
    m[n++] = mg_command_supported_features;
    m[n++] = mg_command_sub_feature;
    m[n++] = mg_command_start_mic;
    n += mg_codec_list(m + n, sizeof(m) - n);
    send_control(p, m, n);

    /* Every setting get_settings and set_settings accept. */
    n = 0;
    m[n++] = mg_command_supported_features;
    m[n++] = mg_command_sub_feature;
    m[n++] = mg_command_set_settings;
    for (unsigned s = 0; s <= 0xFF && n < sizeof(m); s++) {
        if (setting_width(p, (uint8_t)s)) {
            m[n++] = (uint8_t)s;
        }
    }
    send_control(p, m, n);

    /* Device actions: the throughput test. */
    uint8_t da[] = { mg_command_supported_features, mg_command_sub_feature, mg_command_device_action,
                     mg_device_action_throughput_test };
    send_control(p, da, sizeof(da));
}

/* ---- lifecycle ---------------------------------------------------------- */

static void reset_connection(mg_proto_t *p)
{
    p->tp.mode = 0;   /* a throughput test ends with the connection, unreported */
    p->tp.armed = false;
    proof_reset(p);
    memset(p->sub, 0, sizeof(p->sub));
    p->mtu = 23;
    p->ptt_enabled = false;
    p->haptics = false;
    p->start_mic_codec = 0;
    p->data_type = p->settings.codec;
    p->live = false;
    p->reports_state = false;
    p->transfer = false;
}

void mg_proto_init(mg_proto_t *p, const mg_proto_config_t *cfg, const mg_proto_ops_t *ops,
                   const mg_settings_t *settings)
{
    memset(p, 0, sizeof(*p));
    p->cfg = *cfg;
    p->ops = *ops;
    if (settings) {
        p->settings = *settings;
    } else {
        mg_settings_defaults(&p->settings);
    }
    if (!mg_codec_supported(p->settings.codec)) {
        p->settings.codec = mg_data_type_audio_sbc;
    }
    if (!p->cfg.queue) {
        p->settings.queue_enabled = false;
    }
    reset_connection(p);
}

const mg_settings_t *mg_proto_settings(const mg_proto_t *p)
{
    return &p->settings;
}

void mg_proto_connect(mg_proto_t *p)
{
    stream_abort(p, false);
    reset_connection(p);
    p->connected = true;
}

void mg_proto_disconnect(mg_proto_t *p)
{
    bool was = p->connected;
    stream_abort(p, false);   /* nobody left to tell */
    reset_connection(p);
    p->connected = false;
    /* A turn left thinking or responding ends with its client. */
    if (was && p->cfg.assistant && p->ops.assistant_state) {
        p->ops.assistant_state(p->ops.ctx, mg_assistant_state_idle);
    }
    /* Ends a capture the client started, or one it was about to get. */
    if (was && p->ops.capture) {
        p->ops.capture(p->ops.ctx, false, 0);
    }
}

void mg_proto_set_mtu(mg_proto_t *p, uint16_t mtu)
{
    p->mtu = mtu;
}

void mg_proto_subscribe(mg_proto_t *p, mg_ch_t ch, bool on)
{
    if (ch < MG_CH_COUNT) {
        p->sub[ch] = on;
    }
}

/* ---- settings ----------------------------------------------------------- */

/* Byte width of a parameter's value, 0 if unknown here. */
static size_t setting_width(const mg_proto_t *p, uint8_t param)
{
    switch (param) {
    case mg_setting_parameter_spec_version:
    case mg_setting_parameter_push_to_talk_enabled:
    case mg_setting_parameter_audio_codec:
        return 1;
    /* The activation buzz needs a vibration motor. */
    case mg_setting_parameter_haptics_enabled:
    case mg_setting_parameter_ptt_buzz_volume_percent:
        return p->cfg.haptics ? 1 : 0;
    case mg_setting_parameter_ptt_buzz_freq_hz:
    case mg_setting_parameter_ptt_buzz_duration_ms:
        return p->cfg.haptics ? 2 : 0;
    case mg_setting_parameter_audio_queue_enabled:
        return p->cfg.queue ? 1 : 0;
    case mg_setting_parameter_speaker_volume:
        return p->ops.speaker_volume && p->ops.set_speaker_volume ? 1 : 0;
    default:
        return 0;
    }
}

static uint16_t setting_get(const mg_proto_t *p, uint8_t param)
{
    switch (param) {
    case mg_setting_parameter_spec_version: return MG_SPEC_VERSION;
    case mg_setting_parameter_push_to_talk_enabled: return p->ptt_enabled;
    case mg_setting_parameter_haptics_enabled: return p->haptics;
    case mg_setting_parameter_ptt_buzz_freq_hz: return p->settings.buzz_freq_hz;
    case mg_setting_parameter_ptt_buzz_duration_ms: return p->settings.buzz_ms;
    case mg_setting_parameter_ptt_buzz_volume_percent: return p->settings.buzz_volume;
    case mg_setting_parameter_audio_codec: return p->settings.codec;
    case mg_setting_parameter_audio_queue_enabled: return p->settings.queue_enabled;
    case mg_setting_parameter_speaker_volume: return p->ops.speaker_volume(p->ops.ctx);
    default: return 0;
    }
}

/* Applies one setting; false if the value is out of range. *persist: it's a stored one. */
static bool setting_set(mg_proto_t *p, uint8_t param, uint16_t v, bool *persist)
{
    switch (param) {
    case mg_setting_parameter_push_to_talk_enabled:
        if (v > 1) return false;
        p->ptt_enabled = v;
        return true;
    case mg_setting_parameter_haptics_enabled:
        if (v > 1) return false;
        p->haptics = v;
        return true;
    case mg_setting_parameter_ptt_buzz_freq_hz:
        if (v < 20 || v > 20000) return false;
        p->settings.buzz_freq_hz = v;
        break;
    case mg_setting_parameter_ptt_buzz_duration_ms:
        if (v < 1 || v > 5000) return false;
        p->settings.buzz_ms = v;
        break;
    case mg_setting_parameter_ptt_buzz_volume_percent:
        if (v > 100) return false;
        p->settings.buzz_volume = (uint8_t)v;
        break;
    case mg_setting_parameter_audio_codec:
        if (!mg_codec_supported((uint8_t)v)) return false;
        p->settings.codec = (uint8_t)v;
        if (!p->live && !p->transfer) {
            p->data_type = p->settings.codec;
        }
        break;
    case mg_setting_parameter_audio_queue_enabled:
        if (v > 1) return false;
        p->settings.queue_enabled = v;
        break;
    case mg_setting_parameter_speaker_volume:
        /* The board keeps this one (it's also the Muse speaker setting). */
        if (v > 100) return false;
        p->ops.set_speaker_volume(p->ops.ctx, (uint8_t)v);
        return true;
    default:
        return false;   /* spec_version is read-only */
    }
    *persist = true;
    return true;
}

static void cmd_get_settings(mg_proto_t *p, const uint8_t *a, size_t n)
{
    uint8_t m[MAX_MSG];
    size_t o = 0;
    m[o++] = mg_command_get_settings;
    for (size_t i = 0; i < n; i++) {
        size_t w = setting_width(p, a[i]);
        if (!w) {
            send_error(p, mg_command_get_settings, mg_error_code_unsupported);
            return;
        }
        if (o + 1 + w > sizeof(m)) {
            send_error(p, mg_command_get_settings, mg_error_code_invalid_length);
            return;
        }
        uint16_t v = setting_get(p, a[i]);
        m[o++] = a[i];
        m[o++] = (uint8_t)v;
        if (w == 2) {
            m[o++] = (uint8_t)(v >> 8);
        }
    }
    send_control(p, m, o);
}

static void cmd_set_settings(mg_proto_t *p, const uint8_t *a, size_t n)
{
    bool persist = false;
    size_t i = 0;
    while (i < n) {
        uint8_t param = a[i++];
        size_t w = setting_width(p, param);
        if (!w) {
            send_error(p, mg_command_set_settings, mg_error_code_unsupported);
            break;
        }
        if (n - i < w) {
            send_error(p, mg_command_set_settings, mg_error_code_invalid_length);
            break;
        }
        uint16_t v = w == 2 ? le16(a + i) : a[i];
        i += w;
        if (!setting_set(p, param, v, &persist)) {
            send_error(p, mg_command_set_settings, mg_error_code_invalid_value);
            break;
        }
    }
    /* Parameters before a bad one still apply. */
    if (persist && p->ops.settings_changed) {
        p->ops.settings_changed(p->ops.ctx);
    }
}

/* ---- playback ------------------------------------------------------------ */

static uint32_t now_ms(const mg_proto_t *p)
{
    return p->ops.now_ms ? p->ops.now_ms(p->ops.ctx) : 0;
}

static void kick(mg_proto_t *p)
{
    if (p->ops.play_kick) {
        p->ops.play_kick(p->ops.ctx);
    }
}

static void send_buffer_update(mg_proto_t *p)
{
    uint8_t m[10] = { mg_command_stream_audio, mg_stream_audio_buffer_update };
    uint32_t avail = mg_play_available(p->cfg.play);
    put32(m + 2, avail);
    put32(m + 6, mg_play_received(p->cfg.play));
    if (send_control(p, m, sizeof(m))) {
        p->update_ms = now_ms(p);
        p->update_avail = avail;
    }
}

static void send_stop(mg_proto_t *p, uint8_t action)
{
    uint8_t m[3] = { mg_command_stream_audio, mg_stream_audio_stop_streaming, action };
    send_control(p, m, sizeof(m));
}

static void send_capabilities(mg_proto_t *p)
{
    uint8_t m[64];
    size_t n = 0;
    m[n++] = mg_command_stream_audio;
    m[n++] = mg_stream_audio_report_capabilities;
    m[n++] = 5;
    m[n++] = mg_stream_audio_capability_codecs;
    uint8_t *count = &m[n++];
    *count = (uint8_t)mg_play_codecs(m + n, 8);
    n += *count;
    m[n++] = mg_stream_audio_capability_buffer_size;
    put32(m + n, p->cfg.play->size);
    n += 4;
    m[n++] = mg_stream_audio_capability_sample_rates;
    uint32_t rates[8];
    size_t nr = mg_play_rates(rates, 8);
    m[n++] = (uint8_t)nr;
    for (size_t i = 0; i < nr; i++) {
        put32(m + n, rates[i]);
        n += 4;
    }
    m[n++] = mg_stream_audio_capability_max_bitrate;
    put16(m + n, MG_PLAY_MAX_BITRATE_KBPS);
    n += 2;
    m[n++] = mg_stream_audio_capability_max_channels;
    m[n++] = MG_PLAY_MAX_CHANNELS;
    send_control(p, m, n);
}

/* Ends a stream from the device's side: the client hears stop_streaming drop. */
static void stream_abort(mg_proto_t *p, bool notify)
{
    mg_play_t *pl = p->cfg.play;
    if (!pl || !mg_play_active(pl)) {
        return;
    }
    mg_play_stop(pl, false);
    if (notify) {
        send_stop(p, mg_stream_audio_buffer_drop);
    }
    kick(p);
}

static void cmd_start_streaming(mg_proto_t *p, const uint8_t *a, size_t n)
{
    mg_play_params_t pp = { 0 };
    if (n < 1) {
        send_error(p, mg_command_stream_audio, mg_error_code_invalid_length);
        return;
    }
    size_t count = a[0], i = 1;
    for (size_t e = 0; e < count; e++) {
        if (i >= n) {
            send_error(p, mg_command_stream_audio, mg_error_code_invalid_length);
            return;
        }
        uint8_t param = a[i++];
        size_t w = param == mg_stream_audio_parameter_codec || param == mg_stream_audio_parameter_channels ? 1
                   : param == mg_stream_audio_parameter_sample_rate                                          ? 4
                   : param == mg_stream_audio_parameter_bitrate || param == mg_stream_audio_parameter_frame_size
                       ? 2
                       : 0;
        if (!w) {
            send_error(p, mg_command_stream_audio, mg_error_code_unsupported);
            return;
        }
        if (n - i < w) {
            send_error(p, mg_command_stream_audio, mg_error_code_invalid_length);
            return;
        }
        uint32_t v = a[i];
        if (w >= 2) {
            v |= (uint32_t)a[i + 1] << 8;
        }
        if (w == 4) {
            v |= (uint32_t)a[i + 2] << 16 | (uint32_t)a[i + 3] << 24;
        }
        i += w;
        switch (param) {
        case mg_stream_audio_parameter_codec: pp.codec = (uint8_t)v; break;
        case mg_stream_audio_parameter_sample_rate: pp.rate = v; break;
        case mg_stream_audio_parameter_bitrate: pp.bitrate_kbps = (uint16_t)v; break;
        case mg_stream_audio_parameter_channels: pp.channels = (uint8_t)v; break;
        default: pp.frame_bytes = (uint16_t)v; break;
        }
    }
    if (!pp.codec || mg_play_check(&pp)) {
        send_error(p, mg_command_stream_audio, mg_error_code_invalid_value);
        return;
    }
    if (p->audio_claimed || !mg_play_start(p->cfg.play, &pp)) {
        /* Push-to-talk has the audio path, or no memory for the buffer. */
        send_error(p, mg_command_stream_audio, mg_error_code_busy);
        return;
    }
    send_buffer_update(p);
    kick(p);
}

static void cmd_stream_audio(mg_proto_t *p, const uint8_t *a, size_t n)
{
    mg_play_t *pl = p->cfg.play;
    if (!pl) {
        send_error(p, mg_command_stream_audio, mg_error_code_unsupported);
        return;
    }
    if (n < 1) {
        send_error(p, mg_command_stream_audio, mg_error_code_invalid_length);
        return;
    }
    switch (a[0]) {
    case mg_stream_audio_request_capabilities:
        send_capabilities(p);
        return;
    case mg_stream_audio_start_streaming:
        cmd_start_streaming(p, a + 1, n - 1);
        return;
    case mg_stream_audio_stop_streaming: {
        if (n < 2) {
            send_error(p, mg_command_stream_audio, mg_error_code_invalid_length);
            return;
        }
        uint8_t action = a[1];
        if (action != mg_stream_audio_buffer_keep && action != mg_stream_audio_buffer_drop) {
            send_error(p, mg_command_stream_audio, mg_error_code_invalid_value);
            return;
        }
        if (!mg_play_active(pl)) {
            send_stop(p, action);   /* nothing playing: already stopped */
            return;
        }
        mg_play_stop(pl, action == mg_stream_audio_buffer_keep);
        if (pl->state == MG_PLAY_IDLE) {
            send_stop(p, mg_stream_audio_buffer_drop);
        }
        /* keep: the playback task plays out the buffer, then mg_proto_stream_finished notifies. */
        kick(p);
        return;
    }
    case mg_stream_audio_request_buffer_update:
        send_buffer_update(p);
        return;
    default:
        send_error(p, mg_command_stream_audio, mg_error_code_unsupported);
        return;
    }
}

/* Data written by the client: stream audio. */
static bool tp_expired(const mg_proto_t *p);

static void data_in(mg_proto_t *p, const uint8_t *d, size_t n)
{
    if (p->tp.mode == mg_throughput_test_receive) {
        if (p->tp.armed && !tp_expired(p)) {
            mg_tp_account(&p->tp.c, d, n, now_ms(p));
        }
        return;
    }
    mg_play_t *pl = p->cfg.play;
    if (pl && n) {
        mg_play_write(pl, d, n);
    }
}

void mg_proto_claim_audio(mg_proto_t *p, bool claim)
{
    p->audio_claimed = claim;
    if (claim) {
        stream_abort(p, p->connected && mg_proto_ready(p));
    }
}

void mg_proto_stream_tick(mg_proto_t *p)
{
    mg_play_t *pl = p->cfg.play;
    if (!pl || !p->connected || (pl->state != MG_PLAY_PREBUFFER && pl->state != MG_PLAY_PLAYING)) {
        return;
    }
    uint32_t since = now_ms(p) - p->update_ms;
    if (since >= 250 || (since >= 100 && mg_play_available(pl) != p->update_avail)) {
        send_buffer_update(p);
    }
}

void mg_proto_stream_finished(mg_proto_t *p)
{
    mg_play_t *pl = p->cfg.play;
    if (!pl || pl->state != MG_PLAY_DONE) {
        return;
    }
    bool keep = pl->done_keep;
    pl->state = MG_PLAY_IDLE;
    mg_play_release(pl);
    if (p->connected) {
        send_stop(p, keep ? mg_stream_audio_buffer_keep : mg_stream_audio_buffer_drop);
    }
}

/* ---- token proof (mgcommands.h, Token proof) ---------------------------- */

static void proof_reset(mg_proto_t *p)
{
    mg_token_proof_wipe(&p->proof, sizeof(p->proof));
}

static void proof_challenge(mg_proto_t *p, const uint8_t *a, size_t n)
{
    proof_reset(p);   /* a new challenge restarts the exchange */
    if (n < MG_TOKEN_PROOF_NONCE_LEN) {
        send_error(p, mg_command_token_proof, mg_error_code_invalid_length);
        return;
    }
    uint8_t k[MG_TOKEN_PROOF_KEY_LEN];
    if (!p->ops.proof_key(p->ops.ctx, k)) {
        mg_token_proof_wipe(k, sizeof(k));
        send_error(p, mg_command_token_proof, mg_error_code_not_found);
        return;
    }
    uint8_t m[2 + MG_TOKEN_PROOF_NONCE_LEN + MG_TOKEN_PROOF_MAC_LEN] = { mg_command_token_proof,
                                                                       mg_token_proof_response };
    memcpy(p->proof.client_nonce, a, MG_TOKEN_PROOF_NONCE_LEN);
    bool ok = p->ops.random(p->ops.ctx, p->proof.device_nonce, MG_TOKEN_PROOF_NONCE_LEN)
              && mg_token_proof_mac(k, true, p->proof.client_nonce, p->proof.device_nonce,
                                    m + 2 + MG_TOKEN_PROOF_NONCE_LEN)
              && mg_token_proof_mac(k, false, p->proof.client_nonce, p->proof.device_nonce, p->proof.client_mac);
    mg_token_proof_wipe(k, sizeof(k));
    if (!ok) {
        proof_reset(p);
        send_error(p, mg_command_token_proof, mg_error_code_busy);
        return;
    }
    memcpy(m + 2, p->proof.device_nonce, MG_TOKEN_PROOF_NONCE_LEN);
    p->proof.state = MG_PROOF_RESPONDED;
    if (!send_control(p, m, sizeof(m))) {
        proof_reset(p);   /* the client never saw the nonce: nothing to confirm */
    }
}

static void send_proof_result(mg_proto_t *p, bool matched)
{
    uint8_t m[3] = { mg_command_token_proof, mg_token_proof_result, matched ? 1 : 0 };
    send_control(p, m, sizeof(m));
}

static void cmd_token_proof(mg_proto_t *p, const uint8_t *a, size_t n)
{
    if (!proof_supported(p)) {
        send_error(p, mg_command_token_proof, mg_error_code_unsupported);
        return;
    }
    if (n < 1) {
        send_error(p, mg_command_token_proof, mg_error_code_invalid_length);
        return;
    }
    switch (a[0]) {
    case mg_token_proof_challenge:
        proof_challenge(p, a + 1, n - 1);
        return;
    case mg_token_proof_confirm: {
        if (p->proof.state != MG_PROOF_RESPONDED) {
            send_error(p, mg_command_token_proof, mg_error_code_invalid_value);
            return;
        }
        if (n < 1 + MG_TOKEN_PROOF_MAC_LEN) {
            send_error(p, mg_command_token_proof, mg_error_code_invalid_length);
            return;
        }
        bool matched = mg_token_proof_equal(a + 1, p->proof.client_mac, MG_TOKEN_PROOF_MAC_LEN);
        proof_reset(p);   /* one confirm per challenge */
        p->proof.state = matched ? MG_PROOF_MATCHED : MG_PROOF_IDLE;
        send_proof_result(p, matched);
        return;
    }
    case mg_token_proof_clear:
        if (p->proof.state != MG_PROOF_MATCHED) {
            send_error(p, mg_command_token_proof, mg_error_code_proof_required);
            return;
        }
        send_proof_result(p, true);   /* answered first: the clear may end the connection */
        proof_reset(p);
        p->ops.proof_clear(p->ops.ctx);
        return;
    default:
        send_error(p, mg_command_token_proof, mg_error_code_unsupported);
        return;
    }
}

bool mg_proto_proof_matched(const mg_proto_t *p)
{
    return p->proof.state == MG_PROOF_MATCHED;
}

/* ---- offline clips ------------------------------------------------------ */

static void queue_status(mg_proto_t *p)
{
    mg_queue_t *q = p->cfg.queue;
    uint8_t m[14] = { mg_command_audio_queue, mg_audio_queue_command_status, p->settings.queue_enabled };
    put32(m + 3, mg_queue_used(q));
    put32(m + 7, mg_queue_capacity(q));
    put16(m + 11, q->count);
    send_control(p, m, 13);
}

static void cmd_audio_queue(mg_proto_t *p, const uint8_t *a, size_t n)
{
    mg_queue_t *q = p->cfg.queue;
    if (!q) {
        send_error(p, mg_command_audio_queue, mg_error_code_unsupported);
        return;
    }
    if (n < 1) {
        send_error(p, mg_command_audio_queue, mg_error_code_invalid_length);
        return;
    }
    switch (a[0]) {
    case mg_audio_queue_command_status:
        queue_status(p);
        return;
    case mg_audio_queue_command_clear:
        if (p->transfer) {
            p->transfer = false;
        }
        if (!mg_queue_clear(q)) {
            send_error(p, mg_command_audio_queue, mg_error_code_busy);
            return;
        }
        queue_status(p);
        return;
    case mg_audio_queue_command_clip_info:
    case mg_audio_queue_command_read_clip: {
        if (n < 3) {
            send_error(p, mg_command_audio_queue, mg_error_code_invalid_length);
            return;
        }
        uint16_t idx = le16(a + 1);
        const mg_clip_t *c = mg_queue_clip(q, idx);
        if (!c) {
            send_error(p, mg_command_audio_queue, mg_error_code_invalid_value);
            return;
        }
        if (a[0] == mg_audio_queue_command_clip_info) {
            uint8_t m[14] = { mg_command_audio_queue, mg_audio_queue_command_clip_info };
            put16(m + 2, idx);
            m[4] = c->codec;
            put32(m + 5, c->start_ms);
            put32(m + 9, c->bytes);
            send_control(p, m, 13);
            return;
        }
        if (p->live) {
            /* Live push-to-talk wins over a download. */
            send_error(p, mg_command_audio_queue, mg_error_code_busy);
            return;
        }
        p->data_type = mg_data_type_audio_queue;
        send_data_type(p);
        p->transfer = true;
        p->transfer_index = idx;
        p->transfer_off = 0;
        return;
    }
    default:
        send_error(p, mg_command_audio_queue, mg_error_code_unsupported);
        return;
    }
}

/* ---- throughput test ----------------------------------------------------- */

static void put_le32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

void mg_tp_fill(uint8_t *buf, size_t len, uint32_t seq)
{
    if (len < 4) {
        return;
    }
    put_le32(buf, seq);
    for (size_t i = 4; i < len; i++) {
        buf[i] = (uint8_t)(seq + i);
    }
}

bool mg_tp_account(mg_tp_count_t *c, const uint8_t *buf, size_t len, uint32_t now_ms)
{
    if (len < 4) {
        return false;
    }
    uint32_t seq = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
    if (c->packets == 0) {
        c->first_ms = now_ms;
        c->next_seq = seq;
    }
    if (seq > c->next_seq) {
        c->gaps += seq - c->next_seq;
    }
    if (seq >= c->next_seq) {
        c->next_seq = seq + 1;
    }
    c->bytes += len;
    c->packets++;
    c->last_ms = now_ms;
    return true;
}

size_t mg_tp_report(const mg_tp_count_t *c, uint8_t out[19])
{
    out[0] = mg_command_device_action;
    out[1] = mg_device_action_throughput_test;
    out[2] = mg_throughput_test_stop;
    put_le32(&out[3], c->bytes);
    put_le32(&out[7], c->packets);
    put_le32(&out[11], c->packets ? c->last_ms - c->first_ms : 0);
    put_le32(&out[15], c->gaps);
    return 19;
}

/* Ends the running test; with report, tells the client. */
static void tp_finish(mg_proto_t *p, bool report)
{
    if (p->tp.mode) {
        p->tp.last = p->tp.mode;
    }
    p->tp.mode = 0;
    p->tp.armed = false;
    if (report) {
        uint8_t m[19];
        send_control(p, m, mg_tp_report(&p->tp.c, m));
    }
}

static bool tp_expired(const mg_proto_t *p)
{
    return p->tp.duration_ms && now_ms(p) - p->tp.start_ms >= p->tp.duration_ms;
}

static void cmd_device_action(mg_proto_t *p, const uint8_t *a, size_t n)
{
    if (n < 1) {
        send_error(p, mg_command_device_action, mg_error_code_invalid_length);
        return;
    }
    if (a[0] != mg_device_action_throughput_test) {
        send_error(p, mg_command_device_action, mg_error_code_unsupported);
        return;
    }
    if (n < 2) {
        send_error(p, mg_command_device_action, mg_error_code_invalid_length);
        return;
    }
    uint8_t dir = a[1];
    if (dir == mg_throughput_test_stop) {
        tp_finish(p, true);
        return;
    }
    if (dir != mg_throughput_test_send && dir != mg_throughput_test_receive) {
        send_error(p, mg_command_device_action, mg_error_code_invalid_value);
        return;
    }
    mg_play_t *pl = p->cfg.play;
    bool playing = pl && (pl->state == MG_PLAY_PREBUFFER || pl->state == MG_PLAY_PLAYING);
    if (p->tp.mode || p->live || p->audio_claimed || playing) {
        send_error(p, mg_command_device_action, mg_error_code_busy);
        return;
    }
    memset(&p->tp.c, 0, sizeof(p->tp.c));
    p->tp.mode = dir;
    p->tp.armed = false;
    p->tp.seq = 0;
    p->tp.start_ms = now_ms(p);
    p->tp.duration_ms = n >= 4 ? 1000u * (uint32_t)(a[2] | a[3] << 8) : 0;
    if (dir == mg_throughput_test_send) {
        p->transfer = false;
        uint8_t m[2] = { mg_command_change_data_type, mg_data_type_throughput_test };
        send_control(p, m, sizeof(m));
        if (p->ops.play_kick) {
            p->ops.play_kick(p->ops.ctx);   /* wakes the worker to pump */
        }
    }
}

/* The send test: as many packets as the stack takes now. */
static bool tp_pump(mg_proto_t *p)
{
    uint8_t pkt[256];
    size_t n = mg_proto_data_payload(p);
    if (n > sizeof(pkt)) {
        n = sizeof(pkt);
    }
    for (int i = 0; i < 16 && p->tp.mode == mg_throughput_test_send; i++) {
        if (tp_expired(p) || n < 4) {
            tp_finish(p, true);
            break;
        }
        mg_tp_fill(pkt, n, p->tp.seq);
        if (!send_data(p, pkt, n)) {
            break;   /* the stack is full: next time round */
        }
        uint32_t t = now_ms(p);
        if (p->tp.c.packets == 0) {
            p->tp.c.first_ms = t;
        }
        p->tp.c.bytes += n;
        p->tp.c.packets++;
        p->tp.c.last_ms = t;
        p->tp.seq++;
    }
    return p->tp.mode == mg_throughput_test_send;
}

bool mg_proto_transfer_active(const mg_proto_t *p)
{
    return p->transfer || p->tp.mode == mg_throughput_test_send;
}

bool mg_proto_pump(mg_proto_t *p)
{
    if (p->tp.mode == mg_throughput_test_send) {
        return tp_pump(p);
    }
    if (!p->transfer || !p->cfg.queue) {
        return false;
    }
    const mg_clip_t *c = mg_queue_clip(p->cfg.queue, p->transfer_index);
    if (!c || p->transfer_off >= c->bytes) {
        p->transfer = false;
        return false;
    }
    uint8_t chunk[256];
    size_t n = mg_proto_data_payload(p);
    if (n > sizeof(chunk)) {
        n = sizeof(chunk);
    }
    if (n > c->bytes - p->transfer_off) {
        n = c->bytes - p->transfer_off;
    }
    if (!n || !mg_queue_read(p->cfg.queue, p->transfer_index, p->transfer_off, chunk, n)) {
        p->transfer = false;
        return false;
    }
    if (send_data(p, chunk, n)) {
        p->transfer_off += n;
    }
    if (p->transfer_off >= c->bytes) {
        p->transfer = false;
    }
    return p->transfer;
}

void mg_proto_tick(mg_proto_t *p)
{
    if (p->tp.mode == mg_throughput_test_receive && tp_expired(p)) {
        tp_finish(p, true);
    }
}

/* ---- commands ----------------------------------------------------------- */

static void cmd_start_mic(mg_proto_t *p, const uint8_t *a, size_t n)
{
    uint8_t codec = 0;
    if (n >= 1) {
        if (!mg_codec_supported(a[0])) {
            send_error(p, mg_command_start_mic, mg_error_code_invalid_value);
            return;
        }
        codec = p->start_mic_codec = a[0];
    }
    if (p->tp.mode) {
        send_error(p, mg_command_start_mic, mg_error_code_busy);
        return;
    }
    p->transfer = false;   /* live audio wins over a download */
    if (p->ops.capture) {
        p->ops.capture(p->ops.ctx, true, codec ? codec : mg_proto_capture_codec(p));
    }
}

static void command(mg_proto_t *p, const uint8_t *d, size_t n)
{
    if (!n) {
        return;
    }
    uint8_t cmd = d[0];
    const uint8_t *a = d + 1;
    size_t an = n - 1;


    switch (cmd) {
    case mg_command_request_status:
        send_data_type(p);
        send_features(p);
        break;
    case mg_command_start_mic:
        cmd_start_mic(p, a, an);
        break;
    case mg_command_stop_mic:
        if (p->ops.capture) {
            p->ops.capture(p->ops.ctx, false, 0);
        }
        break;
    case mg_command_set_mic_gain:
        if (an < 1) {
            send_error(p, cmd, mg_error_code_invalid_length);
        } else if (a[0] < 1 || a[0] > 100) {
            send_error(p, cmd, mg_error_code_invalid_value);
        } else if (p->ops.mic_gain) {
            p->ops.mic_gain(p->ops.ctx, a[0]);
        }
        break;
    case mg_command_get_settings:
        cmd_get_settings(p, a, an);
        break;
    case mg_command_set_settings:
        cmd_set_settings(p, a, an);
        break;
    case mg_command_audio_queue:
        cmd_audio_queue(p, a, an);
        break;
    case mg_command_stream_audio:
        cmd_stream_audio(p, a, an);
        break;
    case mg_command_token_proof:
        cmd_token_proof(p, a, an);
        break;
    case mg_command_device_action:
        cmd_device_action(p, d + 1, n - 1);
        break;
    case mg_command_change_data_type:
        /* From a client only for the receive throughput test. */
        if (n >= 2 && d[1] == mg_data_type_throughput_test && p->tp.mode == mg_throughput_test_receive) {
            p->tp.armed = true;
        } else {
            send_error(p, cmd, mg_error_code_unsupported);
        }
        break;
    case mg_command_assistant_state:
        if (!p->cfg.assistant) {
            send_error(p, cmd, mg_error_code_unsupported);
        } else if (an < 1) {
            send_error(p, cmd, mg_error_code_invalid_length);
        } else if (a[0] != mg_assistant_state_idle && a[0] != mg_assistant_state_thinking
                   && a[0] != mg_assistant_state_responding && a[0] != mg_assistant_state_done
                   && a[0] != mg_assistant_state_error) {
            send_error(p, cmd, mg_error_code_invalid_value);
        } else {
            p->reports_state = true;
            if (p->ops.assistant_state) {
                p->ops.assistant_state(p->ops.ctx, a[0]);   /* no reply */
            }
        }
        break;
    default:
        send_error(p, cmd, mg_error_code_unsupported);
        break;
    }
}

void mg_proto_write(mg_proto_t *p, mg_ch_t ch, const uint8_t *data, size_t len)
{
    if (!p->connected) {
        return;
    }
    switch (ch) {
    case MG_CH_CONTROL:
        command(p, data, len);
        break;
    case MG_CH_DATA:
        data_in(p, data, len);   /* stream audio, or throughput test data */
        break;
    default:
        break;
    }
}

/* ---- the audio side ----------------------------------------------------- */

bool mg_proto_ready(const mg_proto_t *p)
{
    if (!p->connected) {
        return false;
    }
    return p->sub[MG_CH_CONTROL] && p->sub[MG_CH_DATA];
}

mg_link_t mg_proto_link(mg_proto_t *p, bool set_up)
{
    if (mg_proto_ready(p)) {
        p->ever_ready = true;
        return p->ptt_enabled ? MG_LINK_READY : MG_LINK_SESSION;
    }
    if (p->connected) {
        return MG_LINK_CONNECTING;
    }
    return p->ever_ready || set_up ? MG_LINK_DISCONNECTED : MG_LINK_NEVER;
}

const char *mg_link_name(mg_link_t link)
{
    switch (link) {
    case MG_LINK_NEVER: return "never connected";
    case MG_LINK_DISCONNECTED: return "disconnected";
    case MG_LINK_CONNECTING: return "connecting";
    case MG_LINK_SESSION: return "app connected";
    default: return "ready";
    }
}

bool mg_proto_ptt_enabled(const mg_proto_t *p)
{
    return p->ptt_enabled;
}

bool mg_proto_haptics(const mg_proto_t *p)
{
    return p->cfg.haptics && p->haptics;
}

uint8_t mg_proto_capture_codec(const mg_proto_t *p)
{
    return p->start_mic_codec ? p->start_mic_codec : p->settings.codec;
}

size_t mg_proto_data_payload(const mg_proto_t *p)
{
    size_t n = p->mtu > 3 ? (size_t)p->mtu - 3 : 0;
    return n > 256 ? 256 : n;
}

bool mg_proto_gesture(mg_proto_t *p, uint8_t gesture)
{
    if (!mg_proto_ready(p)) {
        return false;
    }
    uint8_t m[2] = { mg_command_gesture, gesture };
    return send_control(p, m, sizeof(m));
}

bool mg_proto_live_begin(mg_proto_t *p, uint8_t codec)
{
    if (!mg_proto_ready(p) || !mg_codec_supported(codec)) {
        return false;
    }
    p->transfer = false;
    if (p->tp.mode == mg_throughput_test_send) {
        tp_finish(p, true);   /* push-to-talk wins over a test */
    }
    p->data_type = codec;
    p->live = true;
    send_data_type(p);
    return true;
}

bool mg_proto_live(const mg_proto_t *p)
{
    return p->live;
}

bool mg_proto_send_data(mg_proto_t *p, const uint8_t *data, size_t len)
{
    return p->live && mg_proto_ready(p) && send_data(p, data, len);
}

void mg_proto_live_end(mg_proto_t *p, bool notify_stop)
{
    if (!p->live) {
        return;
    }
    p->live = false;
    if (notify_stop && mg_proto_ready(p)) {
        uint8_t m = mg_command_stop_mic;
        send_control(p, &m, 1);
    }
}
