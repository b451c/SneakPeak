// denormals.h — flush-to-zero for worker threads that filter long silences.
//
// A biquad decaying into a silence never reaches zero: its state settles on
// the smallest subnormal (4.9e-324, a fixed point of the DF1 recurrence), and
// x86 SSE runs every operation on such values through microcode (~100x
// slower). ARM handles subnormals at full speed, so the mode is a no-op there
// in practice; the per-thread flag costs nothing either way (audit A7.3).
#pragma once

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #include <xmmintrin.h>
  #include <pmmintrin.h>
  inline void FlushDenormalsToZero()
  {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
  }
#elif defined(__aarch64__) && !defined(_MSC_VER)
  #include <cstdint>
  inline void FlushDenormalsToZero()
  {
    uint64_t fpcr = 0;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ull << 24);   // FZ: flush subnormal results to zero
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
  }
#else
  inline void FlushDenormalsToZero() {}
#endif
