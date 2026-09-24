#
# camstream
#
# Builds build/camstream (native WebRTC stack) and, on request,
# build/camstream-libpeer (same camera pipeline, libpeer as the WebRTC
# stack), plus the standalone tests.
#
# Targets
#   make                   build/camstream
#   make test              build and run the unit tests
#   make install           install camstream, the sample config and the unit file
#   make camstream-libpeer build/camstream-libpeer (needs the libpeer submodule
#                          and cmake; no OpenSSL or libsrtp2 required)
#   make test-libpeer      build and run the libpeer tests
#   make install-libpeer   install camstream-libpeer and its unit file
#   make clean             remove build/
#   make help              this list
#
# Dependency locations
#   DEPS_PREFIX=/opt/cam   prefix holding OpenSSL, libsrtp2 and libx264
#   X264_DIR=/opt/x264     prefix for libx264 only
#   HAVE_X264=0            build without libx264 even when it is installed
#
# Every dependency is located by searching $(DEPS_PREFIX), then /usr/local,
# then /usr. pkg-config is not used: the only thing needed is a header and a
# library file, and Pi images occasionally ship without pkg-config.

CC       ?= gcc
HOST_CC  ?= cc
CSTD      = -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
WARN      = -Wall -Wextra -Wpedantic -Wshadow -Wundef -Wformat=2 \
            -Wstrict-prototypes -Wpointer-arith -Wvla
OPT      ?= -O2 -g
CFLAGS   ?=
LDFLAGS  ?=

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
UNITDIR  ?= /lib/systemd/system
BUILD    ?= build
DESTDIR  ?=

DEPS_PREFIX ?=
X264_DIR    ?=

# Dependency search paths ----------------------------------------------------
#
# lib64 before lib: an OpenSSL built from source installs into lib64 on
# x86_64, a distribution package into lib/<multiarch>.

ifneq ($(DEPS_PREFIX),)
  PREFIX_INCLUDE := $(DEPS_PREFIX)/include
  PREFIX_LIBDIRS := $(wildcard $(DEPS_PREFIX)/lib64) \
                    $(wildcard $(DEPS_PREFIX)/lib)
else
  PREFIX_INCLUDE :=
  PREFIX_LIBDIRS :=
endif

ifneq ($(X264_DIR),)
  X264_INCLUDE := $(X264_DIR)/include
  X264_LIBDIRS := $(wildcard $(X264_DIR)/lib64) \
                  $(wildcard $(X264_DIR)/lib)
else
  X264_INCLUDE :=
  X264_LIBDIRS :=
endif

MULTIARCH  := $(shell $(CC) -print-multiarch 2>/dev/null)
SYSTEM_INCLUDE := /usr/local/include /usr/include /usr/local/include/node
SYSTEM_LIBDIRS := /usr/local/lib /usr/lib/$(MULTIARCH) /usr/lib64 /usr/lib

# find_header(DIRS, RELATIVE_PATH) -> first directory containing the header
find_header = $(firstword $(foreach d,$(1),$(if $(wildcard $(d)/$(2)),$(d))))

OPENSSL_INCLUDE := $(call find_header,$(PREFIX_INCLUDE) $(SYSTEM_INCLUDE),openssl/ssl.h)
SRTP_INCLUDE    := $(call find_header,$(PREFIX_INCLUDE) $(SYSTEM_INCLUDE),srtp2/srtp.h)
X264_HEADER     := $(call find_header,$(X264_INCLUDE) $(PREFIX_INCLUDE) $(SYSTEM_INCLUDE),x264.h)

OPENSSL_LIBDIR := $(firstword $(foreach d,$(PREFIX_LIBDIRS) $(SYSTEM_LIBDIRS),\
                    $(if $(wildcard $(d)/libssl.so* $(d)/libssl.a),$(d))))
SRTP_LIBDIR    := $(firstword $(foreach d,$(PREFIX_LIBDIRS) $(SYSTEM_LIBDIRS),\
                    $(if $(wildcard $(d)/libsrtp2.so* $(d)/libsrtp2.a),$(d))))
X264_LIBDIR    := $(firstword $(foreach d,$(X264_LIBDIRS) $(PREFIX_LIBDIRS) $(SYSTEM_LIBDIRS),\
                    $(if $(wildcard $(d)/libx264.so* $(d)/libx264.a),$(d))))

