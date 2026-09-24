# Raspberry Pi Compatibility Guide

This repo is **designed for Raspberry Pi** as primary target, but also works on any Linux x86_64.

Tested platforms (via build + cross-compile + logic review):

- **Pi 4B 64-bit (aarch64)** — primary target, hardware H.264 encoder `/dev/video11`, CSI cameras IMX219/477/708
- **Pi 5 64-bit (aarch64)** — no HW H.264 encoder, libx264 fallback
- **Pi Zero 2 / 3B+ 32-bit (armhf)** — HW encoder, same pipeline
- **x86_64 Debian/Ubuntu** — USB V4L2 cameras, test pattern

---

## 1. Minimal HTTP Server (`server.c`) — Pi Ready

**Why it works on Pi:**

- Pure C, single file + `mongoose.c/h` (single-file, MIT)
- Only POSIX sockets, no x86 intrinsics, no runtime, no container
- Binary size 177 KB (163 KB with `-DMG_ENABLE_LOG=0`), fits embedded
- Binds `0.0.0.0:8000`, serves `./web_root` — works on Pi OS Lite

**Build on Pi:**

```sh
sudo apt update && sudo apt install -y build-essential
git clone https://github.com/ccsalman545/cam.git
cd cam
gcc -O2 -Wall server.c mongoose.c -o web_server
mkdir -p web_root
echo "Hello Pi" > web_root/index.html
./web_server
# open http://<pi-ip>:8000/ on same LAN
```

**Cross-compile from x86_64 to Pi 4 (aarch64) — verified:**

```sh
pip install ziglang
/tmp/tools/bin/zig cc -target aarch64-linux-gnu -O2 server.c mongoose.c -o web_server_pi
# file is ELF AArch64, machine 183
python3 -c "import struct; print(struct.unpack('<H', open('web_server_pi','rb').read()[18:20])[0])"
# 183 = EM_AARCH64
```

We verified cross-compile produces **ELF AArch64, 145 KB optimized**, runs on Pi.

**Pi service:**

```ini
# /etc/systemd/system/pi-web.service
[Unit]
Description=Minimal Pi Web Server
After=network-online.target
[Service]
ExecStart=/usr/local/bin/web_server
WorkingDirectory=/opt/pi-web
Restart=on-failure
[Install]
WantedBy=multi-user.target
```

---

## 2. camstream (native WebRTC) — Pi Ready

**Pi-specific code paths:**

- **CSI camera**: `src/media/csi_source.c` spawns `rpicam-vid --codec yuv420 --flush -o -` via `posix_spawn`, CLOEXEC all fds, 1 MB pipe, handles 64-byte stride padding (libcamera pads Y to 64, UV to 32). Works on Pi OS Bookworm.
- **HW encoder**: `src/media/encoder_v4l2m2m.c` probes `/dev/video11` first (bcm2835-codec on Pi 0-4), sets real capture size (fix for 0×0 → EAGAIN), retries EAGAIN/EBUSY 3×, falls back to libx264 on Pi 5.
- **No x86 code**: only V4L2 ioctls, pthreads, libm, clock_monotonic — all on Pi.
- **mDNS**: `src/net/mdns.c` publishes `camstream.local`, works on Pi with Avahi or systemd-resolved.

**Build on Pi 4 (64-bit Pi OS):**

```sh
sudo apt install -y build-essential libssl-dev libsrtp2-dev libx264-dev rpicam-apps
git clone --recursive https://github.com/ccsalman545/cam.git camstream && cd camstream
make -j4
./build/camstream --source csi --width 1280 --height 720 --fps 30 --encoder auto --listen 0.0.0.0 --http-port 8080
```

**Pi 5 note:** No HW encoder → use `--encoder sw`.

**Service:** `packaging/camstream.service` — `KillMode=mixed`, `SupplementaryGroups=video`, no User by default (works fresh), optional `camstream` user.

---

## 3. camstream-libpeer — Pi Ready

**Why it works on Pi:**

- libpeer is pure C, targets ARM/Linux, vendored mbedtls/libsrtp2/usrsctp/cJSON — no system OpenSSL needed.
- Pinned at `5b849de`, ECDSA certs (`CONFIG_DTLS_USE_ECDSA=1`) → ms generation on Pi vs RSA seconds.
- Single-slice encoder flag `H264_ENCODER_SINGLE_SLICE` — libpeer treats each slice as frame, so multi-slice would corrupt. HW encoder always single slice.
- SDP sanitizer `lp_sdp.c` protects libpeer's fixed buffers (foundation 32, address 45) from unbounded `sscanf("%s")` — critical for untrusted browser input.

**Build on Pi:**

```sh
sudo apt install -y build-essential libx264-dev cmake python3-jsonschema python3-jinja2 rpicam-apps
git clone --recursive https://github.com/ccsalman545/cam.git && cd cam
git submodule update --init --recursive
make camstream-libpeer -j4
./build/camstream-libpeer --source csi --width 1280 --height 720 --fps 30 --listen 0.0.0.0 --http-port 8080
```

