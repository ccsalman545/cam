#
# camstream build
#
# Target : build/camstream  (WebRTC camera server)
#
# Dependency overrides (all optional when system packages are
# installed):
#
#   make OPENSSL_DIR=/path SRTP_DIR=/path X264_DIR=/path HAVE_X264=1
#
# System packages on Debian, Ubuntu and Raspberry Pi OS:
#   sudo apt install build-essential libssl-dev libsrtp2-dev libx264-dev
# Fedora:
#   sudo dnf install gcc make openssl-devel libsrtp-devel x264-devel
#

CC      ?= gcc
CFLAGS  ?= -O2
WARN     = -Wall -Wextra -Wpedantic
BASE    = -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L

BUILD_DIR = build

# Optional dependency prefixes -----------------------------------------

OPENSSL_DIR ?=
SRTP_DIR    ?=
X264_DIR    ?=

DEP_INCLUDES =
DEP_LIBDIRS  =

ifneq ($(OPENSSL_DIR),)
  DEP_INCLUDES += -I$(OPENSSL_DIR)/include
  DEP_LIBDIRS  += -L$(OPENSSL_DIR)/lib
endif

ifneq ($(SRTP_DIR),)
  DEP_INCLUDES += -I$(SRTP_DIR)/include
  DEP_LIBDIRS  += -L$(SRTP_DIR)/lib
endif

ifneq ($(X264_DIR),)
  DEP_INCLUDES += -I$(X264_DIR)/include
  DEP_LIBDIRS  += -L$(X264_DIR)/lib
endif

# libx264 autodetection --------------------------------------------------

X264_CANDIDATES = $(X264_DIR)/include/x264.h /usr/include/x264.h \
                  /usr/local/include/x264.h

HAVE_X264 ?= $(firstword $(foreach f,$(X264_CANDIDATES),$(if $(wildcard $f),1,)))
ifeq ($(HAVE_X264),)
  HAVE_X264 = 0
endif

# Primary target: camstream ----------------------------------------------

APP_INCLUDES = -Iinclude -Iinclude/app -Iinclude/media -Iinclude/webrtc \
               -Iinclude/janus -Ithird_party/mongoose $(DEP_INCLUDES)

APP_CFLAGS = $(BASE) $(WARN) $(CFLAGS) $(APP_INCLUDES) \
             -DHAVE_X264=$(HAVE_X264)

# Native WebRTC stack (OpenSSL + libsrtp2)
NATIVE_WEBRTC = \
	src/webrtc/ice_lite.c \
	src/webrtc/dtls_srtp.c \
	src/webrtc/rtp_h264.c \
	src/webrtc/rtcp.c \
	src/webrtc/webrtc_session.c

# libpeer backend (mbedTLS + libsrtp + usrsctp)
LIBPEER_WEBRTC = \
	src/webrtc/libpeer_global.c \
	src/webrtc/webrtc_session_libpeer.c

# Janus transport backend (plain RTP to an external Janus gateway).
# No OpenSSL/libsrtp: Janus owns signaling, ICE, DTLS and SRTP.
# Reuses the existing RFC 6184 packetizer and the RTCP helpers.
JANUS_TRANSPORT = \
	src/janus/janus_rtp_sender.c \
	src/webrtc/rtp_h264.c \
	src/webrtc/rtcp.c

# Common sources (including SDP parser used by both backends for logging)
APP_COMMON = \
	src/camstream_main.c \
	src/app/app_server.c \
	src/app/app_config.c \
	src/app/web_ui.c \
	src/media/frame_pool.c \
	src/media/frame_hub.c \
	src/media/au_ring.c \
	src/media/source_worker.c \
	src/media/encoder_worker.c \
	src/media/v4l2_source.c \
	src/media/test_source.c \
	src/media/yuv_convert.c \
	src/media/h264_encoder.c \
	src/media/encoder_v4l2m2m.c \
	src/webrtc/sdp.c \
	third_party/mongoose/mongoose.c

APP_SOURCES = $(APP_COMMON) $(NATIVE_WEBRTC)

# The libx264 backend is only compiled in when x264 was detected.
ifeq ($(HAVE_X264),1)
  APP_SOURCES += src/media/encoder_x264.c
endif

APP_OBJECTS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(APP_SOURCES))

# libm last: x264 math symbols resolve from archives seen later
X264_LIB =
ifeq ($(HAVE_X264),1)
  X264_LIB = -lx264
endif

