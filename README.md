# camstream

A low latency camera to browser streaming server written in C. The media
path is real WebRTC: every viewer gets its own peer connection with ICE
lite, DTLS 1.2, SRTP, RTP H.264 packetization (RFC 6184), NACK based
retransmission and RTCP feedback. There is no WebSockets video transport
and no other streaming framework: Mongoose is used only to serve the
built in web page and the WebRTC signaling HTTP API.

```
[ webcam V4L2 or test pattern ]
        |
        v
  source thread --> frame hub (keep newest mailbox per consumer)
        |
        v
  encode thread --> H.264 access unit ring (overwrite oldest)
        |
        v
  main thread --> per viewer: RTP packetizer --> SRTP --> UDP (ICE socket)
                    Mongoose: web page + WebRTC signaling on one HTTP port
```

## Start here: offline Pi to laptop WebRTC

This is the primary deployment path. The Raspberry Pi owns the camera and
runs `camstream`. The laptop runs only a browser. The Ethernet cable carries
HTTP signaling over TCP and WebRTC media over UDP. No Internet, cloud service,
STUN server, TURN server, container or WebSocket video transport is required.

Pi address:

```text
192.168.1.10/24
```

Laptop address:

```text
192.168.1.20/24
```

Build and run on the Pi:

```sh
sudo apt install -y build-essential libssl-dev libsrtp2-dev libx264-dev
make -j2
./build/camstream --device /dev/video0 --encoder auto --listen 0.0.0.0
```

Open this URL on the laptop:

```text
http://192.168.1.10:8080/
```

If the camera or hardware encoder is not ready, verify the WebRTC path first
with the synthetic source:

```sh
./build/camstream --test --encoder sw --listen 0.0.0.0
```

Allow TCP port `8080` and UDP ports `50000` through `50007` on the Pi. The
browser must use the Pi Ethernet address, not `localhost`. For the complete
procedure, read `docs/20_webrtc_zero_latency.md` and then
`docs/17_troubleshooting.md` if a state does not become `streaming`.

## Table of contents

