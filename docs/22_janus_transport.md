# Janus Transport (`camstream-janus`)

This document describes the **Janus gateway transport**, a third transport
backend that streams H.264 to browsers via the [Janus WebRTC gateway](https://janus.conf.meetecho.com/)
as an **external native C process**.

## Why a third backend?

| Backend | Media path in camstream | Browser transport | Dependencies | Binary |
|---------|-------------------------|-------------------|--------------|--------|
| **native** (`build/camstream`) | ICE-lite + DTLS 1.2 + SRTP + RTP H.264 (own C implementation) | WebRTC (camstream is the peer) | `libssl-dev`, `libsrtp2-dev`, `libx264-dev` | `build/camstream` |
| **libpeer** (`build/camstream-libpeer`) | libpeer PeerConnection | WebRTC (libpeer is the peer) | cmake, git, libpeer | `build/camstream-libpeer` |
| **janus** (`build/camstream-janus`) | **Plain RTP over UDP** — no ICE, no DTLS, no SRTP in camstream | WebRTC (Janus is the peer) | none beyond libc/libpthread (Janus installed separately) | `build/camstream-janus` |

The capture, frame, encoder and vision pipelines are **identical** across all
three. Only the transport changes. In Janus mode camstream does exactly what a
RTP H.264 sender in `ffmpeg` or GStreamer does: packetize Annex-B access units
into RTP and `sendto()` them. Everything the WebRTC protocol requires
(ICE, DTLS, SRTP, SDP, offer/answer) is done by Janus.

Benefits:

- **Zero crypto/transport dependencies** in the application binary.
- Janus is a mature, widely deployed gateway: it handles **many simultaneous
  viewers** for one camera stream (the native backend is 1 viewer per process
  instance).
- **Keyframe control stays in camstream**: Janus relays viewer PLI/FIR back to
  camstream over RTCP, which raises the same `force_idr` flag the native
  backend uses, so the existing encoder idle-skip / keyframe logic is reused
  unchanged.
- Mongoose still serves the dashboard, `/status` and the browser client, so
  operations look the same as the native backend.

## Architecture

```
+---------------------------------------------------------------+
| camstream-janus (this repo, plain C)                          |
|                                                               |
|  test pattern / V4L2  -->  encoder worker (x264 / V4L2 M2M)   |
|                              |                                |
|                              v                                |
|                         AuRing (SPSC)                         |
|                              |                                |
|                              v                                |
|                    janus_rtp_sender thread                    |
|                      poll(local RTCP fd, 20 ms)               |
|                      drain AuRing                             |
|                      rtp_h264_packetize()  (RFC 6184, reused) |
|                              |                                |
|              +---------------+---------------+                |
|              |                               |                |
|   RTP H.264  v            SR (5 s)          ^ PLI/FIR        |
+--------------|-----------------------------|----------------+
               v                             |
        127.0.0.1:5004 (RTP)   127.0.0.1:5005   :5006 (RTCP in)
               |            (RTCP out)          |
+--------------v-------------------------------v----------------+
| Janus Gateway (separate native C process)                     |
|   plugin: janus.plugin.streaming, mountpoint "camstream"      |
|   binds 5004 (RTP in) / 5005 (RTCP in from source) at load    |
|                                                               |
|   HTTP :8088, WebSockets :8188                                |
+--------------+------------------------------------------------+
               |  WebRTC (ICE + DTLS + SRTP, H.264/90000)
+--------------v------------------------------------------------+
| Browser: web/janus/index.html + janus-client.js               |
|   (served by camstream Mongoose on :8080)                     |
+---------------------------------------------------------------+
```

The browser keeps **two independent connections**:

1. HTTP to camstream's Mongoose (`:8080`) for the dashboard, `/status` and the
   client script — same as the native backend.
2. WebSocket to Janus (`:8188`) for the WebRTC session — same flow as the
   official Janus `streamingtest.js` example (see below).

## Ports

| Direction | Port | Protocol | Purpose |
|-----------|------|----------|---------|
| camstream → Janus | 5004 (udp) | RTP | H.264 video (PT 96, 90 kHz clock) |
| camstream → Janus | 5005 (udp) | RTCP | Sender Reports (see *RTCP and keyframes*) |
| Janus → camstream | 5006 (udp, local) | RTCP | PLI / FIR keyframe requests |
| Janus | 8088 (tcp) | HTTP | Janus admin/data API (not used by the app) |
| Janus | 8188 (tcp) | WebSocket | Browser signaling |
| camstream | 8080 (tcp) | HTTP | Dashboard, `/status`, `/janus-client.js` |

All of these are configurable: the three camstream-side ports via CLI flags,
the Janus-side ports in the config files, the HTTP/WS ports in `janus.jcfg`.

## Components

### `src/janus/janus_rtp_sender.c` (+ `include/janus/janus_rtp_sender.h`)

A single dedicated thread:

- **poll()** the local RTCP socket (`--janus-rtcp-listen`, default 5006) with a
  20 ms timeout. The socket is **non-blocking**; the handler drains the queue
  and returns to poll(), so a stop request is always honored within ~20 ms.
- **Drain the AU ring** and packetize each access unit with the existing
  `rtp_h264.c` packetizer (single-NAL units + FU-A per RFC 6184 — no new
  packetizer was written), using a sink that `sendto()`s each complete RTP
  packet to Janus.
- **Sender Reports**: one RTCP SR every 5 s (first one immediately at start),
  built with the existing `rtcp_build_sender_report()` from `rtcp.c`.
- **Keyframe feedback**: RTCP from Janus is parsed with the existing
  `rtcp_parse()`; a PLI (FMT=1) or FIR (FMT=4) sets the shared `force_idr`
  atomic, which the encoder worker consumes exactly like the native backend.

The sender tracks stats (access units, packets, octets, PLI/FIR counts, SR
count, send errors) exposed through `/status`.

### Reused, untouched

- `src/webrtc/rtp_h264.c` — packetization (SSRC/PT/sequence/timestamp are
  configured for the Janus stream: PT 96, 90 kHz).
- `src/webrtc/rtcp.c` — SR builder + RTCP feedback parser.
- `src/media/au_ring.c`, `src/media/encoder_worker.c`, `src/media/v4l2_source.c`
  — no changes.
- `src/app/web_ui.c` — in the janus build it serves the Janus dashboard and
  client script (embedded at build time by `tools/embed_assets.c`, which turns
  `web/janus/index.html` and `web/janus/janus-client.js` into C byte arrays).

### `web/janus/janus-client.js`

A self-contained Janus WebSocket client (no external libraries) implementing
the exact flow of the official `streamingtest.js`:

1. `ws://<janus>:8188` → `create` → session id.
2. `attach` to `janus.plugin.streaming`.
3. `message {request: list}` → mountpoint list.
4. `message {request: watch, id: N}` → Janus generates the **offer**
   (the plugin-offer flow, same as the official example).
5. `RTCPeerConnection` (recvonly) → `setRemoteDescription(offer)` →
   `createAnswer` → `setLocalDescription` → on ICE gathering **complete**
   (non-trickle) send `message {body: {request: start}, jsep: {type: answer,
   sdp}}`.
6. Video via `ontrack` into the `<video>` element; `webrtcup` = streaming.
   A keepalive is sent every 20 s; loss triggers a reconnect.

The page reads `?janus=host:8188`, `?stream=<mountpoint id>` and `?stun=`
query parameters, so the same dashboard works on the Pi or a laptop.

## Configuration

### Janus gateway

Two files in `config/janus/`:

- **`janus.jcfg`** — minimal core config: listen on `0.0.0.0`, HTTP API on
  `8088`, WebSockets on `8188`.
- **`janus.plugin.streaming.jcfg`** — the mountpoint camstream feeds.
  (Filename follows Janus's real plugin-config convention.)

```ini
[mcamstream]          ; becomes mountpoint "camstream", id 1
type = rtp
id = 1
description = camstream H.264
video = yes
videoport = 5004
videortcpport = 5005
videoftype = optr
videopt = 96
videortpmap = H264/90000
videofmtp = packetization-mode=1
```

Install on the machine running Janus:

```sh
sudo cp config/janus/janus.jcfg /etc/janus/
sudo cp config/janus/janus.plugin.streaming.jcfg /etc/janus/
sudo systemctl restart janus
```

Notes:

- Janus **binds the mountpoint ports at plugin load** — restart Janus after
  changing them.
- `videofmtp` is kept minimal (`packetization-mode=1` only, no
  `profile-level-id`) so the offer stays valid for any encoder level; the
  browser reads the level from the SPS.
- If other mountpoints exist, pick an unused `id` and pass it in the URL
  (`?stream=<id>`).

### camstream

New CLI options (Janus build; the `--webrtc` flag is build-validated so a
wrong value fails fast with a helpful message):

```
--webrtc janus             transport backend (this build: janus only)
--janus-host 127.0.0.1     Janus RTP/RTCP destination host
--janus-rtp-port 5004      Janus RTP port (mountpoint videoport)
--janus-rtcp-port 5005     Janus RTCP port (mountpoint videortcpport)
--janus-rtcp-listen 5006   local port for RTCP feedback from Janus
```

The defaults match the shipped config, so on a single machine you only need:

```sh
make camstream-janus -j2
./build/camstream-janus -t -e sw              # or -e hw:/dev/video11 on a Pi
```

On a Pi with the V4L2 M2M H.264 encoder:

```sh
./build/camstream-janus -e hw:/dev/video11 -b 4000
```

### Browser

Open `http://<pi>:8080/` (the dashboard auto-connects to the Janus on the same
host) or be explicit:

```
http://<pi>:8080/?janus=<pi>:8188&stream=1
```

## RTCP and keyframes — how it works

Janus's streaming plugin relays viewer keyframe requests (PLI/FIR) to the RTP
source by sending a 12-byte RTCP PLI to the source's RTCP address — **but it
only learns that address from datagrams it receives on the mountpoint RTCP
port (5005)**. That is why the sender reports matter:

1. camstream sends an SR immediately at startup (and every 5 s after).
2. Janus sees it arrive on 5005 and records camstream's address.
3. A viewer's browser sends a PLI to Janus (e.g. after a decode error or on
   track start).
4. Janus relays a PLI to camstream's local RTCP port (5006).
5. `janus_rtp_sender` parses it and sets `force_idr`.
6. The encoder worker issues the next frame as an IDR, exactly like it does
   for the native backend's per-session IDR requests.

Without the SR, step 2 never happens and keyframe requests are silently
dropped by Janus — a new viewer would wait up to the keyframe interval
(default 2 s) for a sync point.

## Building and testing

```sh
make camstream-janus     # app binary (no ssl/srtp/x264 linking; x264 optional)
make test-janus          # unit test, no camera / no Janus required
```

`test_janus_sender` runs the real sender against a **fake Janus** (two UDP
sockets) and verifies, with synthetic access units:

- single-NAL and FU-A packetization (RFC 6184), contiguous sequence numbers,
  marker bits, 90 kHz timestamps,
- byte-exact reconstruction of a fragmented NAL from the FU fragments,
- PLI → `force_idr` (same FMT=1 layout Janus uses),
- SR contents (PT 200, SSRC, packet count) and sender stats.

```
$ make test-janus
...
65 checks, 0 failures
```

## Troubleshooting

| Symptom | Check |
|---------|-------|
| No video, dashboard shows `janus: connected`, stream `preparing` | Janus log: `journalctl -u janus -f`. The mountpoint must be loaded (check `videoport`/`videortcpport` in the log). |
| `EADDRINUSE` at startup | Port 5004/5005 already used (e.g. another Janus) or 5006 busy. Change `--janus-rtp-port`/`--janus-rtcp-port`/`--janus-rtcp-listen` **and** the matching values in the jcfg, then restart Janus. |
| Janus log says it got media but browser shows nothing | Browser console: WebSocket open to Janus? `watch` sent? answer sent only after ICE gathering completes. Try `?stun=` if NAT is involved. |
| First frame takes ~2 s | Expected if Janus hadn't sent a PLI at connect (keyframe interval). PLIs after that should be sub-second — check `/status` `janus.pli_received` increments when you reload the page. |
| PLI never arrives (`pli_received` stays 0) | The SR must be reaching Janus first (port 5005 open in the firewall — Janus only learns camstream's RTCP address from the SR datagram). |
| `make camstream-janus` says `x264: no` | No libx264 in this environment. On a Pi use `-e hw` (V4L2 M2M) or install `libx264-dev` and pass `X264_DIR=...`. |
| `/status` shows send errors | `sendto` ECONNREFUSED once at startup is normal if Janus hasn't bound 5004 yet; persistent errors mean the RTP port/host don't match the mountpoint. |

## Limitations

- **No retransmission**: the sender does not keep a send buffer and does not
  answer NACKs (Janus's stream is `sendonly`; loss is handled by the periodic
  keyframes and the viewer's PLI). This matches the zero-latency goal of the
  native backend.
- **No audio**: the mountpoint carries the H.264 video stream only.
- **Single RTP source per mountpoint**: one camstream process per mountpoint
  (multiple camstreams need multiple mountpoints/ids).