APP_LIBS = $(DEP_LIBDIRS) -lssl -lcrypto -lsrtp2 -lpthread $(X264_LIB) -lm

# libpeer build detection
LIBPEER_SRC ?= $(BUILD_DIR)/libpeer-src
LIBPEER_BUILD ?= $(BUILD_DIR)/libpeer
LIBPEER_DIST = $(LIBPEER_BUILD)/dist
LIBPEER_INCLUDES = -I$(LIBPEER_DIST)/include -I$(LIBPEER_SRC)/src -I$(LIBPEER_SRC)/include -I$(LIBPEER_SRC)/third_party/cJSON
LIBPEER_LIBDIR = -L$(LIBPEER_DIST)/lib
# libpeer static libs (order matters) - only if built
LIBPEER_LIB_EXISTS = $(wildcard $(LIBPEER_DIST)/lib/libpeer.a)
ifeq ($(LIBPEER_LIB_EXISTS),)
  LIBPEER_LIBS = -lpthread -lm
else
  LIBPEER_LIBS = $(LIBPEER_LIBDIR) -lpeer -lsrtp2 -lusrsctp -lmbedtls -lmbedx509 -lmbedcrypto -lcjson -lpthread -lm
endif

# libpeer variant objects
APP_SOURCES_LIBPEER = $(APP_COMMON) $(LIBPEER_WEBRTC)
ifeq ($(HAVE_X264),1)
  APP_SOURCES_LIBPEER += src/media/encoder_x264.c
endif
APP_OBJECTS_LIBPEER = $(patsubst %.c,$(BUILD_DIR)/libpeer/%.o,$(APP_SOURCES_LIBPEER))
APP_CFLAGS_LIBPEER = $(BASE) $(WARN) $(CFLAGS) $(APP_INCLUDES) $(LIBPEER_INCLUDES) -DUSE_LIBPEER=1 -DHAVE_X264=$(HAVE_X264)
APP_LIBS_LIBPEER = $(DEP_LIBDIRS) $(LIBPEER_LIBS) -lpthread $(X264_LIB) -lm

# Janus variant objects: C application + RTP output, no WebRTC stack
APP_SOURCES_JANUS = $(APP_COMMON) $(JANUS_TRANSPORT)
ifeq ($(HAVE_X264),1)
  APP_SOURCES_JANUS += src/media/encoder_x264.c
endif
APP_OBJECTS_JANUS = $(patsubst %.c,$(BUILD_DIR)/janus/%.o,$(APP_SOURCES_JANUS))
APP_CFLAGS_JANUS = $(BASE) $(WARN) $(CFLAGS) $(APP_INCLUDES) -DUSE_JANUS_TRANSPORT=1 -DHAVE_X264=$(HAVE_X264)
# Minimal deps: no OpenSSL, no libsrtp. Janus is a separate process.
APP_LIBS_JANUS = $(DEP_LIBDIRS) $(X264_LIB) -lpthread -lm

# The Janus web page is embedded at build time from web/janus/.
JANUS_WEB_SOURCES = web/janus/index.html web/janus/janus-client.js
JANUS_WEB_HEADER  = $(BUILD_DIR)/janus/janus_web_assets.h

# Rules ------------------------------------------------------------------

.PHONY: all camstream camstream-janus camstream-libpeer libpeer vision-capture clean help test vision-test libpeer-backend test-janus

all: camstream

# Optional: clone and build upstream libpeer in ignored build/ directories.
# This is deliberately separate: libpeer uses mbedTLS and its own SRTP
# dependency graph, while camstream uses OpenSSL/libsrtp2.
libpeer:
	tools/setup-libpeer.sh

camstream: $(BUILD_DIR)/camstream

# Native backend (OpenSSL + libsrtp2)
$(BUILD_DIR)/camstream: $(APP_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) $(APP_OBJECTS) -o $@ $(APP_LIBS)
	@echo ""
	@echo "built $(BUILD_DIR)/camstream (native WebRTC, x264: $(if $(filter 1,$(HAVE_X264)),yes,no))"

# Janus transport backend (RTP to an external Janus gateway)
camstream-janus: $(BUILD_DIR)/camstream-janus

