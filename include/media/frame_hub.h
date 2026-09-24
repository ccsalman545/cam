/*
 * frame_hub.h
 *
 * Frame distribution between producer and consumer threads.
 *
 * Design:
 *   - One producer (the source worker thread) publishes frames.
 *   - N consumers (the encoder thread today) each own a single
 *     slot mailbox.
 *   - Mailbox semantics are keep-newest: if a consumer has not
 *     taken the previous frame yet, the previous frame is
 *     dropped and replaced. A slow consumer therefore never
 *     builds a backlog and never stalls the producer. Live
 *     video always prefers the freshest frame over queued
 *     stale ones.
 *
 * Frame data is copied exactly once, from the capture buffer
 * into one pooled frame, and reference counted to all
 * subscribers.
 */
#ifndef MEDIA_FRAME_HUB_H
#define MEDIA_FRAME_HUB_H

#include <stddef.h>
#include <stdint.h>

#include "frame_pool.h"

typedef struct FrameHub FrameHub;
typedef struct FrameHubConsumer FrameHubConsumer;

/*
 * Create a hub with an internal pool of 'pool_count' buffers
 * of 'buffer_capacity' bytes each.
 */
FrameHub *frame_hub_create(size_t buffer_capacity, size_t pool_count);

/*
 * Register a consumer. The consumer starts with an empty
 * mailbox. Returns NULL on failure.
 */
FrameHubConsumer *frame_hub_subscribe(FrameHub *hub);

/*
 * Remove a consumer and drop any mailboxed frame.
 */
void frame_hub_unsubscribe(FrameHub *hub, FrameHubConsumer *consumer);

/*
 * Producer side: copy the source data into a pooled buffer and
 * deliver it to every subscribed consumer mailbox.
 * Returns 0 on success, -1 when the pool is exhausted
 * (all buffers still referenced).
 */
int frame_hub_publish(FrameHub *hub,
                      const uint8_t *data,
                      size_t size,
                      uint32_t width,
                      uint32_t height,
                      uint32_t format,
                      uint32_t stride,
                      uint64_t sequence,
                      uint64_t timestamp_us);

/*
 * Consumer side, non-blocking: take the newest frame out of
 * the mailbox, or NULL when empty. Caller receives one
 * reference and must call frame_unref(hub_pool, frame) when
 * done.
 */
Frame *frame_hub_take(FrameHubConsumer *consumer);

/*
 * As frame_hub_take(), but sleeps up to timeout_ms for a frame to
 * arrive instead of returning NULL at once.
 */
Frame *frame_hub_take_wait(FrameHubConsumer *consumer, int timeout_ms);

/*
 * Access the hub pool for frame_unref().
 */
FramePool *frame_hub_pool(FrameHub *hub);

void frame_hub_destroy(FrameHub *hub);

#endif
