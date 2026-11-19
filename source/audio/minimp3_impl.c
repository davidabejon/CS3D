/* The minimp3 decoder (CC0, https://github.com/lieff/minimp3), compiled once.
 * build.py --fetch-deps downloads the pinned header into deps/minimp3/.
 *
 * Always optimized: at -O0 (python build.py --debug) an Old 3DS could not
 * decode 48 kHz stereo in real time. */
#pragma GCC optimize("O2")
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
