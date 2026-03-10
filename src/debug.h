// debug.h — Conditional debug logging for SneakPeak
#pragma once

#ifdef SNEAKPEAK_DEBUG
  #include <cstdio>
  #define DBG(...) do { \
    FILE* _f = fopen("/tmp/sneakpeak_debug.log", "a"); \
    if (_f) { fprintf(_f, __VA_ARGS__); fclose(_f); } \
  } while(0)
#else
  #define DBG(...) ((void)0)
#endif
