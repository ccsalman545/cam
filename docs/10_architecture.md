# Architecture

[camstream docs](README.md) / 10. Architecture &nbsp;·&nbsp; [README](../README.md)

camstream 2.0 is a single process with three long lived threads and a strictly layered data path:

```mermaid
flowchart TB
    subgraph SRC["media layer (src/media)"]
        VS["video_source.h\ninterface"]
        V4L2["v4l2_source.c"]
        TEST["test_source.c"]
        POOL["frame_pool.c"]
        HUB["frame_hub.c"]
        AUR["au_ring.c"]
        YUV["yuv_convert.c"]
        ENC["h264_encoder.c\n+ encoder_x264.c / encoder_v4l2m2m.c"]
        SW["source_worker.c"]
        EW["encoder_worker.c"]
    end

    subgraph RTC["webrtc layer (src/webrtc)"]
        ICE["ice_lite.c"]
        SDP["sdp.c"]
        DTLS["dtls_srtp.c"]
        RTP["rtp_h264.c"]
        RTCP["rtcp.c"]
        SES["webrtc_session.c"]
    end

    subgraph APP["application layer (src/app)"]
        CFG["app_config.c"]
        SRV["app_server.c"]
        UI["web_ui.c"]
    end

    APP --> RTC
    APP --> SRC
    RTC -. "sends encoded frames" .- SRC
```

Dependency rule: media never calls webrtc, webrtc never calls mongoose, only app_server composes everything.

## Threading model

| Thread | Owns | Blocking behavior |
|---|---|---|
| main (network) | Mongoose loop, all RTC sessions, RTP fan-out to every viewer | poll() with 10 ms ceiling, plus DTLS timer deadlines |
| source worker | VideoSource, hub publish | blocks in poll(DQBUF) up to 200 ms |
| encode worker | hub consumer, converter, encoder, AU ring | sleeps 2 ms when no new frame |

Shutdown: SIGINT/SIGTERM set a flag, the network loop exits, threads observe the flag within one poll period, then join and destroy in reverse creation order.

## Memory pools

Two pooled structures remove the allocator from the hot path entirely:

**frame_pool**: fixed count of pre-allocated buffers, refcounted. The hub delivers the same buffer to N consumers by reference; a buffer returns to the pool when the last consumer releases it.

```mermaid
sequenceDiagram
    participant S as source worker
    participant H as hub
    participant E as encoder worker
    participant W as consumer N
    S->>H: publish (copies once into pooled frame, refcount 1)
    H->>E: mailbox slot (refcount 2)
    H->>W: mailbox slot (refcount 2, replaces stale frame)
    E->>H: unref after encode (refcount 1)
    W->>H: unref after send (refcount 0, buffer freed to pool)
```

The hub is written for N subscribers. Version 2.0 subscribes exactly one, the
encoder worker, because the raw frames no longer travel to a viewer: only
encoded access units do.

**au_ring**: 8 slots x 512 KB for encoded access units. Producer overwrites the oldest slot under pressure: for live video, freshness beats completeness.

Keep-newest semantics apply end to end. A slow consumer sees the newest frame and never accumulates a backlog. The frame a mailbox replaces is discarded silently; overwrites in the AU ring are counted and reported as `au_dropped` in `/status`.

## Video source abstraction

`VideoSource` (include/media/video_source.h) is a small vtable: start, capture, release, close. Two implementations:

- **v4l2_source**: mmap streaming, YUYV preferred, YU12 accepted, poll() driven with 200 ms timeout for responsive shutdown. Frames are borrowed pointers into mmap buffers; the caller copies into the hub and immediately QBUFs the buffer back.
- **test_source**: clock_nanosleep paced color bars with a moving marker and frame counter. Identical YUYV contract, so the whole downstream pipeline is exercised without hardware.

## Encoder abstraction

`h264_encoder_open()` resolves the preference string (auto, hw, hw:path, sw) to a backend and returns a dispatch struct. Backends:

- **encoder_v4l2m2m**: stateful V4L2 encoder per the kernel spec. OUTPUT queue gets NV12 or YU12 frames (converted from I420 by the backend), CAPTURE queue yields Annex-B H.264. Optional controls (CBR bitrate, GOP size, repeat SPS/PPS) are applied when the driver supports them and ignored otherwise.
- **encoder_x264**: veryfast + zerolatency, constrained baseline, repeated headers, ABR with VBV, single thread for deterministic latency.

Both emit Annex-B access units with SPS/PPS in front of every IDR, which lets a viewer join mid stream (the session forces an IDR on join).

## WebRTC session object

One viewer maps to one `RtcSession`:

- its own UDP socket (port = configured base + slot)
- generated local ICE credentials, validated on every STUN check
- a `DtlsSrtp` engine (SSL object plus custom BIOs over that socket)
- an `RtpH264` packetizer with a 512 entry retransmission cache
- stats exposed through `/status`

The network thread owns sessions exclusively: no locks needed anywhere in src/webrtc.

## Web UI

`src/app/web_ui.c` embeds the complete dashboard as one C string: no external asset loading, no CDN, works on an isolated LAN. It contains the WebRTC player with `getStats()` polling, sparkline charts, the six step connection timeline, the viewer table and the server card.

WebRTC is the only media transport in this project: there is no second streaming protocol, no MJPEG, no RTSP and no raw-frame WebSocket channel. Mongoose serves the page and the WebRTC signaling (`/rtc/offer`, `/rtc/close`) over HTTP; all video goes over the WebRTC stack (ICE, DTLS, SRTP, RTP) on UDP.

---

| | | |
|---|---|---|
| **Previous**<br>start of the series | **Index**<br>[docs](README.md) | **Next**<br>[11. WebRTC internals](11_webrtc_internals.md) |
