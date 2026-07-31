// Lets the test drive the clock's notion of "now". main.cpp is compiled
// with -Dgettimeofday=test_gettimeofday so it reads this instead of libc.
// The signatures must match glibc's exactly, since the -D also renames
// glibc's own prototypes in <sys/time.h>.
#pragma once
#include <sys/time.h>

extern double g_testNow; // seconds since epoch, fractional

inline int test_gettimeofday(struct timeval *__restrict tv,
                             void *__restrict) noexcept {
  tv->tv_sec = (time_t)g_testNow;
  tv->tv_usec = (suseconds_t)((g_testNow - (double)(time_t)g_testNow) * 1e6);
  return 0;
}
inline int test_settimeofday(const struct timeval *tv,
                             const struct timezone *) noexcept {
  g_testNow = (double)tv->tv_sec;
  return 0;
}