$(BUILD_DIR)/camstream-janus: $(APP_OBJECTS_JANUS) $(JANUS_WEB_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS_JANUS) $(APP_OBJECTS_JANUS) -o $@ $(APP_LIBS_JANUS)
	@echo ""
	@echo "built $(BUILD_DIR)/camstream-janus (Janus RTP transport, x264: $(if $(filter 1,$(HAVE_X264)),yes,no))"
	@echo "  deps: none beyond libc/libpthread (Janus runs as a separate process)"
	@echo "  run: ./build/camstream-janus --test --encoder sw"
	@echo "  then install config/janus/*.jcfg into /etc/janus/ and start janus"

$(BUILD_DIR)/janus/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS_JANUS) -c $< -o $@

# The Janus dashboard, embedded from web/janus/ at build time.
$(BUILD_DIR)/embed_assets: tools/embed_assets.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) -O2 -o $@ $<

$(JANUS_WEB_HEADER): $(JANUS_WEB_SOURCES) $(BUILD_DIR)/embed_assets
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/embed_assets $@ JANUS_WEB_ASSETS_H \
		janus_web_index_html web/janus/index.html \
		janus_web_client_js web/janus/janus-client.js

# The Janus dashboard string is generated; same pedantic exemption
# as the hand-written page above.
$(BUILD_DIR)/janus/src/app/web_ui.o: src/app/web_ui.c $(JANUS_WEB_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(filter-out -Wpedantic,$(WARN)) $(CFLAGS) $(APP_INCLUDES) \
	       -I$(BUILD_DIR)/janus -DUSE_JANUS_TRANSPORT=1 -DHAVE_X264=$(HAVE_X264) \
	       -c $< -o $@

$(BUILD_DIR)/janus/third_party/mongoose/mongoose.o: third_party/mongoose/mongoose.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(CFLAGS) -Ithird_party/mongoose -include alloca.h -c $< -o $@

# libpeer backend (mbedTLS + bundled deps) - fully migrated runtime
camstream-libpeer: $(BUILD_DIR)/camstream-libpeer

libpeer-backend: camstream-libpeer

$(BUILD_DIR)/camstream-libpeer: $(APP_OBJECTS_LIBPEER)
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS_LIBPEER) $(APP_OBJECTS_LIBPEER) -o $@ $(APP_LIBS_LIBPEER)
	@echo ""
	@echo "built $(BUILD_DIR)/camstream-libpeer (libpeer WebRTC, x264: $(if $(filter 1,$(HAVE_X264)),yes,no))"
	@echo "  deps: mbedTLS + libsrtp2 + usrsctp + cJSON (bundled via libpeer)"
	@echo "  run: ./build/camstream-libpeer --test --encoder sw --listen 0.0.0.0 --http-port 8000"

$(BUILD_DIR)/libpeer/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS_LIBPEER) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -c $< -o $@

# The embedded web page is one long string literal, beyond the
# 4095 byte ISO minimum, so pedantic mode is disabled for it.
$(BUILD_DIR)/src/app/web_ui.o: src/app/web_ui.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(filter-out -Wpedantic,$(WARN)) $(CFLAGS) $(APP_INCLUDES) \
	       -DHAVE_X264=$(HAVE_X264) -c $< -o $@

$(BUILD_DIR)/libpeer/src/app/web_ui.o: src/app/web_ui.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(filter-out -Wpedantic,$(WARN)) $(CFLAGS) $(APP_INCLUDES) $(LIBPEER_INCLUDES) \
	       -DUSE_LIBPEER=1 -DHAVE_X264=$(HAVE_X264) -c $< -o $@

$(BUILD_DIR)/third_party/mongoose/mongoose.o: third_party/mongoose/mongoose.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(CFLAGS) -Ithird_party/mongoose -include alloca.h -c $< -o $@

$(BUILD_DIR)/libpeer/third_party/mongoose/mongoose.o: third_party/mongoose/mongoose.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(CFLAGS) -Ithird_party/mongoose -include alloca.h -c $< -o $@

$(BUILD_DIR)/test_stun: tests/test_stun.c src/webrtc/ice_lite.c include/webrtc/ice_lite.h
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(WARN) $(CFLAGS) $(APP_INCLUDES) \
	      tests/test_stun.c src/webrtc/ice_lite.c -o $@ $(DEP_LIBDIRS) -lcrypto

$(BUILD_DIR)/test_vision: tests/test_vision.c src/vision/frame_matrix.c include/vision/frame_matrix.h
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(WARN) $(CFLAGS) -Iinclude/vision \
	      tests/test_vision.c src/vision/frame_matrix.c -o $@

