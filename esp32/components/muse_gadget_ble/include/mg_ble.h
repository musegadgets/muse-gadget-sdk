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

#include "mg_link.h"
#include "mg_token_proof.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * musegadgets BLE server (CONFIG_MUSE_GADGET_BLE_AUDIO).
 *
 * It doesn't own the BLE stack: Home Link's server (main/ble_server.c) does,
 * and this registers with it as a companion, the way Muse's phone setup
 * service does. Its services join the shared GATT database, it sees every GAP
 * event, and Link's server advertises its UUID for it (taking turns with
 * Link's own setup advertising while both want to be found). One central at
 * a time: while an mg client is connected nothing else can connect.
 *
 * Services: musegadgets (Control, Data; with CONFIG_MUSE_GADGET_BLE_SECURE
 * also Encrypted Control and Encrypted Data), Battery, Device Information,
 * Nordic UART (log mirror, CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS).
 */

/* What the board supplies. Any callback may be NULL. */
typedef struct {
    const char *(*model)(void);          /* DIS model number: the board's name */
    /*
     * The token proof (mg_command_token_proof). proof_key copies the proof
     * key K that Link setup stored; false if there's none (not set up).
     * proof_clear erases the tokens and K and returns the board to Link
     * setup, without blocking (it may restart the board). Both NULL: the
     * proof isn't offered.
     */
    bool (*proof_key)(uint8_t k[MG_TOKEN_PROOF_KEY_LEN]);
    void (*proof_clear)(void);
    int (*battery_pct)(void);            /* 0-100, or -1 without a battery */
    void (*mic_gain)(uint8_t gain);      /* set_mic_gain, 1-100 */
    bool display;                        /* can show a pairing code: offer numeric comparison */
    void (*attention)(void);             /* wake the screen: a pairing prompt, capture request or playback */
    void (*refresh_advertising)(void);   /* the host server re-reads mg_ble_wants_advertising() */
    /*
     * Plays 16 kHz mono PCM, blocking at the speaker's pace (the board's
     * volume and mute settings apply). NULL: no speaker, so stream_audio
     * isn't offered.
     */
    bool (*speaker_write)(const int16_t *pcm, size_t frames);
    /*
     * The speaker level (mg_setting_parameter_speaker_volume): 0-100, 0 when
     * the speaker is off. set turns it off at 0, else on at that volume, and
     * persists it. Both NULL: the setting is unsupported.
     */
    int (*speaker_volume)(void);
    void (*set_speaker_volume)(uint8_t volume);
    /*
     * A vibration motor: plays the push-to-talk activation buzz. NULL (no
     * Muse board has one yet): haptics_enabled and ptt_buzz_* are
     * unsupported and there's no buzz.
     */
    void (*vibrate)(uint16_t freq_hz, uint16_t ms, uint8_t volume_pct);
    /*
     * A display or status light that follows the client's assistant_state
     * (mg_assistant_state_t; idle when the client disconnects). NULL: the
     * command isn't offered. Called from the BLE worker: don't block.
     */
    void (*assistant_state)(uint8_t state);
} mg_ble_platform_t;

struct ble_gatt_svc_def;
struct ble_gap_event;

/* ---- hooks for the host BLE server (set before it starts) ---- */
const struct ble_gatt_svc_def *mg_ble_services(void);
int mg_ble_gap_event(struct ble_gap_event *event);
/* The musegadgets service UUID, little-endian, for advertising data. */
const uint8_t *mg_ble_adv_uuid128(void);
/* Whether it wants to be advertised now (initialized, nobody connected). */
bool mg_ble_wants_advertising(void);

/* After NVS is up, before the host server starts. */
void mg_ble_init(const mg_ble_platform_t *platform);

/* Link setup stored or erased the proof key: re-reads whether the gadget is set up. */
void mg_ble_setup_changed(void);

/* ---- pairing (session security) ---- */
typedef struct {
    uint8_t method;   /* mg_pairing_method_t; 0: nothing to show */
    uint32_t code;    /* for numeric comparison, 0..999999 */
} mg_ble_prompt_t;

/* True while a pairing waits for the user; fills *out. */
bool mg_ble_pairing_prompt(mg_ble_prompt_t *out);
/* A press of the talk button. True if it answered a pairing prompt (swallow it). */
bool mg_ble_confirm_press(void);
/* The user asked to pair a new client (pairing mode for two minutes). */
void mg_ble_open_pairing_window(void);

/* An mg client is connected (not necessarily ready for audio). */
bool mg_ble_connected(void);
/* ...and ready for audio, with push-to-talk on: a press goes to it (mg_ble_link() is MG_LINK_READY). */
bool mg_ble_ptt_ready(void);
/* Where the client is, for the screen: kept current on every BLE event and command. */
mg_link_t mg_ble_link(void);

#ifdef __cplusplus
}
#endif