ifeq ($(X264_HEADER),)
  ifeq ($(HAVE_X264),1)
    $(error HAVE_X264=1 was requested but x264.h was not found. Install \
libx264-dev or pass X264_DIR=/path)
  endif
  HAVE_X264 := 0
else
  HAVE_X264 ?= 1
  ifeq ($(X264_LIBDIR),)
    $(error x264.h was found in $(X264_HEADER) but no libx264 library was. \
Install libx264-dev or pass X264_DIR=/path)
  endif
endif

# Missing dependencies are only fatal for goals that compile or install
# something. `make clean` and `make help` must work on a machine that has
# no OpenSSL at all, so the checks are skipped for those two. Switching to
# a build goal then reports the same messages as before.
#
# camstream-libpeer brings its own DTLS and SRTP (mbedtls and libsrtp from
# the libpeer submodule), so its goals do not need OpenSSL or libsrtp2.
SAFE_GOALS := clean help
LIBPEER_GOALS := camstream-libpeer libpeer test-libpeer install-libpeer
BUILD_GOALS := $(filter-out $(SAFE_GOALS) $(LIBPEER_GOALS),\
                 $(or $(MAKECMDGOALS),all))

ifneq ($(BUILD_GOALS),)
ifeq ($(OPENSSL_INCLUDE),)
  $(error OpenSSL headers not found. Install libssl-dev, or build OpenSSL and \
pass DEPS_PREFIX=/path. See the build section of README.md)
endif
ifeq ($(SRTP_INCLUDE),)
  $(error libsrtp2 headers not found. Install libsrtp2-dev, or build libsrtp \
with --prefix=/path --enable-openssl and pass DEPS_PREFIX=/path. See the build \
section of README.md)
endif
ifeq ($(SRTP_LIBDIR),)
  $(error libsrtp2 library not found although srtp2/srtp.h is in \
$(SRTP_INCLUDE))
endif
ifeq ($(OPENSSL_LIBDIR),)
  $(error libssl not found although openssl/ssl.h is in $(OPENSSL_INCLUDE))
endif
endif

# A prefix that only holds static archives needs no run time path; a shared
# library outside the default search path does.
DEP_LIBDIRS_ALL := $(OPENSSL_LIBDIR) $(SRTP_LIBDIR) $(X264_LIBDIR)
RPATH_DIRS := $(filter-out $(SYSTEM_LIBDIRS),$(DEP_LIBDIRS_ALL))

DEP_CFLAGS := $(addprefix -I,$(filter-out /usr/include,$(sort \
                $(OPENSSL_INCLUDE) $(SRTP_INCLUDE) $(X264_HEADER))))
# A comma inside addprefix would be read as its argument separator.
comma := ,
DEP_LDFLAGS := $(addprefix -L,$(sort $(OPENSSL_LIBDIR) $(SRTP_LIBDIR))) \
               $(addprefix -Wl$(comma)-rpath$(comma),$(sort $(RPATH_DIRS)))

# libsrtp2 uses libcrypto, so it must precede it (matters for static archives).
DEP_LIBS := -lsrtp2 -lssl -lcrypto -lpthread
ifeq ($(HAVE_X264),1)
  DEP_CFLAGS += $(addprefix -I,$(filter-out /usr/include,$(X264_INCLUDE)))
  DEP_LDFLAGS += -L$(X264_LIBDIR)
  DEP_LIBS += -lx264
endif
# libm last: a static libx264 has its math symbols resolved from here.
DEP_LIBS += -lm

# Sources -------------------------------------------------------------------

APP_INCLUDE_DIRS := include include/app include/media include/net include/webrtc \
                    $(BUILD)/generated
APP_INCLUDES     := $(addprefix -I,$(APP_INCLUDE_DIRS)) \
                    -isystem third_party/mongoose $(DEP_CFLAGS)

# src/lpstream belongs to camstream-libpeer only (see below).
SOURCES := $(filter-out src/lpstream/%,$(wildcard src/*.c src/*/*.c)) \
           third_party/mongoose/mongoose.c
OBJECTS := $(patsubst %.c,$(BUILD)/obj/%.o,$(SOURCES))

COMMON_CFLAGS := $(CSTD) $(WARN) $(OPT) $(CFLAGS) $(APP_INCLUDES) \
                 -DHAVE_X264=$(HAVE_X264)

