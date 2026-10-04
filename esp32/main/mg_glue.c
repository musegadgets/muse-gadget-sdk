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

#include "mg_glue.h"

#include <stddef.h>

#include "sdkconfig.h"

#include "ble_server.h"
#include "mg_ble.h"

#if CONFIG_MUSE_ENABLED
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_voice.h"
#endif
#if CONFIG_HOMEHUB_VOICE && !CONFIG_MUSE_ENABLED
#include "voice.h"
#endif

#if CONFIG_MUSE_ENABLED
static const char *model(void) {
    return muse_board ? muse_board->name : NULL;
}

static int battery_pct(void) {
    return muse_state_power().battery_pct;   // -1 without a battery
}

static void mic_gain(uint8_t gain) {
    // set_mic_gain's 1-100 onto the codec's 0 dB..MUSE_MIC_GAIN_MAX.
    muse_settings_set_mic_gain((gain * MUSE_MIC_GAIN_MAX + 50) / 100);
}

static bool speaker_write(const int16_t *pcm, size_t frames) {
    // The mouth follows the audio, as it does for the Wi-Fi path's replies.
    muse_state_set_level(muse_settings_speaker_on() ? muse_audio_level(pcm, frames) : 0);
    return muse_audio_write(pcm, frames) == ESP_OK;   // the volume and speaker settings apply
}

// musegadgets speaker_volume is the Muse speaker setting: 0 is off, any other
// value is on at that volume (the level is kept while it's off).
static int speaker_volume(void) {
    return muse_settings_speaker_on() ? muse_settings_volume() : 0;
}

static void set_speaker_volume(uint8_t volume) {
    if (volume == 0) {
        muse_settings_set_speaker_on(false);
    } else {
        muse_settings_set_volume(volume);
        muse_settings_set_speaker_on(true);
    }
}

static void attention(void) {
    muse_state_poke();    // keeps the screen (and the codecs) on for playback
    muse_state_nudge();   // wakes the voice task for a capture request
}
#elif CONFIG_HOMEHUB_VOICE
static const char *model(void) {
    return "Home Assistant Voice PE";
}
#endif

void mg_glue_start(void) {
    static const mg_ble_platform_t platform = {
#if CONFIG_MUSE_ENABLED
        .model = model,
        .battery_pct = battery_pct,
        .mic_gain = mic_gain,
        .attention = attention,
        .speaker_write = speaker_write,
        .assistant_state = muse_voice_assistant_state,   // the face follows the turn
        .speaker_volume = speaker_volume,
        .set_speaker_volume = set_speaker_volume,
#elif CONFIG_HOMEHUB_VOICE
        .model = model,
        .assistant_state = voice_assistant_state,   // the LED ring follows the turn
#endif
        .refresh_advertising = ble_server_refresh_advertising,
    };
    ble_companion_t companion = {
        .svcs = mg_ble_services(),
        .on_gap_event = mg_ble_gap_event,
        .adv_uuid128 = mg_ble_adv_uuid128(),
        .wants_advertising = mg_ble_wants_advertising,
    };
    ble_server_set_companion(&companion);
    mg_ble_init(&platform);
}
