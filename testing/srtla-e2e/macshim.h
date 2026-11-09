/* Compatibility header so that BELABOX's srtla_send (Linux-only sources) compiles on macOS
 * without any change to its source files: passed via -include by build.sh. */
#pragma once
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0   /* sockets stay blocking; srtla_send only reads after select() */
#endif
#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htobe16(x) OSSwapHostToBigInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#endif