# Objects are only reusable while the flags that produced them stay the
# same. This stamp is rewritten when they change, which makes every object
# stale, so `make HAVE_X264=0` or `make OPT=-O3` after a normal build
# recompiles instead of relinking a mix of the two.
FLAG_STAMP := $(BUILD)/.flags
$(shell mkdir -p $(BUILD); \
        printf '%s\n' '$(COMMON_CFLAGS)' > $(FLAG_STAMP).tmp; \
        if ! cmp -s $(FLAG_STAMP).tmp $(FLAG_STAMP) 2>/dev/null; then \
            mv -f $(FLAG_STAMP).tmp $(FLAG_STAMP); \
        else \
            rm -f $(FLAG_STAMP).tmp; \
        fi)

BINARY    := $(BUILD)/camstream
WEB_PAGE  := web/index.html
WEB_ASSETS := $(BUILD)/generated/web_assets.h
EMBED_TOOL := $(BUILD)/embed_assets

# Rules ---------------------------------------------------------------------

.PHONY: all test install clean help camstream-libpeer libpeer test-libpeer \
        install-libpeer

all: $(BINARY)

# $(OPT) is repeated on the link line: a sanitizer or LTO build needs its
# runtime flags while linking, and a plain -O2 link costs nothing extra.
$(BINARY): $(OBJECTS) $(WEB_ASSETS)
	@mkdir -p $(dir $@)
	$(CC) $(OPT) $(OBJECTS) -o $@ $(LDFLAGS) $(DEP_LDFLAGS) $(DEP_LIBS)
	@echo "built $(BINARY) (libx264: $(if $(filter 1,$(HAVE_X264)),yes,no))"

$(BUILD)/obj/%.o: %.c $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -c $< -o $@

# Only the server serves the page, so only its object depends on it.
$(BUILD)/obj/src/app/app_server.o: $(WEB_ASSETS)

# Vendored third party code: built without the project warning set because it is
# not ours to fix, with the alloca declaration its allocator relies on, and with
# its own logging compiled out so mongoose never writes to stderr behind the
# logger (the spec requires one log stream and a quiet hot path).
$(BUILD)/obj/third_party/mongoose/mongoose.o: third_party/mongoose/mongoose.c \
        $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(OPT) $(CFLAGS) -Ithird_party/mongoose -include alloca.h \
	      -DMG_ENABLE_LOG=0 -c $< -o $@

# The web page is embedded in the binary, so a deployment needs no web root.
# Regenerating it only when web/index.html changes keeps rebuilds cheap.
$(EMBED_TOOL): tools/embed_assets.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CSTD) -O2 -o $@ $<

$(WEB_ASSETS): $(WEB_PAGE) $(EMBED_TOOL)
	@mkdir -p $(dir $@)
	$(EMBED_TOOL) $@ CAMSTREAM_WEB_ASSETS_H web_index_html $(WEB_PAGE)

# camstream-libpeer -----------------------------------------------------------
#
# libpeer (github.com/sepfy/libpeer, pinned as the git submodule
# third_party/libpeer) is built by its own CMake project, which also builds
# its bundled mbedtls, libsrtp, usrsctp and cJSON as static libraries into
# $(LIBPEER_BUILD)/dist. Only the `peer` target is built, not the examples.
#
# libpeer compile-time settings (src/config.h defaults otherwise):
#   CONFIG_DTLS_USE_ECDSA=1         P-256 certificate: generated in
#                                   milliseconds per viewer on a Pi, where
#                                   the default RSA key takes much longer
#   CONFIG_STUN_KEEPALIVE_INTERVAL  consent checks every 2 s; a viewer that
#   CONFIG_STUN_KEEPALIVE_TIMEOUT   stops answering for 10 s is closed (the
#                                   default 0 never notices a closed tab)
#   LOG_LEVEL=LEVEL_WARN            libpeer prints straight to stdout, with
#                                   source paths; keep only its warnings
#
# Its mbedtls version generates sources at build time and needs cmake and
# the Python modules jsonschema and jinja2 (Pi OS: sudo apt install cmake
# python3-jsonschema python3-jinja2). LIBPEER_CMAKE_ARGS passes extra CMake
# options, for example -DCMAKE_TOOLCHAIN_FILE=... when cross compiling.
# CMAKE_POLICY_VERSION_MINIMUM lets CMake 4 configure the bundled projects
# that still declare CMake 2.x compatibility; CMake 3 ignores it.