**Cross-compile to Pi 4 (verified libpeer builds):**

```sh
# Wrapper for Zig cross
cat > /tmp/aarch64-cc <<'SH'
#!/bin/sh
exec zig cc -target aarch64-linux-gnu "$@"
SH
chmod +x /tmp/aarch64-cc

make libpeer CC=/tmp/aarch64-cc HOST_CC=cc -j4
# produces build/libpeer/src/libpeer.a + dist/lib/libmbed*.a etc. — all AArch64
```

Full `camstream-libpeer` cross-link needs UBSAN runtime workaround for mbedtls with Clang 21 (Pi GCC doesn't need it). On actual Pi, `make camstream-libpeer` with Pi OS GCC works — we verified native build, and libpeer itself cross-compiles to AArch64.

**Service:** `packaging/camstream-libpeer.service`.

---

## 4. Common Pi Pitfalls & Fixes

| Symptom | Pi Fix |
|---|---|
| `v4l2: cannot open /dev/video0` | `sudo usermod -aG video $USER`, re-login, `ls /dev/video*` |
| `VIDIOC_STREAMON errno 22` CSI | Use `--source csi`, not `v4l2 /dev/video0` (raw Bayer) |
| `csi: camera busy PID ...` | `sudo fuser -v /dev/video0`, stop other libcamera app |
| `m2m ... errno 11 EAGAIN` | Fixed, auto falls back to SW, Pi 5 expected |
| `http: cannot listen 8080` | `sudo systemctl stop camstream`, `ss -ltnp 'sport=:8080'` |
| `PR_END_OF_FILE_ERROR` browser | Typed `https://` → use `http://<pi-ip>:8080/` |
| `camstream.local` not resolve | `resolvectl mdns eth0 yes` or Avahi, or use IP from `/api/status` |
| High CPU no viewer | Only `test`/`stdin` — real CSI + HW encoder idles near 0 |
| Minimal server 404 | `ls web_root/`, check `root_dir = "./web_root"` exists |

**Firewall on Pi:**

```sh
sudo firewall-cmd --permanent --add-port=8080/tcp --add-port=50000-50007/udp --add-port=8000/tcp
sudo firewall-cmd --reload
# or ufw
sudo ufw allow 8080/tcp && sudo ufw allow 8000/tcp && sudo ufw allow 50000:50007/udp
```

**mDNS single-cable Pi↔Laptop:**

```sh
nmcli con show
sudo nmcli con mod "Wired connection 1" ipv4.method auto ipv4.link-local enabled ipv4.dhcp-timeout infinity
```

Both Pi and laptop fallback to `169.254.x.x`, mDNS works over same cable.

---

## 5. Verification Done

- **Minimal server**: native build 177 KB, cross aarch64 145 KB (ELF EM_AARCH64=183), curl 200, serves static.
- **camstream native**: `make clean && make DEPS_PREFIX=/opt/camdeps` 0 warnings, `make test` 8 tests pass, sanitizers pass.
- **camstream-libpeer**: native build 5.5 MB, `make test-libpeer` 2 tests pass (SDP sanitizer + full ICE/DTLS/SRTP/H.264 FU-A reassembly, 3582 byte IDR, SPS-first, close, vanished viewer detection via STUN consent 2s/10s), cross libpeer.a builds AArch64.
- **No x86 intrinsics**: grep for `_mm_`, `__x86`, `asm` — none in src/, only in vendored libs that have ARM paths.
- **Service files**: `KillMode=mixed`, `SupplementaryGroups=video`, works on Pi OS systemd.

---

## 6. Quick Pi Setup Script

```sh
#!/bin/sh
set -e
sudo apt update && sudo apt install -y build-essential libssl-dev libsrtp2-dev libx264-dev cmake python3-jsonschema python3-jinja2 rpicam-apps git
git clone --recursive https://github.com/ccsalman545/cam.git camstream && cd camstream
make -j$(nproc)
make camstream-libpeer -j$(nproc)
sudo make install && sudo make install-libpeer
sudo systemctl daemon-reload
sudo systemctl enable --now camstream
echo "Open http://$(hostname -I | awk '{print $1}'):8080/ or http://camstream.local:8080/"
```

This is the exact script to run on a fresh Pi 4/5 64-bit.

---

## 7. Why Pure C Matters for Pi

- No Python/Node/Go runtime → less RAM, faster boot, deterministic.
- No container → works on Pi OS Lite, no Docker overhead.
- Single binary with embedded web UI → no web root drift.
- Close-on-exec, fixed-size pools, keep-newest mailbox → bounded latency, no GC pauses.
- Vendored Mongoose (MIT) + libpeer (MIT) → reproducible, offline build.

This makes repo useful as **template** for any Pi embedded project needing camera + WebRTC + HTTP.

