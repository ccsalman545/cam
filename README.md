<div align="center">

# camstream

**A webcam-to-browser WebRTC server written in C11.**

Sub-frame latency on a LAN. No signaling server, no STUN, no TURN, no cloud,
no containers, no JavaScript build step, and no video framework — a single
static binary, one HTTP port, and a UDP socket per viewer.

[![language](https://img.shields.io/badge/language-C11-blue.svg)](#)
[![platform](https://img.shields.io/badge/platform-Linux%20%7C%20Raspberry%20Pi-lightgrey.svg)](#raspberry-pi)
[![transport](https://img.shields.io/badge/transport-WebRTC%20%7C%20ICE--lite%20%7C%20DTLS%201.2%20%7C%20SRTP-green.svg)](#the-webrtc-stack)
[![version](https://img.shields.io/badge/version-2.0.0-informational.svg)](#versioning)

</div>

---

## Contents

**Getting started** · [What this is](#what-this-is) · [Quick start](#quick-start) · [Install](#install) · [Build](#build)

**Using it** · [CLI reference](#cli-reference) · [HTTP API](#http-api) · [Web UI](#web-ui)

**Understanding it** · [Architecture](#architecture) · [The WebRTC stack](#the-webrtc-stack) · [Session lifecycle](#session-lifecycle) · [Multi-viewer](#multi-viewer-behaviour)

**Operating it** · [Raspberry Pi](#raspberry-pi) · [Acceptance tests](#acceptance-tests) · [Troubleshooting](#troubleshooting)

**Reference** · [Backends](#transport-backends) · [Janus](#janus-transport) · [Layout](#repository-layout) · [Tuning constants](#tuning-constants) · [Versioning](#versioning)

---

## What this is

camstream captures from a V4L2 device, encodes H.264, and delivers it to a
browser over **real WebRTC** — ICE, DTLS 1.2, SRTP, RTP — implemented directly
in C against OpenSSL and libsrtp2. Mongoose is used *only* to serve the viewer
page and the signaling `POST`; it carries no video.

**Why not MJPEG-over-HTTP or WebSockets?** Those buffer. TCP head-of-line
blocking turns a single lost packet into a visible stall, and every proxy in
the path adds its own buffer. WebRTC over UDP drops what it cannot deliver on
time and repairs selectively via NACK. That is the difference between "roughly
live" and *live*.

### Design commitments

| | |
|---|---|
| **Freshness over completeness** | Every queue is a keep-newest mailbox or an overwrite-oldest ring. A slow consumer never makes the pipeline lag; it skips. |
| **No allocation in the hot path** | Frame buffers are pooled and refcounted. `malloc` appears at startup and on viewer join, never per frame. |
| **Encode once, fan out N times** | One encoder feeds all viewers. Per-viewer state is only RTP sequence numbers, SRTP keys, and a retransmission cache. |
| **The main thread never blocks** | HTTP, ICE, DTLS timers, RTCP, and RTP packetization all run in one non-blocking poll loop. |
| **Auditable** | ~11k lines of C. One vendored dependency (Mongoose). Every protocol decision is commented with its RFC. |

---

## Quick start

### No camera, no hardware, 30 seconds

```sh
sudo apt install -y build-essential libssl-dev libsrtp2-dev libx264-dev
make -j"$(nproc)"
./build/camstream --test --encoder sw --listen 0.0.0.0
```

Open the URL it prints. You should see a test pattern with a scrolling clock.
This exercises the entire pipeline except the camera driver — **always start
here** when diagnosing a problem.

### Real camera

```sh
./build/camstream --device /dev/video0 --encoder auto
```

### The canonical deployment: Raspberry Pi → laptop, offline

The Pi owns the camera; the laptop runs only a browser; one Ethernet cable
between them. HTTP signaling over TCP, media over UDP. No internet, no STUN,
no TURN.

```text
┌────────────────────────┐                      ┌────────────────────────┐
│  Raspberry Pi          │   TCP 8080  signal   │  Laptop                │
│  192.168.1.10/24       │◄────────────────────►│  192.168.1.20/24       │
│  camera + camstream    │   UDP 50000+ media   │  browser only          │
└────────────────────────┘◄────────────────────►└────────────────────────┘
```

```sh
# On the Pi
./build/camstream --device /dev/video0 --encoder auto --listen 0.0.0.0

# On the laptop, open:
http://192.168.1.10:8080/
```

Open TCP `8080` and UDP `50000-50007` on the Pi. Use the Pi's LAN address —
**not** `localhost`, and **not** through a proxy (proxies cannot forward UDP).

> **Latency check.** Point the camera at a screen showing a millisecond
> stopwatch, then photograph the screen and the stream together. Glass-to-glass
> on a wired LAN at 640×480/30 should land in the low tens of milliseconds.

---

## Install

### Dependencies

| Dependency | Purpose | Required |
|---|---|---|
| C11 compiler + pthreads | — | yes |
| OpenSSL 1.1 or 3.x | DTLS 1.2, certificates, HMAC, CSPRNG | yes (native backend) |
| libsrtp2 | SRTP/SRTCP | yes (native backend) |
| libx264 | software H.264 encoder | optional |
| Mongoose 7.x | HTTP server | vendored, no action |

Without libx264 the build still succeeds, but only hardware encoding is
available and `--encoder sw` will fail at startup.

```sh
# Debian · Ubuntu · Raspberry Pi OS
sudo apt install build-essential libssl-dev libsrtp2-dev libx264-dev

# Fedora · RHEL
sudo dnf install gcc make openssl-devel libsrtp-devel x264-devel

# Arch
sudo pacman -S gcc openssl libsrtp x264

# openSUSE
sudo zypper install gcc libopenssl-devel libsrtp-devel x264-devel

# Alpine
sudo apk add build-base openssl-dev libsrtp-dev x264-dev
```

The libsrtp2 package name varies: `libsrtp2-dev` (Debian/Ubuntu),
`libsrtp-devel` (Fedora, 2.x), `libsrtp` (Arch), `libsrtp-dev` (Alpine).

---

## Build

```sh
make -j"$(nproc)"          # → build/camstream
```

Compiled with `-std=c11 -Wall -Wextra -Wpedantic -O2`, linked against
`-lssl -lcrypto -lsrtp2 -lpthread [-lx264] -lm`. There is no install step —
run the binary from `build/`.

| Target | Result |
|---|---|
| `make` | `build/camstream` — native WebRTC stack |
| `make camstream-janus` | `build/camstream-janus` — RTP to a Janus gateway, zero crypto deps |
| `make libpeer && make camstream-libpeer` | `build/camstream-libpeer` — sepfy/libpeer backend |
| `make test` | STUN, vision, and encoder-worker unit tests |
| `make test-janus` | Janus RTP sender against a fake gateway |
| `make vision-capture` | PGM/OBJ mosaic smoke tool |
| `make clean` · `make help` | — |

**Dependencies in non-standard prefixes:**

```sh
make OPENSSL_DIR=/opt/openssl SRTP_DIR=/opt/srtp X264_DIR=/opt/x264
```

x264 is auto-detected by probing for `x264.h` in `$X264_DIR/include`,
`/usr/include`, and `/usr/local/include`. Override with `HAVE_X264=1` or
`HAVE_X264=0`. The final build line reports whether x264 was linked in.

---

## CLI reference

Both `--opt value` and `--opt=value` are accepted. Defaults in the right column.

### Source

| Option | Meaning | Default |
|---|---|---|
| `-d, --device PATH` | V4L2 capture device | `/dev/video0` |
| `-t, --test` | Synthetic test pattern instead of a camera | off |
| `-W, --width N` | Capture width | `640` |
| `-H, --height N` | Capture height | `480` |
| `-F, --fps N` | Capture frame rate | `30` |

The device is opened as YUYV 4:2:2 when supported, otherwise the nearest
planar (YU12) mode. The test pattern emits YUYV directly: SMPTE-style bars, a
scrolling clock, and a sweeping marker that makes dropped frames obvious.

### Network

| Option | Meaning | Default |
|---|---|---|
| `-l, --listen ADDR` | HTTP bind address | `0.0.0.0` |
| `-p, --http-port N` | HTTP port (UI + signaling) | `8080` |
| `-u, --udp-port N` | Base UDP port for media | `50000` |

Viewer *i* binds UDP port `N+i` for `i` in `0..7`. The advertised ICE candidate
is the address the browser used to reach the server (from the HTTP `Host`
header) when that address belongs to a local interface; otherwise the first
private IPv4 address found.

### Encoding

| Option | Meaning | Default |
|---|---|---|
| `-e, --encoder MODE` | `auto` · `hw` · `hw:/dev/videoNN` · `sw` | `auto` |
| `-b, --bitrate KBPS` | Target bitrate | `2500` |
| `-K, --keyframe SEC` | Keyframe interval | `2` |

- **`auto`** — try V4L2 M2M hardware, fall back to libx264.
- **`hw`** — hardware only; fail if unavailable.
- **`hw:/dev/videoNN`** — pin a specific M2M node.
- **`sw`** — libx264 only; fail if not compiled in.

The hardware path negotiates NV12 (preferred) or YU12, interleaving I420→NV12
on the fly, and programs bitrate and GOP length where the driver allows.
The software path is latency-tuned: `zerolatency`, no B-frames, no reference
reordering, CBR, with IDRs forced every `K` seconds *and* on demand (PLI, FIR,
or a new viewer).

### Misc

| Option | Meaning |
|---|---|
| `-v, --verbose` | Verbose Mongoose + protocol logging |
| `-V, --version` | Print version and exit |
| `-h, --help` | Print usage and exit |

### Janus-only options

Accepted by `build/camstream-janus`; validated (and rejected) elsewhere so a
misconfigured build fails immediately rather than silently.

| Option | Meaning | Default |
|---|---|---|
| `--webrtc janus` | Backend selector, fixed per binary | `janus` |
| `--janus-host ADDR` | RTP/RTCP destination | `127.0.0.1` |
| `--janus-rtp-port N` | Mountpoint `videoport` | `5004` |
| `--janus-rtcp-port N` | Mountpoint `videortcpport` | `5005` |
| `--janus-rtcp-listen N` | Local port for PLI/FIR feedback | `5006` |

---

## HTTP API

Four routes. Everything else returns `404`.

### `POST /rtc/offer`

```jsonc
// request
{ "type": "offer", "sdp": "<browser SDP offer>" }

// 200
{ "type": "answer", "session_id": 2716354772, "udp_port": 50000, "sdp": "<SDP answer>" }
```

`400` malformed or missing SDP · `503` all 8 slots busy · `500` session
creation failed (usually a UDP port already bound).

> Request bodies are capped at 512 bytes for `/rtc/close`; offers use the full
> body. Oversized close requests are rejected rather than truncated.

### `POST /rtc/close`

```jsonc
{ "session_id": 2716354772 }   // → { "closed": true }
```

Sends DTLS `close_notify` and frees the slot immediately.

### `GET /status`

Abridged — the live response also carries encoder-worker diagnostics
(`frames_seen`, `skipped_idle`, `skipped_mismatch`, `no_output`) and
`sessions_total`. The `transport` field reads `webrtc`, `webrtc-libpeer`, or
`janus-rtp` depending on the binary.

```jsonc
{
  "version": "2.0.0", "uptime_sec": 412,
  "source":  { "name": "test pattern", "kind": "test", "width": 640, "height": 480, "fps": 30 },
  "encoder": { "name": "sw (libx264)", "preference": "sw", "bitrate_kbps": 2500, "keyframe_seconds": 2 },
  "http_port": 8080, "transport": "webrtc",
  "captured_frames": 12345, "encoded_frames": 12340, "au_dropped": 0,
  "sessions": [
    { "id": 2716354772, "state": "streaming", "udp_port": 50000,
      "packets_sent": 98765, "bytes_sent": 12345678,
      "pli": 0, "nacks": 3, "retx": 3 }
  ],
  "interfaces": [ { "name": "wlan0", "ip": "192.168.1.34" } ]
}
```

Session states: `new` → `ice` → `dtls` → `streaming` → `closed`.

### `GET /`

The embedded viewer page.

---

## Web UI

A single HTML page compiled into the binary (`src/app/web_ui.c`) — no
frameworks, no CDN, no external assets, so it works fully offline.

- Creates an `RTCPeerConnection` on load and `POST`s its offer to `/rtc/offer`.
- Displays state, throughput, and session id.
- Reconnects on its own: polls every 1.5 s while not streaming, gives up after
  10 s, retries every 5 s after a failure.
- Polls `/status` every 2 s for server-side counters.
- `Close` button issues `POST /rtc/close`.

All URLs are relative, so plain HTTP on a LAN is fine: WebRTC media is secured
end-to-end by DTLS-SRTP regardless of the page's transport.

---

## Architecture

```
  ┌─────────────────┐
  │ V4L2 / test src │
  └────────┬────────┘
           │  source thread — grab, never block
           ▼
  ┌─────────────────┐   keep-newest mailbox: a late consumer
  │   frame hub     │   jumps to the freshest frame, never
  └────────┬────────┘   drains a stale backlog
           │  encode thread — YUV convert, H.264
           ▼
  ┌─────────────────┐   overwrite-oldest ring
  │  AU ring (8 ×   │   8 slots × 512 KiB
  │  512 KiB)       │
  └────────┬────────┘
           │  main thread — one non-blocking poll loop
           ▼
  ┌──────────────────────────────────────────────────┐
  │ per viewer:  RTP packetize → SRTP → UDP          │
  │ shared:      Mongoose HTTP (UI + signaling)      │
  │              ICE · DTLS timers · RTCP · NACK     │
  └──────────────────────────────────────────────────┘
```

### Threads

Exactly three. No thread pool, no work queues.

| Thread | Owns |
|---|---|
| **Source** | V4L2 `DQBUF`/`QBUF` (or pattern synthesis) into the frame hub |
| **Encode** | I420 conversion, H.264 encode, push access units to the ring |
| **Main** | Mongoose HTTP, viewer UDP sockets, DTLS retransmit timers, RTCP sender reports, AU fan-out |

### Data structures

- **Frame pool** (`frame_pool.c`) — fixed pre-allocated refcounted buffers. No
  allocation in the capture path.
- **Frame hub** (`frame_hub.c`) — one keep-newest mailbox per consumer, guarded
  by a condition variable.
- **AU ring** (`au_ring.c`) — SPSC ring of encoded access units, 8 × 512 KiB.
  Full ring overwrites the oldest slot.

### The access-unit contract

An access unit is **one H.264 picture in Annex B form**: NAL units prefixed
with `00 00 00 01`, SPS and PPS preceding every keyframe. Both encoder backends
guarantee this, and the RTP packetizer depends on it to find NAL boundaries.

---

## The WebRTC stack

### Signaling

The browser adds one `sendonly` H.264 transceiver, sets a local description,
and `POST`s the offer. The server parses ICE ufrag/pwd, the DTLS fingerprint,
and the negotiated H.264 payload type, then replies with an answer. One HTTP
round trip. No WebSocket, no trickle exchange, no TURN.

### SDP answer

```sdp
v=0
o=- 1 1 IN IP4 192.168.1.10
s=camstream
t=0 0
a=ice-lite
a=ice-options:trickle
a=group:BUNDLE 0
a=msid-semantic: WMS camstream
a=fingerprint:sha-256 EF:0C:40:…
a=setup:passive
a=ice-ufrag:LCcIVVeF
a=ice-pwd:…
m=video 9 UDP/TLS/RTP/SAVPF 96
c=IN IP4 192.168.1.10
a=mid:0
a=sendonly
a=rtcp-mux
a=msid:camstream camstream-video
a=ssrc:… cname:camstream
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=1
a=rtcp-fb:96 nack
a=rtcp-fb:96 nack pli
a=rtcp-fb:96 ccm fir
a=candidate:1 1 udp 2130706431 192.168.1.10 50000 typ host generation 0
```

Details that exist because a browser rejected the alternative:

- **`a=ice-lite` is session-level only** (RFC 8839 §5.4). Placing it on the
  m-line made some browsers treat the server as a full ICE agent and wait
  forever for checks it never sends.
- **Unused audio is rejected with port 0** (JSEP §5.3.1). Port 9 plus
  `a=inactive` is *accepted-but-inactive* and demands its own ICE transport
  when it is not bundled.
- **Candidates carry `generation 0`** and component id `1` — the exact form
  Chromium and Firefox parse.
- **Fingerprint** is `sha-256` in colon-separated uppercase hex.
- Payload type and mid are echoed from the offer rather than hard-coded.

### ICE-lite

The server is an ICE-lite agent (RFC 5245 §6.1.1): it gathers nothing and
initiates nothing. It answers connectivity checks and records the peer.

- **Demultiplexing** per RFC 7983 on the first byte: `0–3` STUN, `20–63` DTLS,
  `≥128` RTP/RTCP.
- **Binding requests** are authenticated before they are acted on: magic
  cookie, `USERNAME` bound to the local ufrag, and **`MESSAGE-INTEGRITY`
  verified** (HMAC-SHA1 over the local ice-pwd) — because the ufrag is public
  in the SDP answer, so without MI any host on the LAN could claim the peer
  slot. Failures are answered with a STUN `401`, not dropped silently.
- **Responses** carry `XOR-MAPPED-ADDRESS`, `MESSAGE-INTEGRITY`, and
  `FINGERPRINT` (CRC32, attribute `0x8028`).
- **The peer address follows the latest valid check.** Locking the first one
  breaks NAT rebinds (Wi-Fi↔cellular, router restart) by leaving media aimed
  at a dead 5-tuple.

> **MESSAGE-INTEGRITY is exact, and easy to get wrong.** The HMAC covers the
> message up to but *not including* the MI attribute, with the header length
> field set as if MI were present. A trailing `FINGERPRINT` makes the
> on-the-wire length 8 bytes larger than the value the sender hashed, so
> verification must hash a length-patched copy. `tests/test_stun.c` pins all of
> this against the **RFC 5769** published vectors.

### DTLS 1.2

- Self-signed P-256 certificate, generated once per process.
- The browser checks the server's SHA-256 fingerprint against the answer; the
  server checks the browser's against the offer. Either mismatch aborts.
- OpenSSL is driven through a BIO pair: inbound datagrams queued, outbound
  records written straight to the UDP socket, retransmission timers serviced
  from the poll loop.
- 60 bytes of keying material are exported with label `EXTRACTOR-dtls_srtp`
  (RFC 5764) and split into client/server master keys (16 B) and salts (14 B).

### SRTP

One libsrtp2 session per direction per viewer,
`SRTP_AES128_CM_SHA1_80`. Keys are derived only from the DTLS exporter and
never leave the process.

### RTP — RFC 6184

- 90 kHz clock, timestamps derived from `CLOCK_MONOTONIC` capture time.
- NALs within the 1200-byte packet budget go out as single-NAL packets; larger
  ones are fragmented **FU-A**.
- Marker bit set on the last packet of each picture.
- Per-session 16-bit sequence space and an SSRC from `RAND_bytes`.

### RTCP

- **Sender reports** every 1 s per connected viewer (NTP epoch offset
  2208988800).
- **PLI** (PT 206 FMT 1) and **FIR** (PT 206 FMT 4) raise a single atomic
  `force_idr` flag. The encoder thread consumes it with `atomic_exchange`, so a
  burst of requests from several viewers coalesces into **one** IDR rather than
  a storm of them.
- **Generic NACK** (PT 205 FMT 1) is served from a 512-packet per-session
  retransmission cache, up to 128 sequence numbers per report.
- **BYE** (PT 203) closes the session.

---

## Session lifecycle

```
POST /rtc/offer ─────────────► NEW        answer sent, UDP port bound
first authenticated STUN ────► ICE        peer locked; DTLS now accepted
first ClientHello ───────────► DTLS       handshake in flight
RFC 5764 keys exported ──────► STREAMING  RTP flowing
BYE · idle 15 s · DTLS watchdog 30 s · fatal error · shutdown
                             ► CLOSED     slot freed same loop iteration
```

Any state can fail directly to `CLOSED`. A session that never produces a valid
STUN check is reaped after **15 s**; one that reaches ICE but never completes
DTLS is reaped after **30 s**. A vanished browser cannot hold a slot.

---

## Multi-viewer behaviour

- **8 concurrent viewers**, one session and one UDP port each.
- **Encode once, fan out.** The main thread pops each AU and packetizes it per
  viewer — independent sequence numbers and SRTP contexts, shared timestamp.
- **Every new viewer forces an IDR**, so nobody waits up to `-K` seconds for a
  first picture.
- **PLI/FIR are global** — there is one encoder, and concurrent requests
  coalesce into a single IDR.
- **Departure** is by BYE, idle timeout, or the DTLS watchdog. When the last
  viewer leaves the encoder idles, but the source thread keeps filling the hub
  — so the next arrival gets a *fresh* frame, never a stale backlog.

---

## Transport backends

Three binaries share one capture/encode/vision pipeline and differ only in how
pixels reach the browser.

| Backend | Binary | WebRTC by | Deps | Use when |
|---|---|---|---|---|
| **Native** | `camstream` | own C code | openssl, srtp2, x264 | Default. Lowest latency, smallest surface. |
| **libpeer** | `camstream-libpeer` | sepfy/libpeer | mbedTLS, bundled srtp/usrsctp/cJSON | You want a third-party spec-compliance reference. |
| **Janus** | `camstream-janus` | external Janus | **none** (`-lx264 -lpthread -lm`) | Many viewers, or you already run Janus. |

```sh
# libpeer
sudo apt install -y git cmake build-essential libx264-dev
make libpeer && make camstream-libpeer -j2
./build/camstream-libpeer --test --encoder sw --listen 0.0.0.0 --http-port 8000
```

The libpeer build routes V4L2/test → FrameHub → H.264 → AU ring →
`peer_connection_send_video()`, with Mongoose still handling `POST /rtc/offer`
and libpeer owning UDP, ICE, and DTLS-SRTP internally.
See `docs/21_libpeer_runtime.md` and `docs/18_libpeer_phase3.md`.

---

## Janus transport

`camstream-janus` swaps only the browser-facing transport. The same C pipeline
feeds an AU ring; a dedicated sender thread packetizes with the *same*
`rtp_h264.c` and `sendto()`s plain RTP to a Janus gateway, which terminates
WebRTC for arbitrarily many viewers.

```
capture/encode (unchanged) → AU ring → janus_rtp_sender
   → RTP H.264 / UDP → Janus streaming plugin (mountpoint "camstream")
   → WebRTC → browsers
   ← RTCP PLI/FIR relayed back → force_idr
```

- **No crypto in the binary** — links `[-lx264] -lpthread -lm`.
- **Keyframe control stays in camstream**: Janus relays viewer PLI/FIR to a
  local RTCP port, parsed by `rtcp.c`, raising the same `force_idr` flag the
  encoder already honours. Sender reports every 5 s also teach Janus the
  return address.
- **Dashboard preserved**: `/status` reports Janus stats; `index.html` and
  `janus-client.js` are embedded at build time via `tools/embed_assets.c`.

```sh
sudo apt install -y janus-gateway
sudo cp config/janus/*.jcfg /etc/janus/ && sudo systemctl restart janus
make camstream-janus -j2
./build/camstream-janus --test -e sw
# browser: http://<host>:8080/?janus=<host>:8188&stream=1
```

| Flow | Port |
|---|---|
| camstream → Janus RTP | 5004 |
| camstream → Janus RTCP | 5005 |
| Janus → camstream RTCP | 5006 |
| camstream HTTP | 8080 |
| Janus HTTP / WebSocket | 8088 / 8188 |

`make test-janus` validates packetization, FU-A reassembly, PLI→`force_idr`,
and SR contents against a fake gateway — no camera and no Janus required.
Full detail: `docs/22_janus_transport.md`.

---

## Raspberry Pi

Runs on Raspberry Pi OS bookworm, 32- or 64-bit.

```sh
sudo apt install build-essential libssl-dev libsrtp2-dev libx264-dev
make -j4
sudo usermod -aG video "$USER"          # then log out and back in
./build/camstream -d /dev/video0 -e hw:/dev/video11
```

- The **bcm2835 M2M H.264 encoder is `/dev/video11`**. It takes NV12
  (preferred) or YU12; camstream interleaves I420→NV12 as needed and reads
  Annex B straight off the capture queue.
- The **camera module is managed by libcamera** and usually also appears as a
  V4L2 node, typically `/dev/video0`. Enumerate with:

  ```sh
  v4l2-ctl --list-devices
  v4l2-ctl -d /dev/video0 --list-formats-ext
  ```

- **Only one process may hold the sensor.** Stop `libcamera-hello`,
  `rpicam-still`, and friends first.
- **CPU budget**: `-e sw` handles 480p30 comfortably and 720p30 warm. Use
  `-e hw` for 1080p.

---

## Acceptance tests

Run in order; each isolates one layer.

**1 — Build**

```sh
make -j"$(nproc)"     # expect: built build/camstream (x264: yes)
make test             # STUN (incl. RFC 5769 vectors), vision, encoder worker
```

**2 — Pipeline without hardware**

```sh
./build/camstream --test -e sw
```

**3 — End-to-end** — open `http://<server-ip>:8080/` from another machine.
Picture within ~2 s, and the log must reach:

```
rtc <id>: ICE validated (…)
rtc <id>: DTLS connected, SRTP keys derived
rtc <id>: streaming video
```

**4 — Telemetry**

```sh
curl -s http://<server-ip>:8080/status | head -c 400
```

State `streaming`, `packets_sent` climbing.

**5 — Real camera** — `./build/camstream -d /dev/video0`

**6 — Second viewer** — both stream; `/status` shows two sessions on UDP
50000 and 50001.

**7 — Churn** — reload several times. Each reload opens a new session, retires
the old one, and gets an immediate keyframe.

---

## Troubleshooting

Full matrix: `docs/17_troubleshooting.md`.

### Read the log first

| Line | Meaning |
|---|---|
| `signaling complete` | Answer sent; waiting for the browser's first check |
| `ICE validated (ip:port)` | Authenticated STUN check; peer locked |
| `DTLS connected, SRTP keys derived` | Media may now flow |
| `streaming video` | First RTP packets sent |
| `peer moved … (NAT rebind)` | Peer address changed; media followed |
| `STUN MESSAGE-INTEGRITY invalid … sending 401` | Check failed authentication — see below |
| `idle timeout` / `closed` | Session reaped |

### Symptom → cause

| Symptom | Likely cause | Check |
|---|---|---|
| Browser shows nothing, no session in log | Wrong URL or blocked TCP | `curl http://<ip>:8080/status` from the *client* |
| `session create failed (UDP port busy?)` | Port occupied | `ss -ulnp \| grep 500` · change `-u` |
| Log stops at `signaling complete`, **no UDP logged** | UDP genuinely blocked, or page opened via a proxy | Open TCP 8080 + UDP 50000-50007; use the LAN IP directly |
| Log stops at `signaling complete`, **UDP arriving** | Checks received but rejected — read the next line | If it says `401`, ICE auth is failing, not the firewall |
| `ICE validated` but no `DTLS connected` | Fingerprint mismatch or one-way UDP | UDP must pass **both** directions; run `-v` |
| `DTLS connected` but no video | Encoder produced nothing | `/status` → `encoded_frames`; retry with `--test`; `-e sw` needs x264 |
| Video stutters | Link cannot carry the bitrate | Lower `-b`, lower resolution; watch `nacks`/`retx` |
| `no usable encoder` | No M2M node and no x264 | `v4l2-ctl -d /dev/video11 --list-formats-ext`; rebuild with x264 |
| Camera will not open | Permissions or in use | `video` group; stop other camera apps |
| Only the first viewer sees video | Keyframe gap | Should not occur — new viewers force an IDR; file a bug with `/status` |

> **"ICE failed" does not imply a firewall.** If the log shows
> `UDP STUN … bytes from …`, UDP is already arriving and the network is fine —
> the packets are being *rejected*. Look at the line immediately after.

---

## Repository layout

```
include/
  app/        app_config.h · app_server.h
  media/      frame_pool · frame_hub · au_ring · video_source · yuv_convert
              h264_encoder · source_worker · encoder_worker
  webrtc/     ice_lite · dtls_srtp · rtp_h264 · rtcp · sdp · webrtc_session
  janus/      janus_rtp_sender.h
  vision/     frame_matrix.h · vision_worker.h

src/
  camstream_main.c        entry point, signal handling
  app/                    app_config.c   CLI parsing
                          app_server.c   HTTP routes + main poll loop
                          web_ui.c       embedded viewer page
  media/                  frame_pool · frame_hub · au_ring
                          v4l2_source · test_source · yuv_convert
                          h264_encoder (dispatch) · encoder_x264 · encoder_v4l2m2m
                          source_worker · encoder_worker
  webrtc/                 ice_lite.c     STUN + UDP demux
                          dtls_srtp.c    OpenSSL DTLS, RFC 5764 export, libsrtp2
                          rtp_h264.c     RFC 6184 packetizer
                          rtcp.c         SR / RR / PLI / FIR / NACK / BYE
                          sdp.c          offer parse, answer build
                          webrtc_session.c  per-viewer state machine
                          webrtc_session_libpeer.c · libpeer_global.c
  janus/                  janus_rtp_sender.c
  vision/                 frame_matrix · vision_capture · vision_worker

tests/      test_stun.c (incl. RFC 5769 vectors) · test_vision.c
            test_encoder_worker.c · test_janus_sender.c
tools/      embed_assets.c        build-time asset → C array
web/janus/  index.html · janus-client.js
config/janus/  janus.jcfg · janus.plugin.streaming.jcfg
third_party/mongoose/   Mongoose 7.x (HTTP only)
docs/       10_architecture · 11_webrtc_internals · 12_build_reference
            13_lan_two_laptops · 14_raspberry_pi · 15_optimization_notes
            16_protocol_reference · 17_troubleshooting · 18_libpeer_phase3
            19_execution_roadmap · 20_webrtc_zero_latency · 21_libpeer_runtime
            22_janus_transport
```

Vision smoke tool:

```sh
make vision-capture
./build/vision-capture --test -W 640 -H 480 -F 30 -s 10 -o build/mosaic
```

Writes a grayscale PGM and an intensity-derived OBJ mosaic.

---

## Tuning constants

Compile-time, with the file that owns each.

| Constant | Value | Where |
|---|---|---|
| `MAX_RTC_SESSIONS` | 8 | `src/app/app_server.c` |
| `AU_RING_SLOTS` × `AU_SLOT_CAPACITY` | 8 × 512 KiB | `src/app/app_server.c` |
| `RTP_MAX_PACKET` | 1200 B | `include/webrtc/rtp_h264.h` |
| `RETX_CACHE_SIZE` | 512 packets | `src/webrtc/webrtc_session.c` |
| `SESSION_IDLE_TIMEOUT_MS` | 15000 | `src/webrtc/webrtc_session.c` |
| `SESSION_DTLS_WATCHDOG_MS` | 30000 | `src/webrtc/webrtc_session.c` |
| `RTCP_SR_INTERVAL_MS` | 1000 | `src/webrtc/webrtc_session.c` |
| `SRTP_KEY_MATERIAL_LEN` | 60 B | `src/webrtc/dtls_srtp.c` |

---

## Standards implemented

| RFC | What |
|---|---|
| 3711 | SRTP — `AES_CM_128_HMAC_SHA1_80` |
| 3550 / 3551 | RTP and RTCP |
| 4585 / 5104 | RTCP feedback: PLI, FIR |
| 4588 | Retransmission / NACK |
| 5245 · 8445 | ICE, ICE-lite |
| 5389 · 5769 | STUN, and its published test vectors |
| 5764 | DTLS-SRTP key export |
| 6184 | RTP payload for H.264 — single-NAL, FU-A |
| 6347 | DTLS 1.2 |
| 7983 | Multiplexing STUN / DTLS / RTP on one port |
| 8839 · 8842 | SDP for ICE and DTLS-SRTP |

---

## Versioning

**2.0.0.** WebRTC-only. The 1.x WebSocket / HTTP-chunked video transport has
been removed — if any page, script, or note still references `/stream` or a
`ws://` video URL, it belongs to 1.x and will not work.
