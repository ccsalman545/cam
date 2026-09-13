# WebRTC-first offline Ethernet deployment

The WebRTC path is the primary product path for this project. The matrix and
libpeer work must not be allowed to interfere with it.

## Actual media path

```text
Pi camera (/dev/video0)
  -> V4L2 capture thread
  -> newest-frame FrameHub
  -> H.264 encoder (V4L2 M2M or libx264 zerolatency)
  -> Annex-B access unit ring
  -> RTP H.264 packetizer (RFC 6184)
  -> DTLS 1.2 SRTP (RFC 5764/libsrtp2)
  -> ICE-lite UDP socket
  -> laptop RTCPeerConnection
  -> <video>
```

Mongoose is used only for the UI and SDP HTTP signaling. It does not carry
video. The browser uses `RTCPeerConnection` with `iceServers: []` and a
`recvonly` video transceiver, so this works without Internet, STUN, TURN,
containers or cloud services.

## Fixed Ethernet setup

Pi:

```text
192.168.1.10/24
```

Laptop:

```text
192.168.1.20/24
```

Open the page by IP:

```text
http://192.168.1.10:8080/
```

Do not use `localhost`: the browser's localhost means the laptop, not the Pi.

Required traffic:

| Traffic | Protocol | Port |
| --- | --- | --- |
| UI and SDP offer/answer | TCP | 8080 |
| STUN, DTLS, SRTP, RTCP | UDP | 50000-50007 |

The application allocates one UDP port per viewer, beginning at `50000`.

## Recommended Pi command

Software encoder fallback:

```sh
./build/camstream \
  --device /dev/video0 \
  --width 640 \
  --height 480 \
  --fps 30 \
  --encoder sw \
  --bitrate 1500 \
  --keyframe 1 \
  --listen 0.0.0.0 \
  --http-port 8080 \
  --udp-port 50000 \
  --verbose
```

If the Pi exposes a working H.264 V4L2 M2M encoder, prefer:

```sh
./build/camstream \
  --device /dev/video0 \
  --width 1280 \
  --height 720 \
  --fps 30 \
  --encoder auto \
  --bitrate 3000 \
  --keyframe 1 \
  --verbose
```

Use the hardware device explicitly only after checking it:

```sh
v4l2-ctl --list-devices
./build/camstream --device /dev/video0 --encoder hw:/dev/video11
```

## Why this is low latency

The implementation intentionally uses:

- A newest-frame mailbox instead of an unbounded capture queue
- `x264` `zerolatency` tuning when software encoding is selected
- One x264 thread to avoid frame reordering and scheduling delay
- No B-frames and baseline H.264 output
- H.264 RTP packetization with MTU-sized FU-A fragments
- UDP instead of TCP for media
- ICE host candidate over the direct Ethernet link
- DTLS-SRTP directly on the media socket
- Periodic RTCP sender reports
- PLI/FIR-triggered keyframes
- NACK retransmission for packet loss

“Zero latency” is not literally possible because capture exposure, encode,
network scheduling, browser decode and display vsync all contribute delay.
The target is minimum practical glass-to-glass latency, not a zero timestamp.

## WebRTC correctness checks

When the page connects, the server log should progress through:

```text
signaling complete
ICE validated
DTLS connected, SRTP keys derived
streaming video
```

The browser dashboard should show:

- ICE `connected` or `completed`
- A remote H.264 track
- Increasing decoded-frame count
- Non-zero bitrate
- Stable packets-lost count

Server-side check:

```sh
curl -s http://192.168.1.10:8080/status
```

Expected fields include:

```json
{
  "transport": "webrtc",
  "sessions": [
    {
      "state": "streaming",
      "packets_sent": 1234,
      "bytes_sent": 456789
    }
  ]
}
```

For detailed browser diagnostics use Chromium `chrome://webrtc-internals`
or Firefox `about:webrtc`.

## Firewall and link verification

```sh
# Laptop
ping -c 20 192.168.1.10
curl -v http://192.168.1.10:8080/status

# Pi
sudo ss -ltnup | grep -E '8080|5000'
```

If the page loads but ICE stays in `checking`, TCP is working but UDP is
blocked or the answer advertises the wrong interface. Open the page through
the Pi Ethernet address and check the candidate IP in the server log.

## WebRTC code boundary

The files that are essential to the stream are:

```text
src/webrtc/ice_lite.c
src/webrtc/dtls_srtp.c
src/webrtc/rtp_h264.c
src/webrtc/rtcp.c
src/webrtc/sdp.c
src/webrtc/webrtc_session.c
src/app/app_server.c
src/app/web_ui.c
```

The libpeer build is optional and isolated. The vision worker is also an
optional separate consumer. Neither should replace, block or initialize a
second WebRTC stack in the production `camstream` process.
