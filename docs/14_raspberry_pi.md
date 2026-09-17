# Raspberry Pi guide

[camstream docs](README.md) / 14. Raspberry Pi guide &nbsp;·&nbsp; [README](../README.md)

camstream targets Raspberry Pi OS (Bookworm, Bullseye) on Pi 3, 4, 5 and Zero 2 W. 64 bit OS is recommended: libx264 and OpenSSL packages are faster and fully supported there.

## Why the hardware encoder matters

Software H.264 encoding of a 720p30 stream is beyond a Pi Zero 2 W and marginal on a Pi 3. The Pi has a dedicated hardware encoder exposed as a V4L2 memory-to-memory device. camstream probes for it automatically:

```mermaid
flowchart LR
    BOOT["camstream start\n--encoder auto"] --> SCAN["scan /dev/videoN"]
    SCAN --> Q{"M2M encoder\nwith H264 output?"}
    Q -- "yes: /dev/video11 on most Pi" --> HW["GPU encode\nNV12 or YU12 input\nnear zero ARM load"]
    Q -- no --> SW["libx264 software\nfallback"]
    HW --> RUN["streaming"]
    SW --> RUN
```

Check what your Pi exposes:

```bash
v4l2-ctl --list-devices          # from v4l2-utils package
# encoder lines appear as:
#   /dev/video11 (bcm2835-codec-encode)  on Pi 3/4
#   /dev/video31 on some configurations
```

You can pin it explicitly:

```bash
./build/camstream -e hw:/dev/video11 -W 1280 -H 720 -b 4000
```

If the hardware path misbehaves (rare, driver dependent), force software:

```bash
./build/camstream -e sw -W 640 -H 480 -b 1500
```

## Install and build

```bash
sudo apt update
sudo apt install build-essential libssl-dev libsrtp2-dev libx264-dev
cd ideal-broccoli
make
```

All three libraries are in the standard Raspberry Pi OS repositories. No other dependencies exist.

## Camera options

### USB webcam (simplest)

Works out of the box through plain V4L2:

```bash
ls /dev/video*                    # USB cams usually land on video0
./build/camstream -d /dev/video0
```

Permissions: your user must be in the `video` group:

```bash
sudo usermod -aG video $USER      # log out and back in afterwards
```

### Raspberry Pi Camera Module (CSI ribbon)

Modern Pi OS routes the CSI camera through libcamera, which does not present a YUYV V4L2 capture device by default. Two supported paths:

**Option A: v4l2loopback bridge (recommended)**

```bash
sudo apt install v4l2loopback-dkms rpicam-apps
sudo modprobe v4l2loopback video_nr=7 max_buffers=8

# feed the CSI camera into the loopback device
rpicam-vid -t 0 --width 1280 --height 720 --framerate 30 \
    --codec yuv420 -o /dev/video7 &

# consume it like a webcam
./build/camstream -d /dev/video7
```

**Option B: legacy camera stack** (older Bullseye images): enable with `raspi-config`, Advanced, Legacy Camera, reboot, then use `/dev/video0`.

**Option C: USB is unavailable and loops are unwanted:** run camstream with the test pattern (`--test`) to validate the network path first, then pick option A.

## CPU load reference, Pi 4

| Setup | Approximate ARM load while streaming 720p30 |
|---|---|
| hardware encoder `/dev/video11` | 10 to 20 percent of one core for the whole pipeline |
| libx264 `sw`, 640x480@30 | 40 to 70 percent of one core |
| libx264 `sw`, 1280x720@30 | saturates one core, use the hardware encoder |

Use `htop` and `/status` (captured versus encoded frame counters) to check for pipeline starvation.

## Tuning per model

| Model | Suggested invocation |
|---|---|
| Pi 5 | `-W 1280 -H 720 -b 4000 -F 30` (hardware encoder) |
| Pi 4 | `-W 1280 -H 720 -b 3000 -F 30` (hardware encoder) |
| Pi 3 | `-W 640 -H 480 -b 1500 -F 25` (hardware encoder) |
| Pi Zero 2 W | `-W 640 -H 480 -b 1000 -F 20` (hardware encoder only) |

## Run as a systemd service

`/etc/systemd/system/camstream.service`:

```ini
[Unit]
Description=camstream WebRTC camera server
After=network-online.target

[Service]
ExecStart=/usr/local/bin/camstream --device /dev/video0 --http-port 8080
Restart=always
RestartSec=3
User=pi
SupplementaryGroups=video
Nice=-5

[Install]
WantedBy=multi-user.target
```

```bash
sudo cp build/camstream /usr/local/bin/
sudo systemctl daemon-reload
sudo systemctl enable --now camstream
journalctl -u camstream -f
```

## Common Pi specific issues

| Symptom | Cause | Fix |
|---|---|---|
| `cannot open /dev/video0: Permission denied` | user not in video group | `sudo usermod -aG video $USER`, relogin |
| no M2M encoder found | camera stack disabled or kernel without bcm2835-codec | `sudo apt install rpicam-apps`, reboot, check `v4l2-ctl --list-devices` |
| video stutters on Pi 3 | USB bandwidth plus software encode | use `-e hw`, lower resolution and fps |
| session drops after 15 s idle log line | keepalive packets not reaching the Pi | check firewall for the UDP range, see troubleshooting doc |
| encoder probe opens the CSI camera node | pointed at a capture node that is not M2M | use `-e hw:/dev/video11` with the exact encoder node |

---

| | | |
|---|---|---|
| **Previous**<br>[13. Two laptops, one cable](13_lan_two_laptops.md) | **Index**<br>[docs](README.md) | **Next**<br>[15. Optimization notes](15_optimization_notes.md) |
