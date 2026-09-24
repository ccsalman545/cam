#ifndef MEDIA_AU_RING_H
#define MEDIA_AU_RING_H

/*
 * au_ring.h
 *
 * Single producer, single consumer ring of encoded H.264
 * access units. The producer (encoder thread) overwrites the
 * oldest slot when full: live video always prefers freshness
 * over completeness. Because a lost access unit breaks the
 * prediction chain, the next one popped after a loss carries
 * 'discontinuity' so the consumer can ask for a keyframe.
 *
 * The ring owns an eventfd that becomes readable on every push,
 * so the network thread can sleep in poll() and still send a
 * frame the moment it is encoded.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct AuRing AuRing;

typedef struct {
    uint64_t pts_us;
    uint64_t sequence;
    int is_idr;
    int discontinuity;      /* access units were lost before this one */
    size_t size;
} AuMeta;

AuRing *au_ring_create(size_t slot_capacity, size_t slot_count);

/*
 * Push one access unit. Returns 0 on success, -1 when the
 * access unit does not fit into a slot at all.
 */
int au_ring_push(AuRing *ring,
                 const uint8_t *data,
                 size_t size,
                 uint64_t pts_us,
                 int is_idr);

/*
 * Pop the oldest access unit into 'buffer'. Returns 1 when an
 * access unit was copied, 0 when the ring is empty.
 */
int au_ring_pop(AuRing *ring,
                uint8_t *buffer,
                size_t buffer_capacity,
                AuMeta *meta);

/* Access units overwritten or skipped before the consumer got them. */
uint64_t au_ring_dropped(const AuRing *ring);

/*
 * Readable (POLLIN) while access units are waiting; -1 when eventfd
 * is unavailable, in which case the consumer has to poll on a timer.
 * au_ring_pop() clears it when the ring runs empty.
 */
int au_ring_fd(const AuRing *ring);

void au_ring_destroy(AuRing *ring);

#endif
