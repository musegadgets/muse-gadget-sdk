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

#include "mg_queue.h"

#include <string.h>

#define MAGIC 0x3151474Du   /* "MGQ1" little-endian */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t align4(uint32_t v)
{
    return (v + 3u) & ~3u;
}

bool mg_queue_mount(mg_queue_t *q, const mg_store_t *store)
{
    memset(q, 0, sizeof(*q));
    if (!store || !store->size || !store->sector || store->size % store->sector) {
        return false;
    }
    q->store = *store;
    uint32_t off = 0;
    for (;;) {
        uint8_t h[MG_QUEUE_HEADER_SIZE];
        if (off + MG_QUEUE_HEADER_SIZE > q->store.size || !q->store.read(q->store.ctx, off, h, sizeof(h))
            || le32(h) != MAGIC) {
            break;
        }
        uint32_t bytes = le32(h + 4);
        uint32_t next = align4(off + MG_QUEUE_HEADER_SIZE + bytes);
        if (bytes == 0xFFFFFFFFu || next > q->store.size || next <= off) {
            break;   /* a header cut off before its length was written */
        }
        if (h[13] == 0x00 && q->count < MG_QUEUE_MAX_CLIPS) {
            q->clips[q->count++] = (mg_clip_t){
                .offset = off, .bytes = bytes, .start_ms = le32(h + 8), .codec = h[12],
            };
        }
        off = next;
    }
    q->end = off;
    /*
     * Every sector the log enters is erased first, so the rest of the one it
     * ends in is erased too. Unless a reset cut off a header as it was being
     * written: then the queue takes no more clips until it's cleared.
     */
    uint8_t tail[MG_QUEUE_HEADER_SIZE];
    uint32_t n = q->store.size - off < sizeof(tail) ? q->store.size - off : sizeof(tail);
    if (n && q->store.read(q->store.ctx, off, tail, n)) {
        for (uint32_t i = 0; i < n; i++) {
            q->dirty |= tail[i] != 0xFF;
        }
    }
    uint32_t sec = q->store.sector;
    q->erased_to = off % sec ? (off / sec + 1) * sec : off;
    return true;
}

uint32_t mg_queue_used(const mg_queue_t *q)
{
    return q->end;
}

uint32_t mg_queue_capacity(const mg_queue_t *q)
{
    return q->store.size;
}

bool mg_queue_fits(const mg_queue_t *q, uint32_t bytes)
{
    return q->store.size && !q->dirty && q->count < MG_QUEUE_MAX_CLIPS
           && (uint64_t)q->end + MG_QUEUE_HEADER_SIZE + bytes <= q->store.size;
}

const mg_clip_t *mg_queue_clip(const mg_queue_t *q, uint16_t index)
{
    return index < q->count ? &q->clips[index] : NULL;
}

/* Erases whole sectors so that [q->end, to) is erased. */
static bool erase_to(mg_queue_t *q, uint32_t to)
{
    uint32_t sec = q->store.sector;
    while (q->erased_to < to) {
        uint32_t start = q->erased_to - q->erased_to % sec;
        if (start < q->erased_to) {
            start += sec;   /* the rest of this sector is already erased */
            q->erased_to = start;
            continue;
        }
        if (!q->store.erase(q->store.ctx, start, sec)) {
            return false;
        }
        q->erased_to = start + sec;
    }
    return true;
}

/* Where a clip of `bytes` would end, and how far it needs erasing: also where
 * the next header goes, so the log visibly ends there. */
static uint32_t clip_end(const mg_queue_t *q, uint32_t bytes, uint32_t *need)
{
    uint32_t next = align4(q->end + MG_QUEUE_HEADER_SIZE + bytes);
    *need = next + MG_QUEUE_HEADER_SIZE <= q->store.size ? next + MG_QUEUE_HEADER_SIZE : q->store.size;
    return next;
}

bool mg_queue_prepare(mg_queue_t *q, uint32_t bytes)
{
    if (!bytes || !mg_queue_fits(q, bytes)) {
        return false;
    }
    uint32_t need;
    clip_end(q, bytes, &need);
    return erase_to(q, need);
}

bool mg_queue_append(mg_queue_t *q, uint8_t codec, uint32_t start_ms, const uint8_t *data, uint32_t bytes)
{
    if (!bytes || !mg_queue_fits(q, bytes)) {
        return false;
    }
    uint32_t off = q->end;
    uint32_t need;
    uint32_t next = clip_end(q, bytes, &need);
    if (!erase_to(q, need)) {
        return false;
    }
    uint8_t h[MG_QUEUE_HEADER_SIZE];
    memset(h, 0xFF, sizeof(h));
    put32(h, MAGIC);
    put32(h + 4, bytes);
    put32(h + 8, start_ms);
    h[12] = codec;
    uint8_t committed = 0x00;
    if (!q->store.write(q->store.ctx, off, h, sizeof(h))
        || !q->store.write(q->store.ctx, off + MG_QUEUE_HEADER_SIZE, data, bytes)) {
        q->dirty = true;   /* don't write over it; full until cleared */
        return false;
    }
    if (!q->store.write(q->store.ctx, off + 13, &committed, 1)) {
        q->end = next;   /* uncommitted: skipped on the next mount */
        return false;
    }
    q->end = next;
    q->clips[q->count++] = (mg_clip_t){ .offset = off, .bytes = bytes, .start_ms = start_ms, .codec = codec };
    return true;
}

bool mg_queue_read(const mg_queue_t *q, uint16_t index, uint32_t off, void *buf, size_t n)
{
    const mg_clip_t *c = mg_queue_clip(q, index);
    if (!c || off > c->bytes || n > c->bytes - off) {
        return false;
    }
    return q->store.read(q->store.ctx, c->offset + MG_QUEUE_HEADER_SIZE + off, buf, n);
}

bool mg_queue_clear(mg_queue_t *q)
{
    if (!q->store.size || !q->store.erase(q->store.ctx, 0, q->store.sector)) {
        return false;
    }
    q->end = 0;
    q->erased_to = q->store.sector;
    q->count = 0;
    q->dirty = false;
    return true;
}
