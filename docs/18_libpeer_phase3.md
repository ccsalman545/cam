# Phase 3: libpeer evaluation and integration boundary

## Important result of the code review

This checkout already contains a complete native WebRTC sender. The current
runtime path is:

```text
V4L2/test source -> frame hub -> H.264 (V4L2 M2M or x264)
                 -> RTP H.264 -> DTLS-SRTP -> ICE-lite UDP -> browser
                         Mongoose HTTP serves the page and SDP signaling
```

It is **not** currently a libpeer application. It has its own audited,
small implementations of SDP, ICE-lite, DTLS-SRTP, RTP H.264 and RTCP. That
is not a cosmetic distinction: upstream libpeer brings a second WebRTC stack,
uses mbedTLS rather than OpenSSL, and builds its own copies of libsrtp,
usrsctp and cJSON. Linking it into this executable while retaining the
existing stack would create duplicate ownership of UDP, ICE, DTLS, SRTP,
threads and retransmission state.

Therefore Phase 3 is split into two safe milestones:

1. **Build milestone (implemented here):** reproducibly fetch and build
   libpeer for native Linux or ARM Linux, without committing generated
   sources or replacing the working stream.
2. **Runtime migration milestone (not silently faked):** choose one stack,
   then port the camera/encoder callbacks and signaling adapter to that
   stack. The two stacks must not send on the same peer connection.

The `camstream` default remains the existing implementation because it
already satisfies the browser video milestone and can be tested without an
external signaling service.

## Build libpeer

On a Debian/Raspberry Pi OS host:

```sh
sudo apt update
sudo apt install -y git cmake build-essential
make libpeer
```

This runs `tools/setup-libpeer.sh`. It clones the upstream repository into
`build/libpeer-src`, checks out a pinned upstream commit, initializes its
submodules and builds a Release tree in `build/libpeer`. Both paths are
ignored by Git. Override the pin when deliberately updating:

```sh
LIBPEER_REF=<reviewed-commit-or-tag> make libpeer
```

For a 64-bit ARM cross build from x86 Linux:

```sh
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
TARGET=aarch64-linux-gnu make libpeer
```

For a sysrooted build, provide a CMake toolchain file:

```sh
CMAKE_TOOLCHAIN_FILE=$PWD/aarch64-pi.cmake make libpeer
```

The upstream build uses CMake ExternalProject dependencies. The first build
needs network access and can take substantially longer than camstream. Do
not use `git clone` in the application build itself: builds should be
repeatable and an unavailable network must not break the existing target.

## Why this is not wired into `camstream` yet

The upstream libpeer public examples use a signaling URL (WHIP or the
project's test service). The browser in this repository uses a local Mongoose
HTTP API, `POST /rtc/offer`, and the server's answer advertises a per-viewer
UDP candidate. A real adapter must explicitly map:

| Existing camstream contract | libpeer migration work |
| --- | --- |
| `SdpOffer` parsed from `/rtc/offer` | libpeer offer/answer and ICE callback API |
| Mongoose HTTP handler | libpeer signaling callback or a Mongoose WHIP/WebSocket adapter |
| `AuRing` Annex-B access units | libpeer H.264 video-track/input callback |
| one UDP socket/session per viewer | one libpeer peer object per viewer |
| `force_idr` on PLI/FIR | libpeer keyframe/request callback |
| `/status` counters | libpeer connection/media state callbacks |

Do not pass a raw Annex-B frame to an API that expects an encoded sample with
its own timestamp/length contract. Do not feed V4L2 buffers after they have
been returned to the capture queue. The integration boundary must copy or
reference-count a frame until libpeer has finished consuming it.

## Recommended migration order

1. Keep the current stack as the reference implementation and record a
   browser `chrome://webrtc-internals` run on Ethernet.
2. Build and run the upstream Raspberry Pi example with its test page. This
   validates the ARM toolchain, H.264 format and libpeer's own dependencies
   independently of the application.
3. Add a compile-time `STREAM_BACKEND=libpeer` executable or branch, not a
   runtime toggle that initializes both stacks.
4. First replace only the session/signaling layer using a single synthetic
   H.264 access-unit source. Verify SDP, ICE, DTLS, SRTP and decoded frames.
5. Replace the synthetic source with the existing `AuRing` callback, then
   connect V4L2/libcamera output and keyframe feedback.
6. Compare latency, frame drops, bitrate, CPU and memory against the current
   backend before deleting any existing code.

## Milestone acceptance test

Use the current backend as the baseline:

```sh
./build/camstream --test -e sw -W 640 -H 480 -F 30
curl -s http://PI:8080/status | jq .
```

Then test the libpeer migration with the same camera and resolution. Accept
only when all of these are true:

- the page and signaling use the Pi's Ethernet address, not localhost;
- TCP 8080 and the selected UDP media range are allowed by the firewall;
- browser ICE reaches `connected`/`completed`, then DTLS/SRTP reaches
  connected;
- `framesDecoded` increases continuously and there are no repeated PLI or
  decoder reset loops;
- measured glass-to-glass latency and drops are no worse than the baseline;
- stopping the browser releases the libpeer peer and camera cleanly.

“Near-zero latency” should be measured, not inferred from a successful SDP
exchange. Ethernet removes wireless variability, but it does not remove
capture buffering, encoder GOP delay, browser decode scheduling or display
vsync.
