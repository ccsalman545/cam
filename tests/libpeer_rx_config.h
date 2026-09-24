/*
 * libpeer_rx_config.h
 *
 * Forced include (-include) for the libpeer copy inside
 * test_libpeer_stream only. libpeer's src/config.h defines CONFIG_MTU
 * without an #ifndef guard, so -DCONFIG_MTU cannot change it: include it
 * here first (its include guard then turns every later #include into a
 * no-op) and replace the value.
 *
 * Why: libpeer receives into a CONFIG_MTU byte buffer, while a libpeer
 * sender emits CONFIG_MTU byte RTP packets plus a 10 byte SRTP tag. With
 * equal values every full-size packet is truncated and fails SRTP
 * authentication. The receiver needs room for the tag; browsers have it.
 */
#include "config.h"

#undef CONFIG_MTU
#define CONFIG_MTU 1500
