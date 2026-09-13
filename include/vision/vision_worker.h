#ifndef VISION_VISION_WORKER_H
#define VISION_VISION_WORKER_H

#include <stdint.h>

#include "frame_hub.h"

typedef struct VisionWorker VisionWorker;

/* A newest-frame consumer for offline matrix/mosaic processing. The worker
 * writes <prefix>.pgm and <prefix>.obj every output_interval frames. */
VisionWorker *vision_worker_create(FrameHub *hub, uint32_t width, uint32_t height,
                                    const char *prefix, uint32_t output_interval);
int vision_worker_start(VisionWorker *worker);
void vision_worker_stop(VisionWorker *worker);
void vision_worker_join(VisionWorker *worker);
uint64_t vision_worker_frames_processed(const VisionWorker *worker);
void vision_worker_destroy(VisionWorker *worker);

#endif
