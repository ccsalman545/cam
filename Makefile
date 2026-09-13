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
               -Ithird_party/mongoose $(DEP_INCLUDES)

APP_CFLAGS = $(BASE) $(WARN) $(CFLAGS) $(APP_INCLUDES) \
             -DHAVE_X264=$(HAVE_X264)

APP_SOURCES = \
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
	src/webrtc/ice_lite.c \
	src/webrtc/dtls_srtp.c \
	src/webrtc/rtp_h264.c \
	src/webrtc/rtcp.c \
	src/webrtc/sdp.c \
	src/webrtc/webrtc_session.c \
	third_party/mongoose/mongoose.c

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

# Rules ------------------------------------------------------------------

.PHONY: all camstream libpeer clean help test

all: camstream

# Optional: clone and build upstream libpeer in ignored build/ directories.
# This is deliberately separate: libpeer uses mbedTLS and its own SRTP
# dependency graph, while camstream uses OpenSSL/libsrtp2.
libpeer:
	tools/setup-libpeer.sh

camstream: $(BUILD_DIR)/camstream

$(BUILD_DIR)/camstream: $(APP_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) $(APP_OBJECTS) -o $@ $(APP_LIBS)
	@echo ""
	@echo "built $(BUILD_DIR)/camstream (x264: $(if $(filter 1,$(HAVE_X264)),yes,no))"

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -c $< -o $@

# The embedded web page is one long string literal, beyond the
# 4095 byte ISO minimum, so pedantic mode is disabled for it.
$(BUILD_DIR)/src/app/web_ui.o: src/app/web_ui.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(filter-out -Wpedantic,$(WARN)) $(CFLAGS) $(APP_INCLUDES) \
	       -DHAVE_X264=$(HAVE_X264) -c $< -o $@

$(BUILD_DIR)/third_party/mongoose/mongoose.o: third_party/mongoose/mongoose.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(CFLAGS) -Ithird_party/mongoose -include alloca.h -c $< -o $@

$(BUILD_DIR)/test_stun: tests/test_stun.c src/webrtc/ice_lite.c include/webrtc/ice_lite.h
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(WARN) $(CFLAGS) $(APP_INCLUDES) \
	      tests/test_stun.c src/webrtc/ice_lite.c -o $@ $(DEP_LIBDIRS) -lcrypto

test: $(BUILD_DIR)/test_stun
	$(BUILD_DIR)/test_stun

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "targets:"
	@echo "  make            build build/camstream (WebRTC server)"
	@echo "  make libpeer    clone/build upstream libpeer in build/ (optional)"
	@echo "  make clean      remove build/"
	@echo ""
	@echo "overrides:"
	@echo "  OPENSSL_DIR=... SRTP_DIR=... X264_DIR=...  dependency prefixes"
	@echo "  HAVE_X264=0/1                               force x264 on or off"