CMAKE ?= cmake
LIBPEER_SRC   := third_party/libpeer
LIBPEER_BUILD := $(BUILD)/libpeer
LIBPEER_DIST  := $(LIBPEER_BUILD)/dist/lib
LIBPEER_LIB   := $(LIBPEER_BUILD)/src/libpeer.a
LIBPEER_DEFINES := -DCONFIG_DTLS_USE_ECDSA=1 \
                   -DCONFIG_STUN_KEEPALIVE_INTERVAL=2000 \
                   -DCONFIG_STUN_KEEPALIVE_TIMEOUT=10000 \
                   -DLOG_LEVEL=LEVEL_WARN
LIBPEER_CMAKE_ARGS ?=
LIBPEER_STAMP := $(LIBPEER_BUILD)/.configured
# Link order: users before the libraries they use.
LIBPEER_LIBS := $(LIBPEER_LIB) $(LIBPEER_DIST)/libsrtp2.a \
                $(LIBPEER_DIST)/libusrsctp.a $(LIBPEER_DIST)/libmbedtls.a \
                $(LIBPEER_DIST)/libmbedx509.a $(LIBPEER_DIST)/libmbedcrypto.a \
                $(LIBPEER_DIST)/libtfpsacrypto.a $(LIBPEER_DIST)/libcjson.a

