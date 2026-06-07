/*
 * Portable cross-core primitives for the HOJA dongle host library.
 *
 * Copyright (c) 2026 Hand Held Legend, LLC
 * Author: Mitchell Cairns
 *
 * SPDX-License-Identifier: MIT-0
 */

/**
 * @file dongle_crosscore.h
 * @brief Self-contained SPSC snapshot + FIFO templates used by dongle_host.c.
 *
 * The dongle host splits its work across two tasks (a "wlan" radio task and a
 * "transport" console task) that a platform may pin to two different cores. All
 * state shared between those tasks is passed through one of two strict
 * single-producer / single-consumer (SPSC) primitives generated here:
 *
 *   - DONGLE_CROSSCORE_SNAPSHOT_TYPE(name, type): a seqlock "latest value"
 *     cell. Use it when only the most recent value matters and intermediate
 *     updates may be coalesced (status, session, link, latest input).
 *
 *   - DONGLE_CROSSCORE_FIFO_TYPE(name, type, len): a lock-free ring buffer.
 *     Use it when every element must be preserved in order (reliable lanes).
 *
 * These are intentionally dependency-free (C11 <stdatomic.h> only) so the host
 * library stays portable: on a single-core platform the producer and consumer
 * are simply the same core and the fences become no-ops. The implementation is
 * deliberately equivalent to the firmware's own crosscore_*.h utilities so the
 * dongle host behaves identically whether built standalone or inside the app.
 *
 * Macro contract (identical for both primitives):
 *   - EXACTLY ONE task/core may call the *_write / *_push (the producer).
 *   - EXACTLY ONE task/core may call the *_read  / *_pop  (the consumer).
 *   - `len` for a FIFO MUST be a power of two.
 */

#ifndef HOJA_LIB_DONGLE_CROSSCORE_H
#define HOJA_LIB_DONGLE_CROSSCORE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Seqlock-style "latest value" snapshot                                      */
/* -------------------------------------------------------------------------- */
/* Generates dongle_snapshot_<name>_t plus inline read/write helpers. The seq
 * counter is odd while a write is in progress and even when stable; release /
 * acquire fences order the payload copy against the counter so a torn read is
 * detected and the last fully published value (stale_data) is returned instead
 * of a partial one. The consumer therefore always gets a usable value. */
#define DONGLE_CROSSCORE_SNAPSHOT_TYPE(name, type)                              \
typedef struct {                                                               \
    atomic_uint seq;        /* odd while writing, even when stable */          \
    type        data;       /* live value being published */                   \
    type        stale_data; /* last fully published value (torn-read fallback)*/\
} dongle_snapshot_##name##_t;                                                  \
                                                                               \
/* PRODUCER ONLY. */                                                           \
static inline void dongle_snapshot_##name##_write(                             \
    dongle_snapshot_##name##_t *s, const type *src)                            \
{                                                                              \
    unsigned int seq0 = atomic_load_explicit(&s->seq, memory_order_relaxed);   \
    atomic_store_explicit(&s->seq, seq0 + 1, memory_order_relaxed);            \
    atomic_thread_fence(memory_order_release);                                 \
    memcpy(&s->data, src, sizeof(type));                                       \
    atomic_thread_fence(memory_order_release);                                 \
    atomic_store_explicit(&s->seq, seq0 + 2, memory_order_release);            \
    memcpy(&s->stale_data, src, sizeof(type));                                 \
}                                                                              \
                                                                               \
/* CONSUMER ONLY. Returns true for a fresh read, false (and copies stale_data) \
 * if a write was in progress or began during the read. */                     \
static inline bool dongle_snapshot_##name##_read(                              \
    dongle_snapshot_##name##_t *s, type *dst)                                  \
{                                                                              \
    unsigned int s1 = atomic_load_explicit(&s->seq, memory_order_acquire);     \
    if (s1 & 1u) {                                                             \
        memcpy(dst, &s->stale_data, sizeof(type));                             \
        return false;                                                          \
    }                                                                          \
    memcpy(dst, &s->data, sizeof(type));                                       \
    atomic_thread_fence(memory_order_acquire);                                 \
    unsigned int s2 = atomic_load_explicit(&s->seq, memory_order_acquire);     \
    if (s1 != s2 || (s2 & 1u)) {                                              \
        memcpy(dst, &s->stale_data, sizeof(type));                             \
        return false;                                                          \
    }                                                                          \
    return true;                                                               \
}

/* -------------------------------------------------------------------------- */
/* Lock-free SPSC ring buffer FIFO                                            */
/* -------------------------------------------------------------------------- */
/* Generates dongle_fifo_<name>_t plus inline push/pop helpers. There is no
 * shared count (which would need a cross-core read-modify-write the Cortex-M0+
 * lacks): the producer owns `tail`, the consumer owns `head`, and fullness /
 * emptiness is derived from the free-running counters with wrap-safe unsigned
 * subtraction. `len` MUST be a power of two. */
#define DONGLE_CROSSCORE_FIFO_TYPE(name, type, len)                            \
typedef struct {                                                               \
    type        buf[len];                                                      \
    atomic_uint head; /* written ONLY by the consumer */                       \
    atomic_uint tail; /* written ONLY by the producer */                       \
} dongle_fifo_##name##_t;                                                      \
                                                                               \
/* PRODUCER ONLY. Returns false (drops the element) when full. */              \
static inline bool dongle_fifo_##name##_push(                                  \
    dongle_fifo_##name##_t *f, const type *src)                                \
{                                                                              \
    unsigned int t = atomic_load_explicit(&f->tail, memory_order_relaxed);     \
    unsigned int h = atomic_load_explicit(&f->head, memory_order_acquire);     \
    if ((unsigned int)(t - h) >= (len)) return false;                          \
    memcpy(&f->buf[t & ((len) - 1u)], src, sizeof(type));                      \
    atomic_store_explicit(&f->tail, t + 1u, memory_order_release);             \
    return true;                                                               \
}                                                                              \
                                                                               \
/* CONSUMER ONLY. Returns false when empty. */                                 \
static inline bool dongle_fifo_##name##_pop(                                   \
    dongle_fifo_##name##_t *f, type *dst)                                      \
{                                                                              \
    unsigned int h = atomic_load_explicit(&f->head, memory_order_relaxed);     \
    unsigned int t = atomic_load_explicit(&f->tail, memory_order_acquire);     \
    if (h == t) return false;                                                  \
    memcpy(dst, &f->buf[h & ((len) - 1u)], sizeof(type));                      \
    atomic_store_explicit(&f->head, h + 1u, memory_order_release);             \
    return true;                                                               \
}

#endif /* HOJA_LIB_DONGLE_CROSSCORE_H */
