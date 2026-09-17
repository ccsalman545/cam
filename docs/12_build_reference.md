# Build reference

[camstream docs](README.md) / 12. Build reference &nbsp;·&nbsp; [README](../README.md)

## Toolchain

- C11 compiler (gcc 8+ or clang 10+)
- GNU make
- POSIX threads

## Libraries

| Library | Debian/Ubuntu/RPi OS | Fedora | Used for |
|---|---|---|---|
| OpenSSL 1.1.1 or 3.x | `libssl-dev` | `openssl-devel` | DTLS 1.2, certificates, HMAC |
| libsrtp2 2.x | `libsrtp2-dev` | `libsrtp-devel` | SRTP protect and unprotect |
| libx264 (optional) | `libx264-dev` | `x264-devel` | software encoder fallback |

Mongoose 7.23 is vendored in `third_party/`, nothing to install.

Install everything on Debian family:

```bash
sudo apt install build-essential libssl-dev libsrtp2-dev libx264-dev
```

## Targets

```bash
make                      # build/camstream, the WebRTC server (default)
make camstream-janus      # build/camstream-janus, RTP to an external Janus gateway
make camstream-libpeer    # build/camstream-libpeer, the libpeer backend
make libpeer              # clone and build upstream libpeer under build/ (needs network)
make libpeer-backend      # alias for camstream-libpeer
make test                 # run test_stun, test_vision, test_encoder_worker
make test-janus           # run the Janus RTP sender test against a fake gateway
make vision-capture       # build the PGM/OBJ mosaic smoke tool
make vision-test          # build and run the vision unit test alone
make clean                # remove build/
make help                 # quick reference
```

| Target | Needs a camera | Needs network | External libraries |
|---|---|---|---|
| `camstream` | no | no | OpenSSL, libsrtp2, optional x264 |
| `camstream-janus` | no | no | optional x264 only |
| `camstream-libpeer` | no | yes, to fetch libpeer once | cmake, git, optional x264 |
| `test`, `vision-test` | no | no | `libcrypto` for `test_stun` |
| `test-janus` | no | no | none beyond libc and pthread |

## Make variables

| Variable | Default | Meaning |
|---|---|---|
| `OPENSSL_DIR` | system | prefix with OpenSSL headers and libs |
| `SRTP_DIR` | system | prefix with `include/srtp2` and lib |
| `X264_DIR` | system | prefix with `x264.h` and lib |
| `HAVE_X264` | autodetected | force `0` to build without the software encoder |
| `CC`, `CFLAGS` | gcc, -O2 | usual overrides |

Example with custom prefixes:

```bash
make OPENSSL_DIR=/opt/ssl SRTP_DIR=/opt/srtp X264_DIR=/opt/x264
```

x264 autodetection checks `$(X264_DIR)/include`, `/usr/include` and `/usr/local/include`. Without x264 the binary still builds and runs wherever a hardware V4L2 M2M encoder exists (for example Raspberry Pi); otherwise startup fails with instructions.

## Static or offline builds (optional)

The same variables work against static libraries:

```bash
# build static deps into /tmp/deps, then
make OPENSSL_DIR=/tmp/deps SRTP_DIR=/tmp/deps X264_DIR=/tmp/deps HAVE_X264=1
```

libsrtp2 can be compiled directly from source without autotools: compile `srtp/srtp.c` plus `crypto/{cipher,hash,kernel,math,replay}/*.c` (minus test files) with `-DHAVE_CONFIG_H -DOPENSSL`, a minimal `config.h` and `crypto/include` on the include path, then archive with `ar rcs`.

## Compile time switches

All of them come from the Makefile, so the sources stay plain C11.

| Macro | Set by | Effect |
|---|---|---|
| `HAVE_X264=0/1` | every target | Enables the libx264 software encoder. Autodetected, or forced with `HAVE_X264=` |
| `USE_JANUS_TRANSPORT=1` | `camstream-janus` | Swaps the WebRTC transport for the Janus RTP sender and the Janus dashboard |
| `USE_LIBPEER=1` | `camstream-libpeer` | Swaps the native stack for the libpeer runtime |
| `_DEFAULT_SOURCE`, `_POSIX_C_SOURCE=200809L` | every target | POSIX feature macros |

## Cross compiling

Set `CC` to the cross prefix and point the three `*_DIR` variables at the sysroot:

```bash
make CC=aarch64-linux-gnu-gcc \
     OPENSSL_DIR=$SYSROOT/usr SRTP_DIR=$SYSROOT/usr X264_DIR=$SYSROOT/usr
```

## Platform support

| Platform | Status |
|---|---|
| x86_64 and arm64 Linux (any distro) | fully supported |
| Raspberry Pi OS Bullseye and Bookworm | fully supported, hardware encoder auto-detected |
| 32 bit ARM | supported (compile time checks pass), hardware encoder recommended |
| macOS and Windows | not supported (V4L2 is Linux only) |

---

| | | |
|---|---|---|
| **Previous**<br>[11. WebRTC internals](11_webrtc_internals.md) | **Index**<br>[docs](README.md) | **Next**<br>[13. Two laptops, one cable](13_lan_two_laptops.md) |