1. [Start here: offline Pi to laptop WebRTC](#start-here-offline-pi-to-laptop-webrtc)
2. [Features](#features)
3. [Requirements](#requirements)
4. [Build](#build)
5. [Run](#run)
6. [Command line reference](#command-line-reference)
7. [Web UI](#web-ui)
8. [HTTP API](#http-api)
9. [Architecture](#architecture)
10. [WebRTC internals](#webrtc-internals)
11. [Multi viewer behavior](#multi-viewer-behavior)
12. [Raspberry Pi](#raspberry-pi)
13. [Verification checklist](#verification-checklist)
14. [Troubleshooting](#troubleshooting)
15. [Repository layout](#repository-layout)
16. [libpeer Phase 3](docs/18_libpeer_phase3.md)
17. [libpeer Runtime - Fully Migrated](docs/21_libpeer_runtime.md)
18. [Execution roadmap](docs/19_execution_roadmap.md)
19. [Offline WebRTC deployment](docs/20_webrtc_zero_latency.md)

## Features

- Real WebRTC media transport (RFC 8839 stack built natively in C):
  ICE lite (RFC 5245/5389), DTLS 1.2 (RFC 6347/5764), SRTP (RFC 3711,
  AES_CM_128_HMAC_SHA1_80), RTP H.264 (RFC 6184).
- Hardware or software H.264 encoding: V4L2 memory-to-memory encoders
  (bcm2835 on Raspberry Pi, cedrus, etc.) with automatic fallback to
  libx264 when the hardware encoder is not available.
- Up to 8 concurrent viewers, each with its own SRTP keys, RTP sequence
  space, retransmission cache and DTLS session.
- NACK (RFC 4588) retransmission from a per session cache, PLI/FIR key
  frame requests with rate limiting, RTCP sender reports.
- Built in web viewer (single file, no frameworks, no CDNs) with
  auto connect, auto reconnect and live quality indicators.
- No camera? The synthetic test pattern source (`--test`) gives a full
  end to end pipeline for verification.
- Small and auditable: about 6000 lines of C including the embedded
  web page; the only third party code is Mongoose (HTTP server).

## Requirements

- Linux with a V4L2 video device (or `--test` for the synthetic
  source).
- C11 compiler with pthreads (GCC or Clang).
- OpenSSL 1.1 or 3.x (DTLS).
- libsrtp2 (SRTP).
- libx264 (software encoder fallback; the build works without it but
  then only hardware encoding is available).
- Mongoose 7.x, vendored under `third_party/mongoose/`.

### System packages

Debian, Ubuntu, Raspberry Pi OS (including 64 bit):

```
sudo apt install build-essential libssl-dev libsrtp2-dev libx264-dev
```

Fedora:

```
sudo dnf install gcc make openssl-devel libsrtp-devel x264-devel
```

Arch Linux:

```
sudo pacman -S gcc openssl libsrtp x264
```

openSUSE:

```
sudo zypper install gcc libopenssl-devel libsrtp-devel x264-devel
```

Alpine:

```
sudo apk add build-base openssl-dev libsrtp-dev x264-dev
```

Notes:

- The libsrtp2 package name differs between distributions: `libsrtp2-dev`
  (Debian/Ubuntu), `libsrtp-devel` (Fedora/RHEL, 2.x), `libsrtp` (Arch),
  `libsrtp-dev` (Alpine).
- If you built the dependencies yourself, point the Makefile at them:

```
make OPENSSL_DIR=/opt/openssl SRTP_DIR=/opt/srtp X264_DIR=/opt/x264
```

- x264 detection: the Makefile auto detects `x264.h` in
  `/usr/include`, `/usr/local/include` or `$X264_DIR/include`. Force it
  with `HAVE_X264=1` or `HAVE_X264=0`. The final build line prints
  whether x264 is in.

## Build

```
make -j$(nproc)
```

Output: `build/camstream`.

```
make clean        # remove build/
make help         # list targets and overrides
```

The build compiles with `-std=c11 -Wall -Wextra -Wpedantic -O2` and
links `-lssl -lcrypto -lsrtp2 -lpthread [-lx264] -lm`. No installation
step is needed; run the binary from `build/`.

## Run

```
./build/camstream --test -e sw        # no camera: test pattern, software encode
./build/camstream -d /dev/video0      # real camera, auto encoder
./build/camstream -e hw:/dev/video11  # force a specific V4L2 M2M encoder
```

Startup log (abridged):

```
camstream 2.0.0
source        : test pattern
resolution    : 640x480 @ 30 fps
encoder       : sw (libx264), 2500 kbps, keyframe every 2s
http          : http://0.0.0.0:8080/  (web UI + WebRTC signaling)
udp media     : ports from 50000

camstream 2.0.0 ready
open http://192.168.1.34:8080/   (wlan0)
```

Open the printed URL in a browser. When the video appears the server
logs (per viewer session, id in hex):

```
rtc a1b2c3d4: signaling complete (slot 0, 192.168.1.34)
rtc a1b2c3d4: ICE validated (192.168.1.35:51234)
rtc a1b2c3d4: DTLS connected, SRTP keys derived
rtc a1b2c3d4: streaming video
```

## Command line reference

Defaults in parentheses. Both `-x value` and `-x=value` forms work.

### Source

| Option | Meaning | Default |
| --- | --- | --- |
| `-d, --device PATH` | V4L2 capture device | `/dev/video0` |
| `-t, --test` | Synthetic test pattern instead of a camera | off |
| `-W, --width N` | Capture width in pixels | 640 |
| `-H, --height N` | Capture height in pixels | 480 |
| `-F, --fps N` | Capture frame rate | 30 |

The source is opened in YUYV 4:2:2 at the requested resolution when the
device supports it; otherwise it falls back to the best planar (YU12)
mode at or near the requested size. The test pattern generates YUYV
directly: SMPTE style color bars, a scrolling clock and a sweeping
marker so dropped frames are visible.

### Network

| Option | Meaning | Default |
| --- | --- | --- |
| `-l, --listen ADDR` | HTTP listen address | `0.0.0.0` |
| `-p, --http-port N` | HTTP port (web UI + signaling) | 8080 |
| `-u, --udp-port N` | Base UDP port for media | 50000 |

Viewer `i` binds UDP port `N+i` (0 to 7). The SDP candidate advertises
the IP the browser used to reach the server (from the HTTP `Host`
header) when that IP is one of the local interfaces, otherwise the
first private IPv4 address.

### Encoding

| Option | Meaning | Default |
| --- | --- | --- |
| `-e, --encoder MODE` | Encoder selection: `auto`, `hw`, `hw:/dev/videoNN`, `sw` | `auto` |
| `-b, --bitrate KBPS` | Target bitrate in kbps | 2500 |
| `-K, --keyframe SEC` | Keyframe interval in seconds | 2 |

Encoder selection:

- `auto` (default): try the V4L2 memory-to-memory H.264 encoder first,
  fall back to libx264.
- `hw`: V4L2 M2M only, fail if none works.
- `hw:/dev/videoNN`: force a specific device node.
- `sw`: libx264 only, fail if it was not compiled in.

The hardware backend probes the device for an input format (NV12
preferred, YU12 accepted and interleaved on the fly) and a matching
output format, and sets the bitrate/keyframe interval where the driver
supports it. The software backend is tuned for latency: zero latency
preset, no reference reordering, CBR rate control, keyframes forced
every `K` seconds plus on demand (PLI/FIR/new viewer).

### Misc

| Option | Meaning |
| --- | --- |
| `-v, --verbose` | Verbose Mongoose logging |
| `-V, --version` | Print version and exit |
| `-h, --help` | Print usage and exit |

## Web UI

`GET /` serves a single embedded HTML page (`src/app/web_ui.c`). No
frameworks, no external assets, no CDNs. It:

- connects with the browser `RTCPeerConnection` on load
  (`iceTransportPolicy: all`, UDP candidate gathering, host candidates
  preferred for lowest latency),
- POSTs the SDP offer to `/rtc/offer`, applies the answer,
- shows connection state (offer, connecting, streaming, failed) plus
  bytes/s and the session id,
- auto reconnects: polls every 1.5 s while not streaming, gives up
  after 10 s, retries every 5 s after a failure,
- polls `/status` every 2 s and shows server side session statistics,
- offers a `Close` button (POST `/rtc/close`).

The page uses relative URLs, so it works over plain HTTP on a LAN
without TLS (WebRTC media is end to end secured by DTLS/SRTP
independently of the transport scheme).

## HTTP API

All endpoints are on the HTTP port (default 8080).

### `POST /rtc/offer`

Start a WebRTC session.

Request body:

```
{"type":"offer","sdp":"<SDP offer from the browser>"}
```

Response `200`:

```
{"type":"answer","session_id":2716354772,"udp_port":50000,"sdp":"<SDP answer>"}
```

Errors: `400` (missing or unparsable SDP), `503` (all 8 session slots
occupied), `500` (session create failed, usually a busy UDP port).

### `POST /rtc/close`

```
{"session_id":2716354772}
```

Response `200`: `{"closed":true}`. The server sends a DTLS
close_notify and releases the slot.

### `GET /status`

Server and per session statistics as JSON:

```
{"version":"2.0.0","uptime_sec":412,
 "source":{"name":"test pattern","kind":"test","width":640,"height":480,"fps":30},
 "encoder":{"name":"sw (libx264)","preference":"sw","bitrate_kbps":2500,"keyframe_seconds":2},
 "http_port":8080,"transport":"webrtc",
 "captured_frames":12345,"encoded_frames":12340,"au_dropped":0,
 "sessions":[{"id":2716354772,"state":"streaming","udp_port":50000,
              "packets_sent":98765,"bytes_sent":12345678,
              "pli":0,"nacks":3,"retx":3}],
 "interfaces":[{"name":"wlan0","ip":"192.168.1.34"}]}
```

Session states: `new`, `ice`, `dtls`, `streaming`, `closed`.

### `GET /`

The web UI.

Everything else returns `404`.

## Architecture

### Threading model

| Thread | Responsibility |
| --- | --- |
| Main | Mongoose HTTP loop (web UI, signaling), poll loop over viewer UDP sockets, DTLS retransmission timers, RTCP sender reports, access unit fan out to all viewers |
| Source | V4L2 (or test pattern) frame grabs into the frame hub |
| Encode | I420 conversion, H.264 encode, push access units to the ring |

There are no other threads. The main thread never blocks on media.

### Pipeline data structures

- **Frame pool** (`frame_pool.c`): fixed number of pre allocated
  buffers with reference counting. No malloc/free in the hot capture
  path.
- **Frame hub** (`frame_hub.c`): one per source. Each consumer (today:
  the encoder) gets a keep newest mailbox protected by a condition
  variable: late consumers always jump to the freshest frame instead of
  draining stale ones.
- **Access unit ring** (`au_ring.c`): single producer (encoder thread),
  single consumer (main thread) ring of encoded H.264 access units.
  When full, the oldest slot is overwritten: live video prefers
  freshness over completeness. 8 slots of 512 KiB by default.

### Access unit contract

An access unit is one H.264 picture in Annex B form: a sequence of
`00 00 00 01` prefixed NAL units (SPS/PPS before keyframes). Both
encoder backends produce Annex B; the RTP packetizer relies on that to
find NAL boundaries.

## WebRTC internals

### Signaling (HTTP)

The browser creates an `RTCPeerConnection`, adds one transceiver
(`sendonly` video, `H264`), sets local description and POSTs the offer
to `/rtc/offer`. The server parses ICE ufrag/pwd, the fingerprint and
the H.264 payload type, answers with an SDP answer and the browser sets
it as remote description. That is all; no WebSocket and no TURN.

### SDP answer

The answer advertises exactly one ICE candidate and one media line:

```
v=0
o=- 0 0 IN IP4 192.168.1.34
s=camstream
t=0 0
a=group:BUNDLE 0
m=video 9 UDP/TLS/RTP/SAVPF 96
c=IN IP4 192.168.1.34
a=mid:0
a=sendonly
a=ice-ufrag:<16 random bytes>
a=ice-pwd:<32 random bytes>
a=fingerprint:sha-256 <cert digest>
a=rtpmap:96 H264/90000
a=fmtp:96 profile-level-id=42e01f;level-asymmetry-allowed=1;packetization-mode=1
a=rtcp-fb:96 nack
a=candidate:1 1 udp 2113667327 192.168.1.34 50000 typ host generation 0
```

Details that matter for browser compatibility:

- The candidate is `a=candidate:1 1 udp ...` with component id `1` and
  a trailing `generation 0` extension, the exact form Chromium and
  Firefox accept.
- The fingerprint is `a=fingerprint:sha-256 <digest>` (RFC 8842/7999
  format, digest in colon separated uppercase hex), the form modern
  browsers require.
- ICE credentials: 16 byte ufrag, 32 byte pwd, from `RAND_bytes`.
- Payload type 96 is offered with `nack` RTCP feedback; the packetizer
  uses 90 kHz clock as H.264 requires.

### ICE lite

The server is an ICE lite agent (RFC 5245 section 6.1.1): it does not
gather candidates, it only answers the browser's connectivity checks
and remembers the validated peer address.

- UDP demultiplexing per RFC 7983: first byte 0 to 3 = STUN, 20 to 63
  = DTLS, 128 to 191 = RTP/RTCP, anything else ignored.
- STUN binding requests (RFC 5389) are validated (magic cookie,
  USERNAME must start with `<server-ufrag>:`) and answered with
  XOR-MAPPED-ADDRESS, MESSAGE-INTEGRITY (HMAC-SHA1 with the local
  ice-pwd) and FINGERPRINT (CRC32).
- The first valid check locks the remote address. DTLS data is only
  processed after that, as ICE requires.

### DTLS 1.2

- The server acts as the DTLS server on a self signed P-256 EC
  certificate (generated at startup, one per process).
- The browser verifies the certificate SHA-256 fingerprint against the
  SDP answer; the server verifies the browser's fingerprint from the
  offer (RFC 7999). Mismatch fails the handshake.
- OpenSSL drives the handshake through a custom BIO pair: inbound
  datagrams are queued, outbound records go straight to the UDP socket.
  Retransmission timers are served from the poll loop.
- After the handshake, 60 bytes of keying material are exported with
  the label `EXTRACTOR-dtls_srtp` (RFC 5764) and split into
  client/server SRTP master keys and salts.

### SRTP

One libsrtp2 session per direction per viewer, AES_CM_128_HMAC_SHA1_80.
Keys never leave the process; the DTLS layer is the only source of
keying material.

### RTP (RFC 6184)

- Payload type 96, 90 kHz timestamp derived from the frame capture
  timestamp (CLOCK_MONOTONIC microseconds).
- NALs that fit in the MTU budget (1200 byte max packet) go out as
  single NAL unit packets; larger NALs are fragmented with FU-A.
- The last RTP packet of each picture carries the marker bit.
- Each session has its own 16 bit sequence number space and an
  SSRC from `RAND_bytes`.

### RTCP

- Sender report every second while a viewer is connected (NTP mapping
  is CLOCK_REALTIME based, offset 2208988800).
- Parsed from the viewer: PLI and FIR trigger a rate limited keyframe
  request (atomic flag read by the encoder thread), Generic NACK
  (PT 200, FMT 1) is answered from the per session retransmission
  cache, BYE closes the session.

### Session life cycle

```
POST /rtc/offer --> RTC_NEW (answer sent, UDP port bound)
first valid STUN check --> RTC_ICE (peer address locked, DTLS waits)
first ClientHello --> RTC_DTLS (handshake in progress)
handshake done, RFC 5764 keys exported --> RTC_STREAMING (video flows)
BYE, idle 15 s, DTLS watchdog 30 s, fatal error, or server shutdown
--> RTC_CLOSED (slot freed)
```

Every state transitions to `RTC_CLOSED` on fatal errors. A session that
never produces even the first STUN check is reaped after 15 s of
silence, so a vanished browser cannot hold a slot forever.

## Multi viewer behavior

- Up to 8 viewers, one session (and one UDP port) each.
- All viewers receive the same encoded access units: encode once,
  fan out. The main thread pops each access unit from the ring and
  packetizes it per viewer (each viewer has its own sequence numbers,
  timestamp is the same, RTP payload type is per offer but fixed at 96
  here).
- A new viewer always gets a forced keyframe: the server sets the
  force IDR flag when a session is created and when a viewer connects
  past DTLS; the encoder emits an IDR on its next frame.
- Keyframe requests (PLI/FIR) are global (one encoder) and rate
  limited to one per 500 ms.
- Viewer drop out: BYE, idle timeout or the 30 s DTLS watchdog. The
  slot is freed in the same loop iteration, `rtc_active` goes back to
  0 when the last viewer leaves, and the encoder idles. The source
  thread keeps grabbing frames into the hub meanwhile, so when the
  next viewer joins it gets fresh frames, never a stale backlog.

## Raspberry Pi

camstream runs on Raspberry Pi OS (32 or 64 bit, bookworm).

```
sudo apt install build-essential libssl-dev libsrtp2-dev libx264-dev
make -j4
sudo usermod -aG video $USER     # if /dev/video0 is not readable
./build/camstream -e hw:/dev/video11   # Pi hardware encoder
```

Notes:

- The bcm2835 hardware encoder exposes `/dev/video11` (memory-to-memory H.264
  M2M). It accepts NV12 (preferred) or YU12; the code handles both and
  interleaves I420 to NV12 in the input path. Output is Annex B
  directly from the capture queue.
- On 32 bit Raspberry Pi OS the V4L2 M2M API works with the 32 bit
  userspace; the 64 bit Pi OS image works the same way.
- The Pi camera module is managed by libcamera. On recent Raspberry
  Pi OS the sensor shows up as a V4L2 device, usually `/dev/video0`.
  List what is present with:

```
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
```

Only one process can use the sensor at a time, so stop any running
camera application (`libcamera-hello`, `still`, etc.) before starting
camstream:

```
./build/camstream -d /dev/video0 -e hw:/dev/video11
```

- CPU: with `-e sw` the Pi can encode 480p/30 comfortably; 720p/30 is
  possible but warm. Prefer `-e hw` for 1080p.
- The test pattern works without a camera for a full pipeline check:

```
./build/camstream --test -e hw:/dev/video11
./build/camstream --test -e sw
```

## Verification checklist

1. Build:

```
make -j$(nproc)
# expect: built build/camstream (x264: yes)
```

2. No camera, software encode:

```
./build/camstream --test -e sw
```

3. Open `http://<server-ip>:8080/` in a browser on the same LAN.
   Expect the test pattern within about 2 s. The server log must show
   `ICE validated`, `DTLS connected, SRTP keys derived`,
   `streaming video`.

4. Check `/status` in a second browser tab or with curl:

```
curl -s http://<server-ip>:8080/status | head -c 400
```

   The session state should be `streaming` and `packets_sent` should
   be growing.

5. Real camera:

```
./build/camstream -d /dev/video0
```

6. Second viewer: open the URL in another browser (or a second
   machine). Both must stream; `/status` lists both sessions on UDP
   ports 50000 and 50001.

7. Refresh the page a few times: each reload creates a new session,
   the old one is closed, a fresh keyframe arrives within the keyframe
   interval.

## Troubleshooting

Full reference: `docs/17_troubleshooting.md`. The short version:

| Symptom | Likely cause | Check |
| --- | --- | --- |
| Server starts, browser shows `waiting for offer` / nothing | Wrong URL or firewall | curl the UI from the client machine; open the HTTP port |
| `session create failed (UDP port busy?)` | Port in use or limit | `ss -ulnp | grep 500`, use `-u` to change the base port |
| Offer accepted, no video, log stops after `signaling complete` | ICE never validated | Firewall between the machines dropping UDP 50000 to 50007; candidate IP not reachable from the browser (multi homed server: check `Host` header IP) |
| `ICE validated` but no `DTLS connected` | Fingerprint mismatch or DTLS blocked | UDP must be open both ways; server clock not relevant (no timestamps in DTLS); check `verbose` output |
| `DTLS connected` but no video | Encoder produced nothing | Run with `--test` to isolate; check `/status` `encoded_frames` grows; `-e sw` needs x264 built in |
| Video but choppy | Bitrate for the link | Lower `-b`, or lower resolution; check `nacks`/`retx` counters in `/status` |
| `no usable encoder` | Hardware encoder not found, no x264 | `v4l2-ctl -d /dev/video11 --list-formats-ext`; rebuild with x264 or `-e sw` |
| Camera won't open | Permissions or busy | `video` group, `v4l2-ctl -d /dev/video0 --list-formats-ext`, close other users of the device |
| Works in Firefox, not Chromium (or vice versa) | Should not happen after 2.0 | Check the server log candidate and fingerprint lines; both must be well formed |
| Only first viewer gets video | Keyframe gap | Second viewer joins between keyframes: wait up to `-K` seconds, or reload |

Server log messages worth knowing:

- `rtc <id>: ICE validated (ip:port)`: first valid STUN check, peer
  locked.
- `rtc <id>: DTLS connected, SRTP keys derived`: media can flow.
- `rtc <id>: streaming video`: first RTP packets sent.
- `rtc <id>: keyframe requested (pli/fir)`: viewer asked for a refresh.
- `rtc <id>: idle timeout` / `closed`: session ended.

## libpeer Phase 3 - Fully Migrated Runtime

Two backends are now available:

- **Native** (`build/camstream`): ICE-lite + DTLS 1.2 + SRTP + RTP H.264 in own C code,
  deps `libssl-dev`, `libsrtp2-dev`, `libx264-dev`, vendored `mongoose.c` - **minimal**
- **libpeer** (`build/camstream-libpeer`): sepfy/libpeer PeerConnection (mbedTLS + bundled
  libsrtp, usrsctp, cJSON), same V4L2/test pipeline and Mongoose signaling - **spec compliant**

Build native (minimal):
```sh
sudo apt install -y build-essential libssl-dev libsrtp2-dev libx264-dev
make -j2
./build/camstream --test --encoder sw --listen 0.0.0.0 --http-port 8080
```

Build libpeer (fully migrated runtime per Phase 3 spec):
```sh
sudo apt install -y git cmake build-essential libx264-dev
make libpeer
make camstream-libpeer -j2
./build/camstream-libpeer --test --encoder sw --listen 0.0.0.0 --http-port 8000
```

The libpeer binary:
- Uses `src/webrtc/webrtc_session_libpeer.c` + `libpeer_global.c` (peer_init, PeerConnection per viewer)
- Video path: V4L2/test -> FrameHub -> H264 (V4L2 M2M or x264) -> AU ring -> `peer_connection_send_video()`
- Signaling: Mongoose `POST /rtc/offer` with raw SDP, answer from `peer_connection_create_answer()`
- Media: libpeer manages UDP, ICE host candidates, DTLS-SRTP internally, `peer_connection_loop()` in main tick
- Browser: same `RTCPeerConnection` JS, works offline RJ45 `192.168.1.10 <-> 192.168.1.20`

Details: `docs/21_libpeer_runtime.md` (new) and `docs/18_libpeer_phase3.md` (original evaluation).

## Repository layout

```
include/
  app/          app_config.h, app_server.h
  media/        frame_pool.h, frame_hub.h, au_ring.h, video_source.h,
                yuv_convert.h, h264_encoder.h,
                source_worker.h, encoder_worker.h
  webrtc/       ice_lite.h, dtls_srtp.h, rtp_h264.h, rtcp.h, sdp.h,
                webrtc_session.h
  vision/       frame_matrix.h, vision_worker.h (profiles, motion search,
                newest-frame mosaic worker)
src/
  camstream_main.c   entry point, signal handling
  app/               app_config.c (CLI), app_server.c (HTTP + main loop),
                     web_ui.c (embedded viewer page)
  media/             frame_pool.c, frame_hub.c, au_ring.c,
                     v4l2_source.c, test_source.c, yuv_convert.c,
                     h264_encoder.c (dispatcher), encoder_x264.c,
                     encoder_v4l2m2m.c, source_worker.c, encoder_worker.c
  webrtc/            ice_lite.c (STUN, UDP demux), dtls_srtp.c (OpenSSL
                     DTLS + RFC 5764 export + libsrtp2), rtp_h264.c
                     (RFC 6184 packetizer), rtcp.c (SR/RR/PLI/FIR/NACK),
                     sdp.c (offer parse, answer build),
                     webrtc_session.c (session state machine)
third_party/
  mongoose/        Mongoose 7.x (HTTP only)
docs/
  10_architecture.md      detailed data flow and threading
  11_webrtc_internals.md  protocol layer by layer
  12_build_reference.md   dependency and build notes
  13_lan_two_laptops.md   LAN setup walkthrough
  14_raspberry_pi.md      Pi setup in depth
  15_optimization_notes.md
  16_protocol_reference.md  SDP/STUN/DTLS/SRTP/RTP/RTCP tables
  17_troubleshooting.md   symptom by symptom guide
  18_libpeer_phase3.md     libpeer build and migration boundary
  19_execution_roadmap.md  phases 1 through 5 and acceptance checklist
  20_webrtc_zero_latency.md offline Pi-to-laptop WebRTC deployment
Makefile
```

Vision smoke pipeline:

```
make vision-capture
./build/vision-capture --test -W 640 -H 480 -F 30 -s 10 -o build/mosaic
```

This writes a grayscale PGM and an intensity-derived OBJ mosaic.

## Version

2.0.0. WebRTC only: the 1.x WebSocket/HTTP chunked video transport is
gone; if a page, script or note in your setup still references
`/stream` or `ws://` it belongs to 1.x.
