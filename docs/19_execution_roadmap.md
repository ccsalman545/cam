# Project execution roadmap

This document reconciles the original five-phase outline with the current
checkout. The repository has moved beyond the original Phase 2 prototype:
`camstream` is now the integrated C server, and its defaults are HTTP `8080`
and media UDP `50000` through `50007` rather than HTTP `8000`.

## Phase 1 - direct Raspberry Pi link

### Pi

Assign the wired interface, replacing `eth0` with the result of `ip -br link`:

```sh
sudo ip addr flush dev eth0
sudo ip addr add 192.168.1.10/24 dev eth0
sudo ip link set eth0 up
ip -br addr show dev eth0
```

### Host PC

```sh
sudo ip addr flush dev eth0
sudo ip addr add 192.168.1.20/24 dev eth0
sudo ip link set eth0 up
ping -c 10 -W 1 192.168.1.10
```

A direct cable does not need a gateway, DNS or Internet access. A measured
sub-millisecond ping is useful, but do not make it a hard correctness gate:
Linux scheduling and power management can produce occasional values above
one millisecond even on a healthy direct link. Require zero packet loss and
stable results instead.

Camera smoke test:

```sh
rpicam-still -n -t 1000 -o test.jpg
# Older Raspberry Pi OS:
libcamera-still -n -t 1000 -o test.jpg
```

For a CSI camera on modern Raspberry Pi OS, prefer `rpicam-*`; the old
`raspi-config` camera-interface toggle is not required on current libcamera
systems. Verify the actual nodes with `v4l2-ctl --list-devices`.

## Phase 2 - Mongoose server

The current server embeds Mongoose and already serves the dashboard and
signaling API. It does not use a `/web_root` directory because the page is
compiled into `src/app/web_ui.c`, which keeps the offline deployment to one
binary.

```sh
sudo apt install -y build-essential libssl-dev libsrtp2-dev libx264-dev
make -j2
./build/camstream --test -e sw --listen 0.0.0.0 --http-port 8080
```

Open `http://192.168.1.10:8080/` from the host. The old outline's port 8000
is valid only for a separate Phase 2-only server; do not run both servers on
the same port.

## Phase 3 - WebRTC/libpeer decision

The existing server already provides the complete browser video path with
V4L2/test capture, H.264, RTP, ICE-lite, DTLS-SRTP, RTCP and a native
`RTCPeerConnection` client. The optional libpeer build is now available:

```sh
make libpeer
# ARM64 cross compilation:
TARGET=aarch64-linux-gnu make libpeer
```

Read `docs/18_libpeer_phase3.md` before replacing the current WebRTC stack.
It explains the signaling/media callback boundary and why two stacks must not
be initialized in one executable. The recommended order is synthetic H.264,
then the AU callback, then real V4L2/libcamera frames.

For the current implementation, verify:

```sh
./build/camstream --test -e sw -W 640 -H 480 -F 30
curl -s http://192.168.1.10:8080/status
```

Allow both signaling and media on the Pi firewall:

```sh
sudo ufw allow 8080/tcp
sudo ufw allow 50000:50007/udp
```

The browser must access the Pi by its Ethernet IP, not `localhost`.

## Phase 4 - raw matrix extraction

The new `include/vision/frame_matrix.h` and
`src/vision/frame_matrix.c` provide a dependency-free foundation:

- YUYV to 8-bit grayscale conversion
- RGB24 to grayscale conversion
- Row and column intensity profiles
- Integer translation search using overlap-constrained SAD
- No allocation in the conversion/profile/search hot loops

Run the unit test:

```sh
make vision-test
```

Run the integrated newest-frame capture/vision pipeline without a camera:

```sh
make vision-capture
./build/vision-capture --test -W 640 -H 480 -F 30 -s 10 -o build/mosaic
# writes build/mosaic.pgm and build/mosaic.obj
```

Use a real V4L2 camera by replacing `--test`:

```sh
./build/vision-capture --device /dev/video0 -W 1024 -H 768 -F 30 -s 0 -o build/mosaic
```

`-s 0` runs until Ctrl-C. The worker uses a keep-newest mailbox, so a slow
matrix operation drops stale frames rather than blocking camera capture.

The current camera source produces YUYV or planar YU12. For a raw processing
pipeline, consume the frame before H.264 conversion, copy it into a buffer
owned by the vision worker, then release the V4L2 buffer. Never retain a V4L2
pointer after `VIDIOC_QBUF` or the next capture call.

For 1024x768 grayscale, allocate at least:

```text
1024 * 768 = 786,432 bytes per frame
```

Use a bounded queue or newest-frame mailbox so matrix processing cannot stall
camera capture indefinitely.

## Phase 5 - spatial mapping and stitching

The matrix module includes `VisionMosaic`, which maintains a sum and sample
count per output pixel. Overlaps are averaged and can be exported as:

- Binary PGM (`P5`) for inspection and image tooling
- Regular-grid OBJ vertices/faces for Blender or WebGL conversion

The intended loop is:

```text
previous frame
      + current frame
      -> vision_estimate_offset(max_dx, max_dy)
      -> update cumulative origin
      -> vision_mosaic_add(current, origin_x, origin_y)
      -> periodic PGM/OBJ export
```

This is an integer translational baseline, not a calibrated DEM system. For
real elevation measurements, add camera calibration, lens distortion
correction, scale/pose estimation and a defined sensor-to-world model.
Intensity alone is not depth. If the camera is moving in a general 3D scene,
2D correlation can fail at occlusions, rotation, parallax or illumination
changes; record the correlation error and reject bad offsets rather than
silently stitching them.

## Acceptance checklist

- [ ] Pi and host use `192.168.1.10/24` and `192.168.1.20/24`.
- [ ] Ten pings complete with zero loss.
- [ ] Camera smoke frame is readable.
- [ ] `make vision-test` passes.
- [ ] `make` succeeds with OpenSSL/libsrtp2/x264 development packages.
- [ ] Dashboard loads over Ethernet at port 8080.
- [ ] Browser reaches ICE, DTLS/SRTP and decoded video.
- [ ] `/status` counters increase while streaming.
- [ ] Matrix processing does not cause capture backlog.
- [ ] PGM and OBJ outputs open correctly.
- [ ] Latency is measured with a visible moving LED/clock, not estimated
      only from network ping.
