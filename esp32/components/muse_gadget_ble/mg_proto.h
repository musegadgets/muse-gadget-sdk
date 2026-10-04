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
#include "mg_link.h"
#include "mg_play.h"
#include "mgcommands.h"
#include "mg_queue.h"
#include "mg_token_proof.h"

/*
 * The device side of the musegadgets BLE protocol (protocols/mgcommands.h)
 * for one connection, with no BLE
 * stack in it: the stack glue (mg_ble.c) feeds it writes and subscription
 * changes, and it answers through mg_proto_ops_t. The host tests drive it the
 * same way. Not thread-safe: the caller serializes every call.
 */

typedef enum {
    MG_CH_CONTROL,
    MG_CH_DATA,
    MG_CH_COUNT,
} mg_ch_t;

/* Throughput test counters: the report's bytes, packets, first-to-last ms and gaps. */
typedef struct {
    uint32_t bytes, packets, gaps, next_seq;
    uint32_t first_ms, last_ms;
} mg_tp_count_t;

/* A test packet: a uint32 sequence number, then (seq + i) & 0xFF. */
void mg_tp_fill(uint8_t *buf, size_t len, uint32_t seq);
/* Counts one packet; false if it is too short to carry a sequence number. */
bool mg_tp_account(mg_tp_count_t *c, const uint8_t *buf, size_t len, uint32_t now_ms);
/* [device_action, throughput_test, 0, bytes, packets, ms, gaps]: 19 bytes. */
size_t mg_tp_report(const mg_tp_count_t *c, uint8_t out[19]);

/* The settings the device keeps across restarts. */
typedef struct {
    uint16_t buzz_freq_hz;
    uint16_t buzz_ms;
    uint8_t buzz_volume;
    uint8_t codec;          /* for device-initiated capture */
    bool queue_enabled;
} mg_settings_t;

void mg_settings_defaults(mg_settings_t *s);

typedef struct {
    void *ctx;
    /* One notification on a characteristic; false if it couldn't go. */
    bool (*notify)(void *ctx, mg_ch_t ch, const uint8_t *data, size_t len);
    void (*disconnect)(void *ctx);
    /* start_mic / stop_mic from the client. */
    void (*capture)(void *ctx, bool start, uint8_t codec);
    void (*mic_gain)(void *ctx, uint8_t gain);
    /* A persisted setting changed: store mg_proto_settings(). */
    void (*settings_changed)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
    /* Fills out with random bytes (the platform CSPRNG); false on failure. */
    bool (*random)(void *ctx, uint8_t *out, size_t n);
    /* ---- playback ---- */
    /* A stream started or stopped: wake the playback task. */
    void (*play_kick)(void *ctx);
    /*
     * mg_setting_parameter_speaker_volume, for boards with a speaker (both
     * NULL: the setting is unsupported). 0-100, 0 = speaker off; the board
     * persists it.
     */
    uint8_t (*speaker_volume)(void *ctx);
    void (*set_speaker_volume)(void *ctx, uint8_t volume);
    /* mg_command_assistant_state from the client (and idle at disconnect), for cfg.assistant. */
    void (*assistant_state)(void *ctx, uint8_t state);
    /*
     * ---- token proof (mg_command_token_proof; NULL: unsupported) ----
     * proof_key copies the proof key K that Link setup stored; false if the
     * device holds none. proof_clear erases the tokens and K and returns the
     * device to setup (it may end the connection); the protocol has already
     * answered result 1.
     */
    bool (*proof_key)(void *ctx, uint8_t k[MG_TOKEN_PROOF_KEY_LEN]);
    void (*proof_clear)(void *ctx);
} mg_proto_ops_t;

typedef struct {
    mg_queue_t *queue;           /* the offline clip queue, or NULL */
    mg_play_t *play;             /* playback (the board has a speaker), or NULL */
    bool haptics;                /* a vibration motor: haptics_enabled and ptt_buzz_* */
    bool assistant;              /* a display or status light: assistant_state */
} mg_proto_config_t;

typedef enum {
    MG_PROOF_IDLE,
    MG_PROOF_RESPONDED,          /* response sent, waiting for confirm */
    MG_PROOF_MATCHED,            /* result 1 on this connection */
} mg_proof_state_t;

