# Optimization notes

[camstream docs](README.md) / 15. Optimization notes &nbsp;·&nbsp; [README](../README.md)

Every change from the stage 4 prototype to 2.0, with the reason and the measured or expected effect.

## Summary table

| Area | Stage 4 | 2.0 | Effect |
|---|---|---|---|
| Wire format | raw YUYV, ~139 Mbps at 640x480@30 | H.264 at 2 to 4 Mbps | 35 to 70x less bandwidth |
| Per frame heap | malloc + memcpy in queue push, malloc + 2x memcpy per WS send, free | pre-allocated pool, one copy into the pool | zero allocations per frame |
| Queue policy | capacity 3, push fails silently when full (frame loss at the head) | keep-newest mailboxes plus overwrite-oldest AU ring | latency never accumulates, drops are counted |
| Event loop | mg_mgr_poll(10) then drain queue | single poll() over session fds + mg_mgr_poll(0), DTLS aware timeout | bounded 10 ms worst case, no busy spin |
| Capture | select() 2 s timeout, blocking shutdown | poll() 200 ms, flag checked each round | shutdown within 200 ms instead of 2 s |
| WS backpressure | unbounded mg send buffer | frames dropped above 384 KB pending | viewer stalls no longer grow memory |
| Encode CPU when idle | n/a | encode worker skips conversion and encoding with no session | idle cost near zero (important on Pi) |
| Color conversion | JS per pixel in browser (YUYV to RGBA) | C two-rows-at-once scan on the server once per frame | browser CPU freed, GPU decode path used |
| Loss recovery | none | NACK cache 512 packets, PLI driven IDRs | transient loss heals in one RTT |

## Detailed rationale

### 1. Bandwidth: encode before transmit

The single largest win. Uncompressed 640x480 YUYV at 30 fps is 640 x 480 x 2 x 30 = 18.4 MB/s, about 139 Mbps, per viewer. H.264 at the same visual quality needs 2 to 4 Mbps, and the decoder runs in the browser's accelerated pipeline instead of a JS pixel loop.

### 2. Zero allocation steady state

Stage 4 allocated and copied three times per frame (queue buffer, packet assembly, WS framing). 2.0 allocates buffers exactly once at startup:

- capture buffer (mmap, borrowed) -> pooled frame (one memcpy) -> encoder scratch (conversion) -> AU slot -> UDP send buffer.

The pool uses a free list under one mutex; the hub hands the same buffer to several consumers via refcounts.

### 3. Keep-newest everywhere

Live video is only useful if it is current. When any consumer lags, 2.0 drops the stale frame and delivers the fresh one, counting the drop. Stage 4 silently refused new frames when its tiny queue was full, which effectively froze the newest state.

### 4. Single network thread for session state

All ICE, DTLS, RTP and RTCP processing happens on the main thread with a poll set: no locks, no cross thread session mutation, deterministic ordering between STUN, DTLS and RTP on the same socket.

### 5. Gated encode thread

With no WebRTC session the encode thread takes frames from its mailbox and drops them immediately: no conversion, no x264 call. The pipeline warms up within one frame period when the first viewer connects (an IDR is forced on join).

### 6. Bounded DTLS timing

The main loop computes `min(10 ms, next DTLS timeout)` so handshake retransmissions fire exactly on time without polling faster than needed.

### 7. Repeated headers for instant join

Both encoder backends repeat SPS/PPS before every IDR and every new session forces an IDR, so a viewer never waits for the next GOP start (with the default 2 s interval, worst case join delay drops from 2 s to one frame).

### 8. Retransmission without re-encryption

NACK responses replay the stored protected packet verbatim. Receivers accept identical SRTP packets for retransmission, and the server avoids a second encrypt pass under loss bursts.

## Numbers observed during development (loopback, test pattern, x264)

| Metric | Value |
|---|---|
| Encode latency 640x480 veryfast, single thread | 2 to 4 ms per frame |
| Poll loop overhead with one viewer | under 2 percent of one core |
| Server RSS at 640x480 | about 30 MB steady (pools pre-allocated) |

## Remaining known costs

- YUYV to I420 conversion is scalar C. It is memory bandwidth bound at 640x480 (about 0.5 ms) and still fine at 720p; a SIMD pass is a natural next step.
- One IDR per new viewer costs one large frame burst at the configured bitrate; acceptable on LAN, and mitigated by VBV on the software encoder.

---

| | | |
|---|---|---|
| **Previous**<br>[14. Raspberry Pi guide](14_raspberry_pi.md) | **Index**<br>[docs](README.md) | **Next**<br>[16. Protocol reference](16_protocol_reference.md) |
