# libpeer runtime

[camstream docs](README.md) / 21. libpeer runtime &nbsp;·&nbsp; [README](../README.md)

This document describes the **fully migrated libpeer runtime** (`camstream-libpeer`),
which replaces the native OpenSSL/libsrtp2 stack with sepfy/libpeer (mbedTLS +
libsrtp + usrsctp + cJSON).

## Why two backends?

| Backend | Transport | Dependencies | Binary | Use case |
|---------|-----------|--------------|--------|----------|
| **native** (`build/camstream`) | ICE-lite + DTLS 1.2 + SRTP + RTP H.264 (own C impl) | `libssl-dev`, `libsrtp2-dev`, `libx264-dev`, vendored `mongoose.c` | minimal, auditable ~6000 LOC | **Minimal deps**, offline Pi, direct RJ45 |
| **libpeer** (`build/camstream-libpeer`) | libpeer PeerConnection (mbedTLS + bundled deps) | `cmake`, `git`, `libx264-dev`, vendored `mongoose.c` + libpeer builds its deps | larger, but pure C WebRTC library | **Spec compliance** - Phase 3 requires libpeer |

If your main requirement is "don't want too many libraries", use **native**.
If you need libpeer API for IoT/embedded, use **libpeer**.

## Phase 2: Mongoose Web Server

Both binaries use Mongoose 7.x vendored in `third_party/mongoose/`:

- **Native default:** `http://0.0.0.0:8080/` (configurable)
- **libpeer default:** `http://0.0.0.0:8000/` per your spec (Phase 2 says 8000)

Build:
```sh
gcc -O2 -Wall server.c mongoose.c -o web_server
# In this repo:
make -j2                    # native -> build/camstream
make camstream-libpeer -j2  # libpeer -> build/camstream-libpeer (stub if libpeer not built)
```

The server serves the embedded dashboard (`src/app/web_ui.c`) - single file, no CDN,
no external fonts, works offline. It also serves `/status` and `/rtc/offer`.

Milestone: open `http://PI_IP:8000/` or `http://PI_IP:8080/` in PC browser, dashboard loads instantly.

## Phase 3: libpeer Integration

### Build libpeer (requires network, once)

```sh
sudo apt install -y git cmake build-essential
make libpeer
# or pinned ref:
LIBPEER_REF=5b849de378545c31d34759a145413846953e1366 make libpeer
# ARM cross:
TARGET=aarch64-linux-gnu make libpeer
```

This clones `https://github.com/sepfy/libpeer.git` into `build/libpeer-src`,
builds Release in `build/libpeer` with `dist/` containing `libpeer.a`,
`libsrtp2.a`, `libmbedtls.a`, etc. Both dirs are git-ignored.

### Build camstream-libpeer

```sh
make camstream-libpeer -j$(nproc)
# Output: build/camstream-libpeer
```

If `build/libpeer/dist/lib/libpeer.a` exists, it links real libpeer.
If not, it builds a **stub** that compiles but returns "libpeer not built"
at runtime - useful for CI/offline.

### Video Pipeline

```text
Pi camera (/dev/video0) or test pattern
  -> V4L2 capture thread (v4l2_source.c / test_source.c)
  -> newest-frame FrameHub (frame_hub.c)
  -> H.264 encoder (V4L2 M2M bcm2835-codec or libx264 zerolatency)
  -> Annex-B AU ring (au_ring.c)
  -> libpeer: peer_connection_send_video() (RTP H.264 inside libpeer)
  -> ICE host candidates + DTLS-SRTP (mbedTLS) + UDP
  -> browser RTCPeerConnection -> <video>
```

Mongoose is still only for UI + SDP signaling (`POST /rtc/offer`).
Media is **pure UDP**, no WebSocket video transport.

### Signaling Flow (libpeer as answerer)

1. Browser creates offer via `pc.createOffer()` (recvonly video)
2. `fetch('/rtc/offer', {sdp: offer.sdp})` to Pi
3. Pi: `peer_connection_create()` with `video_codec=CODEC_H264`
4. Pi: `peer_connection_set_remote_description(offer, SDP_TYPE_OFFER)`
5. Pi: `answer = peer_connection_create_answer(pc)`
6. Pi returns `{type:"answer", sdp:answer, session_id, udp_port}` JSON
7. Browser: `pc.setRemoteDescription(answer)`
8. libpeer gathers host candidates via `onicecandidate` callback, ICE checks flow
9. libpeer DTLS handshake, SRTP keys derived internally
10. Encoder thread pushes frames via `peer_connection_send_video()`

