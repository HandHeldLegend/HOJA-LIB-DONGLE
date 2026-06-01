#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

// Generic container for snapshot-protected data with stale value support
#define DONGLE_CROSSCORE_SNAPSHOT_TYPE(name, type)        \
typedef struct {                         \
    atomic_uint seq;   /**< Sequence: odd while writing, even when stable */ \
    type data;         /**< Live value being published by the writer */ \
    type stale_data;   /**< Last fully published value, returned on torn reads */ \
} dongle_snapshot_##name##_t;                   \
                                         \
/* PRODUCER-CORE ONLY. Publishes a new value: bump seq odd, store payload,    \
 * bump seq even, then refresh the stale copy for future torn reads. */        \
static inline void dongle_snapshot_##name##_write(dongle_snapshot_##name##_t *s, const type *src) { \
    unsigned int seq0 = atomic_load_explicit(&s->seq, memory_order_relaxed); \
    atomic_store_explicit(&s->seq, seq0 + 1, memory_order_relaxed); /* odd = writing */ \
    atomic_thread_fence(memory_order_release);                       \
    memcpy(&s->data, src, sizeof(type));                             \
    atomic_thread_fence(memory_order_release);                       \
    atomic_store_explicit(&s->seq, seq0 + 2, memory_order_release); /* even = stable */ \
    /* Update stale copy after write is complete */                  \
    memcpy(&s->stale_data, src, sizeof(type));                       \
}                                                                    \
                                                                     \
/* CONSUMER-CORE ONLY. Copies the latest value into @p dst. Returns true for a \
 * fresh, consistent read; returns false and copies stale_data if a write was   \
 * in progress or began during the read. */                                     \
static inline bool dongle_snapshot_##name##_read(dongle_snapshot_##name##_t *s, type *dst) { \
    unsigned int s1 = atomic_load_explicit(&s->seq, memory_order_acquire); \
    if (s1 & 1) {                                                    \
        /* Writer in progress - return stale value */                \
        memcpy(dst, &s->stale_data, sizeof(type));                   \
        return false; /* indicate stale read */                      \
    }                                                                \
    /* Try to get fresh value */                                     \
    memcpy(dst, &s->data, sizeof(type));                             \
    atomic_thread_fence(memory_order_acquire);                       \
    unsigned int s2 = atomic_load_explicit(&s->seq, memory_order_acquire); \
    if (s1 != s2 || (s2 & 1)) {                                      \
        /* Write happened during read - return stale value */        \
        memcpy(dst, &s->stale_data, sizeof(type));                   \
        return false; /* indicate stale read */                      \
    }                                                                \
    return true; /* indicate fresh read */                           \
}  

#define DONGLE_CROSSCORE_FIFO_TYPE(name, type, len)                            \
typedef struct {                                                               \
    type        buf[len];                                                      \
    atomic_uint head; /* free-running; written ONLY by the consumer core */    \
    atomic_uint tail; /* free-running; written ONLY by the producer core */    \
} dongle_fifo_##name##_t;                                                      \
                                                                               \
/* PRODUCER-CORE ONLY. Must never be called from the consumer core.            \
 * Returns false (and drops the element) when the FIFO is full.                \
 *   - tail is loaded relaxed (this core is its only writer).                  \
 *   - head is loaded acquire so we observe the consumer's freed slots.        \
 *   - the slot is filled BEFORE tail is published with a release store, so the \
 *     consumer can never read a slot before its data is visible.              \
 * Fullness is (tail - head) >= len using wrap-safe unsigned subtraction. */    \
static inline bool dongle_fifo_##name##_push(dongle_fifo_##name##_t *f, const type *src) {   \
    unsigned int t = atomic_load_explicit(&f->tail, memory_order_relaxed);     \
    unsigned int h = atomic_load_explicit(&f->head, memory_order_acquire);     \
    if ((unsigned int)(t - h) >= (len)) return false; /* full */               \
    memcpy(&f->buf[t & ((len) - 1u)], src, sizeof(type));                      \
    atomic_store_explicit(&f->tail, t + 1u, memory_order_release);             \
    return true;                                                               \
}                                                                              \
                                                                               \
/* CONSUMER-CORE ONLY. Must never be called from the producer core.            \
 * Returns false when the FIFO is empty.                                       \
 *   - head is loaded relaxed (this core is its only writer).                  \
 *   - tail is loaded acquire to pair with the producer's release store, so     \
 *     the slot data is guaranteed visible before we copy it out.              \
 *   - head is advanced with a release store so the producer sees the slot is   \
 *     free only after we have finished reading it. */                         \
static inline bool dongle_fifo_##name##_pop(dongle_fifo_##name##_t *f, type *dst) { \
    unsigned int h = atomic_load_explicit(&f->head, memory_order_relaxed);     \
    unsigned int t = atomic_load_explicit(&f->tail, memory_order_acquire);     \
    if (h == t) return false; /* empty */                                      \
    memcpy(dst, &f->buf[h & ((len) - 1u)], sizeof(type));                      \
    atomic_store_explicit(&f->head, h + 1u, memory_order_release);             \
    return true;                                                               \
}

// -----------------------------------------------------------------------------
// Example usage
// -----------------------------------------------------------------------------
//
// 1) Declare the FIFO type and an instance (len must be a power of two):
//
//      typedef struct { uint8_t data[64]; uint16_t len; } my_packet_s;
//      CROSSCORE_FIFO_TYPE(rx_reliable, my_packet_s, 16);
//      static fifo_rx_reliable_t _rx_reliable_fifo;
//
// 2) Producer core (and ONLY this core) pushes:
//
//      my_packet_s in;
//      memcpy(in.data, pkt->data, pkt->len);
//      in.len = pkt->len;
//      if (!fifo_rx_reliable_push(&_rx_reliable_fifo, &in)) {
//          // FIFO full: element was dropped.
//      }
//
// 3) Consumer core (and ONLY this core) pops:
//
//      my_packet_s out;
//      while (fifo_rx_reliable_pop(&_rx_reliable_fifo, &out)) {
//          // handle `out`
//      }