typedef struct {
    mg_proto_config_t cfg;
    mg_proto_ops_t ops;
    mg_settings_t settings;

    /* ---- this connection ---- */
    bool connected;
    uint16_t mtu;
    bool sub[MG_CH_COUNT];
    bool ptt_enabled;
    bool haptics;
    uint8_t start_mic_codec;     /* codec of the last start_mic that named one, else 0 */
    uint8_t data_type;           /* the type last announced (or the default) */
    bool live;                   /* an utterance is streaming */
    bool reports_state;          /* the client has sent assistant_state */

    struct {                     /* the token proof on this connection; wiped at connect and disconnect */
        uint8_t state;           /* mg_proof_state_t */
        uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN];
        uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN];
        uint8_t client_mac[MG_TOKEN_PROOF_MAC_LEN];   /* the confirm expected */
    } proof;

    bool ever_ready;             /* a client's session has been up since boot */

    bool audio_claimed;          /* push-to-talk (or the device's own sound) has the audio path */
    uint32_t update_ms;          /* when the last buffer_update went */
    uint32_t update_avail;       /* ...and what it said */

    bool transfer;               /* a read_clip download is under way */
    uint16_t transfer_index;
    uint32_t transfer_off;

    /* device_action throughput_test (mgcommands.h, Throughput test). */
    struct {
        uint8_t mode;            /* mg_throughput_test_t: 0 none, 1 sending, 2 receiving */
        uint8_t last;            /* the direction of the last test, for a late stop */
        bool armed;              /* receive: the client's change_data_type arrived */
        uint32_t start_ms, duration_ms;
        uint32_t seq;            /* send: next sequence number */
        mg_tp_count_t c;
    } tp;

} mg_proto_t;

void mg_proto_init(mg_proto_t *p, const mg_proto_config_t *cfg, const mg_proto_ops_t *ops,
                   const mg_settings_t *settings);
const mg_settings_t *mg_proto_settings(const mg_proto_t *p);

void mg_proto_connect(mg_proto_t *p);
void mg_proto_disconnect(mg_proto_t *p);
void mg_proto_set_mtu(mg_proto_t *p, uint16_t mtu);
void mg_proto_subscribe(mg_proto_t *p, mg_ch_t ch, bool on);
void mg_proto_write(mg_proto_t *p, mg_ch_t ch, const uint8_t *data, size_t len);
/* Timeouts (the throughput test). Call about once a second. */
void mg_proto_tick(mg_proto_t *p);

/* The client's token proof matched on this connection. Nothing is gated on it. */
bool mg_proto_proof_matched(const mg_proto_t *p);

/* ---- the audio side ---- */

/* Connected with notifications on: gestures and audio may go. */
/* (mg_proto_haptics is false on a board without a vibration motor.) */
bool mg_proto_ready(const mg_proto_t *p);

/* Where the client is (mg_link.h). set_up: the gadget holds a proof key (it's been set up). */
mg_link_t mg_proto_link(mg_proto_t *p, bool set_up);
bool mg_proto_ptt_enabled(const mg_proto_t *p);
bool mg_proto_haptics(const mg_proto_t *p);
/* Codec for device-initiated capture: the last start_mic's, else the setting. */
uint8_t mg_proto_capture_codec(const mg_proto_t *p);
/* Largest Data payload per notification. */
size_t mg_proto_data_payload(const mg_proto_t *p);
bool mg_proto_gesture(mg_proto_t *p, uint8_t gesture);
/* Announces the codec (ending any clip download) and opens the utterance. */
bool mg_proto_live_begin(mg_proto_t *p, uint8_t codec);
bool mg_proto_live(const mg_proto_t *p);
/* One Data notification of whole frames. */
bool mg_proto_send_data(mg_proto_t *p, const uint8_t *data, size_t len);
/* Closes the utterance; notify_stop sends [stop_mic], which marks the end of every capture. */
void mg_proto_live_end(mg_proto_t *p, bool notify_stop);

/* ---- playback (mg_command_stream_audio) ---- */

/*
 * Push-to-talk (or another sound of the device's) takes the speaker and mic:
 * a stream in progress ends with stop_streaming drop, and start_streaming is
 * refused as busy until it's released.
 */
void mg_proto_claim_audio(mg_proto_t *p, bool claim);
/* Sends the periodic buffer_update while streaming. Call every 50 ms or so. */
void mg_proto_stream_tick(mg_proto_t *p);
/* The playback task found the stream DONE and has played it out: notify stop_streaming and go idle. */
void mg_proto_stream_finished(mg_proto_t *p);

/* A read_clip download: sends the next chunk. True while there's more. */
bool mg_proto_transfer_active(const mg_proto_t *p);
bool mg_proto_pump(mg_proto_t *p);
