# Troubleshooting

Start at the top of the decision tree and follow the first failing step.

```mermaid
flowchart TB
    A["symptom: no video in browser"] --> B{"does the page open?"}
    B -- no --> C{"does ping <server-ip> work?"}
    C -- no --> C1["fix IPs, cable, interfaces\nsee LAN guide steps 1 to 3"]
    C -- yes --> C2["open firewall TCP 8080\nLAN guide step 4"]
    B -- yes --> D{"timeline: SDP step green?"}
    D -- no --> D1["check server log for\n'offer is missing' errors\nuse a recent browser"]
    D -- yes --> E{"ICE step green?"}
    E -- no --> E1["firewall UDP 50000 to 50008\nno proxy between browser and server"]
    E -- no2["red"] --> E1
    E -- yes --> F{"DTLS step green?"}
    F -- no --> F1["very old browser or MTU path\nverify UDP is not filtered after the\nfirewall (router, VPN, proxy)"]
    F -- yes --> G{"stats show frames decoded?"}
    G -- no --> G1["encoder log lines at startup\ntry -e sw or --test"]
    G -- yes --> H["all good: use stats panel\nfor quality tuning"]
```

## Build problems

| Symptom | Cause | Fix |
|---|---|---|
| `openssl/ssl.h: No such file` | missing OpenSSL dev package | `sudo apt install libssl-dev` |
| `srtp2/srtp.h: No such file` | missing libsrtp2 dev | `sudo apt install libsrtp2-dev` |
| `x264.h: No such file` | missing x264 dev | install it, or accept hardware-only mode |
| `no usable encoder` at startup | neither hardware nor software available | install x264, or run on hardware with an M2M encoder |
| `undefined reference to srtp_protect` | mixed libsrtp 3.x preview with 2.x sources | use the distro `libsrtp2-dev` (2.x) |

## Camera problems

| Symptom | Cause | Fix |
|---|---|---|
| `cannot open /dev/video0: Permission denied` | user not in video group | `sudo usermod -aG video $USER` and relogin |
| `Device does not support video capture` | wrong node (output or metadata node) | list with `v4l2-ctl --list-devices`, try `/dev/video1` and so on |
| `supports neither YUYV nor YU12` | MJPEG-only camera | prefer a UVC cam with YUYV, or bridge via ffmpeg into v4l2loopback |
| capture timeouts in the log | USB bandwidth saturation | lower resolution or fps, use a USB 3 port |
| Pi CSI camera not found | libcamera does not expose V4L2 YUYV | see the v4l2loopback bridge in the Pi guide |

## Encoder problems

| Symptom | Cause | Fix |
|---|---|---|
| `encoded frames` stays 0, viewers table says none | idle by design: with no session the encode worker discards raw frames instead of burning CPU | nothing to do; the server panel shows `encoder idle`. Counters move as soon as a viewer connects |
| captured grows, encoded stays 0, a viewer connected | the encode worker stalled while active | compare `captured_frames` with `encoder_frames_seen` in `/status`: equal means frames reach the worker and the `encoder_skipped_*` / `encoder_no_output` counters name the drop path; `encoder_frames_seen` stuck near 0 means frames never arrive (hub/pool problem) |
| `encode worker: frame WxH does not match the encoder WxH` in the log | source negotiated a different resolution than the encoder was opened with | restart; if it persists, force resolution with `-W`/`-H` matching what `v4l2-ctl --list-formats-ext` reports |
| `encode worker: stalled, no output for N frames while active` in the log | nothing was encoded for about three seconds while a session existed | the same line prints the drop breakdown (mismatch, bad-size, encoder-no-output); with all counters zero the encode thread starves, check capture health |

## Network problems

| Symptom | Cause | Fix |
|---|---|---|
| page opens, ICE stuck on checking | UDP blocked between the peers | open UDP 50000 to 50008 on the server firewall |
| works on localhost, not across the cable | advertised candidate is the wrong interface | check `interfaces` in `/status`, open the page via the IP you want used |
| ICE connects, video freezes then resumes | loss bursts on the link | shorten keyframes `-K 1`, lower `-b` |
| Page loads but media never arrives | TCP 8080 fine, UDP path broken | firewall, VPN, proxy: eliminate middleboxes, open the UDP range |
| session closes after exactly 15 s | idle timeout: no packets from viewer | look at `stun_rx` / `stun_ok` / `stun_bad_user` in `/status` and the idle-timeout log line: `stun_rx=0` means UDP never arrived; `stun_bad_user>0` means USERNAME did not match the session ice-ufrag |
| two viewers, second gets nothing | UDP ports exhausted | raise `-u` base or check the 8 session limit |

## WebRTC diagnostics

1. Server side: read the log lines. Each session logs creation, ICE peer address, state transitions (ice, dtls, streaming) and close reason.
2. `/status` shows per session `state`, `packets_sent`, `pli`, `nacks`, `retx`.
3. Browser side: open `chrome://webrtc-internals` while streaming: capture start, ICE candidate pairs, inbound-rtp graphs.
4. The dashboard timeline pinpoints the failing layer: HTTP, SDP, ICE, DTLS, SRTP or media.

## Hardware encoder notes

| Symptom | Cause | Fix |
|---|---|---|
| `no V4L2 M2M H.264 encoder detected` | driver not loaded, or none present | on Pi: `sudo apt install rpicam-apps`, reboot, verify with `v4l2-ctl --list-devices` |
| probe finds a device, open fails | node busy (another process) | `fuser -v /dev/video11`, stop the other user |
| hardware open ok but zero output forever | driver depth or format mismatch | run `-e sw` and report the dmesg lines |
| blocky video from hardware encoder | bitrate control unsupported | lower `-b`, or use `-e sw` |

## Getting a useful bug report

Include:

1. `./build/camstream --version`, OS and hardware
2. the full startup log including encoder selection lines
3. `curl -s http://server:8080/status`
4. what the dashboard timeline showed when it failed
5. `chrome://webrtc-internals` dump if the failure is media side