LP_BINARY := $(BUILD)/camstream-libpeer
LP_WEB_PAGE := web/libpeer.html
LP_WEB_ASSETS := $(BUILD)/generated/lp_web_assets.h
# The camera pipeline, config, log, mDNS and HTTP code are shared with
# camstream; the native WebRTC stack (src/webrtc) and its server are not.
LP_SHARED_SOURCES := src/app/app_config.c src/app/log.c src/net/mdns.c \
                     $(wildcard src/media/*.c) third_party/mongoose/mongoose.c
LP_SOURCES := $(wildcard src/lpstream/*.c)
LP_OBJECTS := $(patsubst %.c,$(BUILD)/obj/%.o,$(LP_SHARED_SOURCES) $(LP_SOURCES))
LP_CFLAGS := $(COMMON_CFLAGS) -Iinclude/lpstream -isystem $(LIBPEER_SRC)/include
LP_LDFLAGS := $(if $(filter 1,$(HAVE_X264)),-L$(X264_LIBDIR) \
                $(if $(filter-out $(SYSTEM_LIBDIRS),$(X264_LIBDIR)),\
                  -Wl$(comma)-rpath$(comma)$(X264_LIBDIR)))
LP_LIBS := $(LIBPEER_LIBS) $(if $(filter 1,$(HAVE_X264)),-lx264) -lpthread -lm

camstream-libpeer: $(LP_BINARY)

$(LP_BINARY): $(LP_OBJECTS) $(LIBPEER_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(OPT) $(LP_OBJECTS) -o $@ $(LDFLAGS) $(LP_LDFLAGS) $(LP_LIBS)
	@echo "built $(LP_BINARY) (libpeer $$(git -C $(LIBPEER_SRC) rev-parse --short HEAD 2>/dev/null), libx264: $(if $(filter 1,$(HAVE_X264)),yes,no))"

$(BUILD)/obj/src/lpstream/%.o: src/lpstream/%.c $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(LP_CFLAGS) -c $< -o $@

$(BUILD)/obj/src/lpstream/lp_server.o: $(LP_WEB_ASSETS)

$(LP_WEB_ASSETS): $(LP_WEB_PAGE) $(EMBED_TOOL)
	@mkdir -p $(dir $@)
	$(EMBED_TOOL) $@ CAMSTREAM_LP_WEB_ASSETS_H web_libpeer_html $(LP_WEB_PAGE)

# Configure once per setting; `cmake --build` itself is incremental and runs
# every time so an updated submodule is rebuilt.
$(LIBPEER_STAMP): Makefile
	@test -f $(LIBPEER_SRC)/CMakeLists.txt -a \
	        -f $(LIBPEER_SRC)/third_party/mbedtls/CMakeLists.txt || { \
	    echo "libpeer submodule missing: run" \
	         "'git submodule update --init --recursive'"; exit 1; }
	@command -v $(CMAKE) >/dev/null || { \
	    echo "cmake not found (Pi OS: sudo apt install cmake" \
	         "python3-jsonschema python3-jinja2)"; exit 1; }
	CC="$(CC)" CMAKE_POLICY_VERSION_MINIMUM=3.5 $(CMAKE) -S $(LIBPEER_SRC) \
	    -B $(LIBPEER_BUILD) -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
	    -DCMAKE_C_FLAGS="$(LIBPEER_DEFINES)" $(LIBPEER_CMAKE_ARGS)
	@touch $@

# '+' hands make's jobserver to the generated Makefiles, so `make -j4` shares
# its four slots; a serial `make` still builds libpeer on every core.
libpeer: $(LIBPEER_STAMP)
	+CC="$(CC)" CMAKE_POLICY_VERSION_MINIMUM=3.5 $(CMAKE) --build $(LIBPEER_BUILD) \
	    --target peer $(if $(findstring -j,$(MAKEFLAGS)),,-j $$(nproc 2>/dev/null || echo 2)) \
	    -- --no-print-directory

$(LIBPEER_LIB): libpeer
	@test -f $@

# Tests ---------------------------------------------------------------------
#
# Each test links the real module under test. Tests never link the network
# server: app_server.c is exercised by running the binary (see README.md).

TEST_BUILD := $(BUILD)/tests

$(TEST_BUILD)/test_stun: tests/test_stun.c src/webrtc/ice_lite.c include/webrtc/ice_lite.h \
        $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) $(CFLAGS) -Iinclude -Iinclude/webrtc \
	      $(DEP_CFLAGS) tests/test_stun.c src/webrtc/ice_lite.c -o $@ \
	      $(DEP_LDFLAGS) -lcrypto

$(TEST_BUILD)/test_encoder_worker: tests/test_encoder_worker.c \
        src/media/encoder_worker.c src/media/frame_hub.c src/media/frame_pool.c \
        src/media/au_ring.c src/media/yuv_convert.c src/media/h264_bitstream.c \
        src/app/log.c $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) $(CFLAGS) -Iinclude -Iinclude/media \
	      -Iinclude/app tests/test_encoder_worker.c src/media/encoder_worker.c \
	      src/media/frame_hub.c src/media/frame_pool.c src/media/au_ring.c \
	      src/media/yuv_convert.c src/media/h264_bitstream.c src/app/log.c \
	      -o $@ -lpthread

$(TEST_BUILD)/test_csi_source: tests/test_csi_source.c src/media/csi_source.c \
        src/media/test_source.c src/app/log.c $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) $(CFLAGS) -Iinclude -Iinclude/media \
	      -Iinclude/app tests/test_csi_source.c src/media/csi_source.c \
	      src/media/test_source.c src/app/log.c -o $@ -lpthread

# The responder is protocol code with no dependency of its own, so the
# test links it directly with the log module it writes to.
$(TEST_BUILD)/test_mdns: tests/test_mdns.c src/net/mdns.c src/app/log.c \
        include/net/mdns.h $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) $(CFLAGS) -Iinclude -Iinclude/net \
	      -Iinclude/app tests/test_mdns.c src/net/mdns.c src/app/log.c -o $@

$(TEST_BUILD)/test_sdp_rtcp: tests/test_sdp_rtcp.c src/webrtc/sdp.c src/webrtc/rtcp.c \
        src/app/log.c include/webrtc/sdp.h include/webrtc/rtcp.h $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) $(CFLAGS) -Iinclude -Iinclude/app \
	      -Iinclude/webrtc tests/test_sdp_rtcp.c src/webrtc/sdp.c \
	      src/webrtc/rtcp.c src/app/log.c -o $@ -lpthread

# The session test needs the whole WebRTC stack: STUN, DTLS, SRTP,
# RTP packetization, RTCP parsing and SDP answer generation.
WEBRTC_TEST_SOURCES := src/webrtc/webrtc_session.c src/webrtc/dtls_srtp.c \
                       src/webrtc/ice_lite.c src/webrtc/rtp_h264.c \
                       src/webrtc/rtcp.c src/webrtc/sdp.c src/app/log.c

$(TEST_BUILD)/test_rtc_session: tests/test_rtc_session.c $(WEBRTC_TEST_SOURCES) \
        $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) $(CFLAGS) -Iinclude -Iinclude/app \
	      -Iinclude/media -Iinclude/webrtc $(DEP_CFLAGS) \
	      tests/test_rtc_session.c $(WEBRTC_TEST_SOURCES) -o $@ \
	      $(DEP_LDFLAGS) -lsrtp2 -lssl -lcrypto -lpthread -lm

# Every test depends on the flag stamp too: a change of OPT or HAVE_X264
# must rebuild the tests, otherwise a sanitizer run silently executes a
# binary instrumented with something else.
#
# This one drives the real binary over HTTP, so it takes the server path
# as its only argument and needs the binary built first.
$(TEST_BUILD)/test_server_api: tests/test_server_api.c $(BINARY) $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) $(CFLAGS) tests/test_server_api.c -o $@

# This one acts as the browser: real UDP, STUN, DTLS and SRTP against the
# running server, so it links OpenSSL and libsrtp2 but no project source.
$(TEST_BUILD)/test_lan_stream: tests/test_lan_stream.c $(BINARY) $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) $(CFLAGS) $(DEP_CFLAGS) \
	      tests/test_lan_stream.c -o $@ $(DEP_LDFLAGS) -lsrtp2 -lssl -lcrypto -lpthread

# Pure parsing code of camstream-libpeer: no libpeer needed to test it.
$(TEST_BUILD)/test_lp_sdp: tests/test_lp_sdp.c src/lpstream/lp_sdp.c \
        include/lpstream/lp_sdp.h $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) $(CFLAGS) -Iinclude/lpstream \
	      tests/test_lp_sdp.c src/lpstream/lp_sdp.c -o $@

TEST_BINARIES := $(TEST_BUILD)/test_stun $(TEST_BUILD)/test_encoder_worker \
                 $(TEST_BUILD)/test_csi_source $(TEST_BUILD)/test_mdns \
                 $(TEST_BUILD)/test_rtc_session $(TEST_BUILD)/test_sdp_rtcp \
                 $(TEST_BUILD)/test_lp_sdp \
                 $(TEST_BUILD)/test_server_api $(TEST_BUILD)/test_lan_stream

# argv[1] is the server binary for the black box tests; argv[2] is the
# configuration file that `make install` ships, which the API test loads to
# prove a fresh install can start. Tests that need neither ignore them.
test: $(TEST_BINARIES) $(BINARY)
	@for test_binary in $(TEST_BINARIES); do \
	    echo "== $$test_binary"; \
	    $$test_binary $(BINARY) config/camstream.conf || exit 1; \
	done
	@echo "all tests passed"

# End to end: libpeer as the browser against a running camstream-libpeer
# (HTTP signaling, ICE, DTLS, SRTP, H.264 depacketization).
#
# The receiving side is libpeer compiled a second time with a larger
# CONFIG_MTU (tests/libpeer_rx_config.h). Stock libpeer receives into a CONFIG_MTU (1300) byte buffer
# while its sender emits CONFIG_MTU byte RTP packets plus a 10 byte SRTP
# tag, so a libpeer receiver truncates every full-size packet (and then
# depacketizes the undecryptable bytes anyway). Browsers are unaffected;
# only this test client needs the room. The sender under test, in
# camstream-libpeer, is the unmodified build above.
LIBPEER_RX_SOURCES := $(wildcard $(LIBPEER_SRC)/src/*.c)
LIBPEER_RX_OBJECTS := $(patsubst $(LIBPEER_SRC)/src/%.c,$(TEST_BUILD)/libpeer_rx/%.o,\
                        $(LIBPEER_RX_SOURCES))

$(TEST_BUILD)/libpeer_rx/%.o: $(LIBPEER_SRC)/src/%.c tests/libpeer_rx_config.h \
        $(LIBPEER_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(OPT) -w -include tests/libpeer_rx_config.h -DDISABLE_PEER_SIGNALING \
	      -DCONFIG_DTLS_USE_ECDSA=1 -DLOG_LEVEL=LEVEL_WARN \
	      -I$(LIBPEER_SRC)/src -I$(LIBPEER_BUILD)/dist/include \
	      -I$(LIBPEER_BUILD)/dist/include/cjson -c $< -o $@

$(TEST_BUILD)/test_libpeer_stream: tests/test_libpeer_stream.c \
        $(LIBPEER_RX_OBJECTS) $(FLAG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) $(CFLAGS) -isystem $(LIBPEER_SRC)/include \
	      -DDISABLE_PEER_SIGNALING tests/test_libpeer_stream.c \
	      $(LIBPEER_RX_OBJECTS) -o $@ $(filter-out $(LIBPEER_LIB),$(LIBPEER_LIBS)) \
	      -lpthread -lm

test-libpeer: $(TEST_BUILD)/test_lp_sdp $(TEST_BUILD)/test_libpeer_stream \
        $(LP_BINARY)
	@echo "== $(TEST_BUILD)/test_lp_sdp"; $(TEST_BUILD)/test_lp_sdp
	@echo "== $(TEST_BUILD)/test_libpeer_stream"; \
	    $(TEST_BUILD)/test_libpeer_stream $(LP_BINARY)
	@echo "libpeer tests passed"

# Install -------------------------------------------------------------------

install: $(BINARY)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BINARY) $(DESTDIR)$(BINDIR)/camstream
	install -d $(DESTDIR)$(SYSCONFDIR)
	@# Never overwrite an operator's configuration: new defaults go next to it.
	@if [ -e $(DESTDIR)$(SYSCONFDIR)/camstream.conf ]; then \
	    install -m 0644 config/camstream.conf $(DESTDIR)$(SYSCONFDIR)/camstream.conf.new; \
	    echo "kept $(DESTDIR)$(SYSCONFDIR)/camstream.conf; shipped defaults are in camstream.conf.new"; \
	else \
	    install -m 0644 config/camstream.conf $(DESTDIR)$(SYSCONFDIR)/camstream.conf; \
	fi
	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 packaging/camstream.service \
	        $(DESTDIR)$(UNITDIR)/camstream.service
	@echo "installed binary matches the build if these two checksums are equal:"
	@sha256sum $(BINARY) $(DESTDIR)$(BINDIR)/camstream

install-libpeer: $(LP_BINARY)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(LP_BINARY) $(DESTDIR)$(BINDIR)/camstream-libpeer
	install -d $(DESTDIR)$(SYSCONFDIR)
	@if [ ! -e $(DESTDIR)$(SYSCONFDIR)/camstream.conf ]; then \
	    install -m 0644 config/camstream.conf $(DESTDIR)$(SYSCONFDIR)/camstream.conf; \
	fi
	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 packaging/camstream-libpeer.service \
	        $(DESTDIR)$(UNITDIR)/camstream-libpeer.service
	@sha256sum $(LP_BINARY) $(DESTDIR)$(BINDIR)/camstream-libpeer

clean:
	rm -rf $(BUILD)

help:
	@echo "targets:"
	@echo "  make             build build/camstream"
	@echo "  make test        build and run the unit tests"
	@echo "  make install     install camstream and its support files"
	@echo "  make camstream-libpeer  build build/camstream-libpeer (libpeer WebRTC)"
	@echo "  make test-libpeer  build and run the libpeer tests"
	@echo "  make install-libpeer  install camstream-libpeer and its unit file"
	@echo "  make clean       remove build/"
	@echo ""
	@echo "variables:"
	@echo "  DEPS_PREFIX=/path  OpenSSL, libsrtp2 and libx264 prefix"
	@echo "  X264_DIR=/path     libx264 prefix only"
	@echo "  HAVE_X264=0        build without libx264"
	@echo "  PREFIX=/usr/local  install prefix"
	@echo "  DESTDIR=/tmp/root  staging directory for packaging"
	@echo "  OPT='-O0 -g'       override optimisation flags"
	@echo "  CMAKE=cmake        CMake used for libpeer"
	@echo "  LIBPEER_CMAKE_ARGS extra CMake options for libpeer (toolchain file)"
	@echo ""
# Detection is printed whether or not a dependency was found, because the
# usual reason to run `make help` on a fresh machine is a missing one.
	@echo "detected (pass DEPS_PREFIX=/path to point elsewhere):"
	@echo "  OpenSSL   headers: $(if $(OPENSSL_INCLUDE),$(OPENSSL_INCLUDE),NOT FOUND)  libraries: $(if $(OPENSSL_LIBDIR),$(OPENSSL_LIBDIR),NOT FOUND)"
	@echo "  libsrtp2  headers: $(if $(SRTP_INCLUDE),$(SRTP_INCLUDE),NOT FOUND)  libraries: $(if $(SRTP_LIBDIR),$(SRTP_LIBDIR),NOT FOUND)"
	@echo "  libx264   $(if $(filter 1,$(HAVE_X264)),headers: $(X264_HEADER)  libraries: $(X264_LIBDIR),not used (HAVE_X264=0 or no header found))"