No STUN/TURN server, `iceServers:[]`, direct RJ45 `192.168.1.10 <-> 192.168.1.20`.

### Browser Client

`src/app/web_ui.c` embedded HTML:

```js
pc = new RTCPeerConnection({iceServers:[]});
pc.addTransceiver('video',{direction:'recvonly'});
pc.ontrack = ev => { vid.srcObject = ev.streams[0]; };
offer = await pc.createOffer();
await pc.setLocalDescription(offer);
r = await fetch('/rtc/offer',{method:'POST', body:JSON.stringify({sdp:offer.sdp})});
answer = await r.json();
await pc.setRemoteDescription({type:'answer', sdp:answer.sdp});
```

Renders into `<video autoplay playsinline>`, plus stats from `getStats()`.

### Milestone Check

Pi:
```sh
./build/camstream-libpeer --test --encoder sw --listen 0.0.0.0 --http-port 8000 --verbose
# or real camera:
./build/camstream-libpeer --device /dev/video0 --encoder auto --listen 0.0.0.0 --http-port 8000
```

Laptop (direct RJ45):
```text
http://192.168.1.10:8000/
```

Expected logs:
```text
camstream 2.0.0 ready [libpeer backend - mbedTLS + libsrtp + usrsctp]
libpeer: initialized
rtc 12345678 [libpeer]: created
rtc 12345678 [libpeer]: ice state checking
rtc 12345678 [libpeer]: ice state connected
rtc 12345678 [libpeer]: streaming video
```

Browser:
- ICE `connected`/`completed`
- `framesDecoded` increasing
- bitrate >0

Firewall:
```sh
sudo ufw allow 8000/tcp
sudo ufw allow 50000:50100/udp
```

### Minimal Dependencies Argument

**Native** is minimal:
- `libssl-dev` (DTLS), `libsrtp2-dev` (SRTP), `libx264-dev` (SW encode), `mongoose.c` vendored
- No GStreamer, no Docker, no Node, no libpeer bundled deps

**libpeer** is heavier but spec-compliant:
- Builds `mbedtls`, `libsrtp2`, `usrsctp`, `cJSON` via CMake ExternalProject
- Binary links 7 static libs
- Still no GStreamer (we use V4L2 directly, unlike upstream raspberrypi example which uses GStreamer)

Choose based on your constraint.

### Switching Backends

```sh
# native (minimal)
./build/camstream --test -e sw -l 0.0.0.0 -p 8080

# libpeer (spec)
./build/camstream-libpeer --test -e sw -l 0.0.0.0 -p 8000
```

Both share same web UI and `/status` API, `transport` field shows `webrtc` vs `webrtc-libpeer`.

### Known Limitations (libpeer)

- libpeer manages its own UDP socket per PeerConnection, not the `udp_base_port` pool
  (native uses one port per viewer from 50000). Firewall rule should allow wide UDP range.
- No NACK retransmission stats exposed (native has `retx_data` cache)
- Answer SDP includes libpeer-generated fingerprint, not global `dtls_srtp_local_fingerprint()`
- Trickle ICE not yet wired via HTTP - host candidates gathered internally work for direct LAN,
  but for NAT you would need to forward `onicecandidate` via WebSocket/HTTP.

For offline RJ45, host candidates are sufficient.

### Verification Checklist

- [ ] `make libpeer` succeeds (needs network)
- [ ] `make camstream-libpeer -j2` builds `build/camstream-libpeer`
- [ ] Pi and laptop have static IPs `192.168.1.10/24` and `192.168.1.20/24`, RJ45 direct
- [ ] Pi: `./build/camstream-libpeer --test --encoder sw --listen 0.0.0.0 --http-port 8000 --verbose`
- [ ] Laptop: `http://192.168.1.10:8000/` loads dashboard
- [ ] Click Start WebRTC, ICE -> connected, video flows, latency <150ms glass-to-glass
- [ ] `curl http://192.168.1.10:8000/status` shows `"transport":"webrtc-libpeer"` and sessions `streaming`

---

| | | |
|---|---|---|
| **Previous**<br>[20. Offline Ethernet deployment](20_webrtc_zero_latency.md) | **Index**<br>[docs](README.md) | **Next**<br>[22. Janus transport](22_janus_transport.md) |