$(BUILD_DIR)/test_encoder_worker: tests/test_encoder_worker.c \
        src/media/encoder_worker.c src/media/frame_hub.c \
        src/media/frame_pool.c src/media/au_ring.c \
        src/media/yuv_convert.c include/media/encoder_worker.h
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(WARN) $(CFLAGS) -Iinclude -Iinclude/media \
	  tests/test_encoder_worker.c src/media/encoder_worker.c \
	  src/media/frame_hub.c src/media/frame_pool.c \
	  src/media/au_ring.c src/media/yuv_convert.c -o $@ -lpthread

# Janus sender test: synthetic AUs through the real sender against
# a local fake-Janus UDP socket. No camera, no x264, no Janus needed.
$(BUILD_DIR)/test_janus_sender: tests/test_janus_sender.c \
        src/janus/janus_rtp_sender.c src/webrtc/rtp_h264.c \
        src/webrtc/rtcp.c src/media/au_ring.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(WARN) $(CFLAGS) -Iinclude -Iinclude/media \
	  -Iinclude/webrtc -Iinclude/janus \
	  tests/test_janus_sender.c src/janus/janus_rtp_sender.c \
	  src/webrtc/rtp_h264.c src/webrtc/rtcp.c src/media/au_ring.c \
	  -o $@ -lpthread

$(BUILD_DIR)/vision-capture: src/vision/vision_capture.c src/vision/vision_worker.c \
        src/vision/frame_matrix.c src/media/source_worker.c src/media/frame_hub.c \
        src/media/frame_pool.c src/media/v4l2_source.c src/media/test_source.c \
        include/vision/vision_worker.h include/vision/frame_matrix.h
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(WARN) $(CFLAGS) -Iinclude -Iinclude/media -Iinclude/vision \
	      src/vision/vision_capture.c src/vision/vision_worker.c src/vision/frame_matrix.c \
	      src/media/source_worker.c src/media/frame_hub.c src/media/frame_pool.c \
	      src/media/v4l2_source.c src/media/test_source.c -o $@ -lpthread -lm

vision-capture: $(BUILD_DIR)/vision-capture

vision-test: $(BUILD_DIR)/test_vision
	$(BUILD_DIR)/test_vision

test: $(BUILD_DIR)/test_stun $(BUILD_DIR)/test_vision $(BUILD_DIR)/test_encoder_worker
	$(BUILD_DIR)/test_stun
	$(BUILD_DIR)/test_vision
	$(BUILD_DIR)/test_encoder_worker

test-janus: $(BUILD_DIR)/test_janus_sender
	$(BUILD_DIR)/test_janus_sender

# Documentation lint: dash style, link resolution, anchor and index coverage.
.PHONY: check-docs
check-docs:
	./tools/check_docs.sh

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "targets:"
	@echo "  make                  build build/camstream (native WebRTC, minimal deps)"
	@echo "  make camstream-janus  build build/camstream-janus (H.264 RTP -> external Janus gateway)"
	@echo "  make camstream-libpeer build build/camstream-libpeer (libpeer runtime, Phase 3 fully migrated)"
	@echo "  make libpeer          clone/build upstream libpeer in build/ (needs network)"
	@echo "  make libpeer-backend  alias for camstream-libpeer"
	@echo "  make test             run the STUN, vision and encoder-worker unit tests"
	@echo "  make test-janus       unit test for the Janus RTP sender (no camera/Janus needed)"
	@echo "  make vision-capture   build the PGM/OBJ mosaic smoke tool"
	@echo "  make vision-test      build and run the vision unit test alone"
	@echo "  make check-docs       lint README.md and docs/ (dash style, links, index)"
	@echo "  make clean            remove build/"
	@echo ""
	@echo "examples:"
	@echo "  make -j2 && ./build/camstream --test --encoder sw --listen 0.0.0.0 --http-port 8080"
	@echo "  make camstream-janus -j2 && ./build/camstream-janus --test --encoder sw"
	@echo "      (first: install config/janus/*.jcfg into /etc/janus/ and start janus)"
	@echo "  make libpeer && make camstream-libpeer -j2 && ./build/camstream-libpeer --test --encoder sw --listen 0.0.0.0 --http-port 8000"
	@echo ""
	@echo "overrides:"
	@echo "  OPENSSL_DIR=... SRTP_DIR=... X264_DIR=...  dependency prefixes"
	@echo "  HAVE_X264=0/1                               force x264 on or off"
	@echo "  USE_LIBPEER=1                               force libpeer backend"
