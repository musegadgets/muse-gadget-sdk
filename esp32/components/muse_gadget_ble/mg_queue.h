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

/*
 * The offline clip queue (mg_command_audio_queue): push-to-talk clips
 * recorded while no mg client is connected, kept in a flash partition until
 * a client reads them and clears the queue.
 *
 * Layout: an append-only log from offset 0. Each clip is a 16-byte header
 *   LE32 magic "MGQ1", LE32 compressed bytes, LE32 start uptime ms,
 *   u8 codec (mg_data_type_t), u8 committed (0xFF, then 0x00), 2 bytes 0xFF
 * and its codec frames, then the next header at the next 4-byte boundary. A
 * whole clip is written at once from RAM, header first; `committed` is
 * programmed last, so a clip cut off by a reset is skipped. The log ends at
 * the first offset without the magic. Sectors are erased as the log enters
 * them, so clearing only erases the first sector. There's no wrap: a full
 * queue refuses new clips until it's cleared, as does one whose last header
 * was cut off as it was being written.
 */

#define MG_QUEUE_MAX_CLIPS 64
#define MG_QUEUE_HEADER_SIZE 16

typedef struct {
    void *ctx;
    uint32_t size;     /* bytes, a multiple of sector */
    uint32_t sector;   /* erase unit */
    bool (*read)(void *ctx, uint32_t off, void *buf, size_t n);
    bool (*write)(void *ctx, uint32_t off, const void *buf, size_t n);
    bool (*erase)(void *ctx, uint32_t off, size_t n);   /* sector-aligned */
} mg_store_t;

typedef struct {
    uint32_t offset;   /* of the header */
    uint32_t bytes;
    uint32_t start_ms;
    uint8_t codec;
} mg_clip_t;

typedef struct {
    mg_store_t store;
    uint32_t end;        /* where the next header goes */
    uint32_t erased_to;  /* everything from end up to here is erased */
    uint16_t count;
    bool dirty;          /* a cut-off header ends the log: full until cleared */
    mg_clip_t clips[MG_QUEUE_MAX_CLIPS];
} mg_queue_t;

/* Scans the log. False if the store is unusable. */
bool mg_queue_mount(mg_queue_t *q, const mg_store_t *store);
uint32_t mg_queue_used(const mg_queue_t *q);
uint32_t mg_queue_capacity(const mg_queue_t *q);
/* Whether a clip of `bytes` fits. */
bool mg_queue_fits(const mg_queue_t *q, uint32_t bytes);
const mg_clip_t *mg_queue_clip(const mg_queue_t *q, uint16_t index);
/* Erases ahead for a clip of `bytes`, so the append that follows only writes:
 * the slow part, done where it holds no locks. Optional. */
bool mg_queue_prepare(mg_queue_t *q, uint32_t bytes);
bool mg_queue_append(mg_queue_t *q, uint8_t codec, uint32_t start_ms, const uint8_t *data, uint32_t bytes);
/* Reads `n` bytes of clip `index`'s data from `off`. */
bool mg_queue_read(const mg_queue_t *q, uint16_t index, uint32_t off, void *buf, size_t n);
bool mg_queue_clear(mg_queue_t *q);
