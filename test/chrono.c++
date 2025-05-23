/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2025
/// \copyright SPDX-License-Identifier: Apache-2.0

#include "test.h++"

namespace chrono {

#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000u
#endif /* NSEC_PER_SEC */

uint32_t ns2fractional(uint32_t ns) {
  assert(ns < NSEC_PER_SEC);
  /* LY: здесь и далее используется "длинное деление", которое
   * для ясности кода оставлено как есть (без ручной оптимизации). Так как
   * GCC, Clang и даже MSVC сами давно умеют конвертировать деление на
   * константу в быструю reciprocal-форму. */
  return uint32_t((uint64_t(ns) << 32) / NSEC_PER_SEC);
}

uint32_t fractional2ns(uint32_t fractional) { return uint32_t((fractional * uint64_t(NSEC_PER_SEC)) >> 32); }

#ifndef USEC_PER_SEC
#define USEC_PER_SEC 1000000u
#endif /* USEC_PER_SEC */
uint32_t us2fractional(uint32_t us) {
  assert(us < USEC_PER_SEC);
  return uint32_t((uint64_t(us) << 32) / USEC_PER_SEC);
}

uint32_t fractional2us(uint32_t fractional) {
#if !(defined(_M_ARM) || defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64))
  /* Смеяться или плакать, но все существующие на май 2024 компиляторы Microsoft
   * для ARM/ARM64, уже порядка 10 лет, падают на этом коде из-за внтутренней
   * ошибке (aka ICE). */
  return uint32_t((fractional * uint64_t(USEC_PER_SEC)) >> 32);
#else
  static_assert(USEC_PER_SEC % 16 == 0, "WTF?");
  /* Crutch for MSVC ARM/ARM64 compilers to avoid internal compiler error. */
  return UInt32x32To64(fractional, USEC_PER_SEC / 16) >> 28;
#endif
}

#ifndef MSEC_PER_SEC
#define MSEC_PER_SEC 1000u
#endif /* MSEC_PER_SEC */
uint32_t ms2fractional(uint32_t ms) {
  assert(ms < MSEC_PER_SEC);
  return uint32_t((uint64_t(ms) << 32) / MSEC_PER_SEC);
}

uint32_t fractional2ms(uint32_t fractional) { return uint32_t((fractional * uint64_t(MSEC_PER_SEC)) >> 32); }

time from_ns(uint64_t ns) {
  time result;
  result.fixedpoint = ((ns / NSEC_PER_SEC) << 32) | ns2fractional(uint32_t(ns % NSEC_PER_SEC));
  return result;
}

time from_us(uint64_t us) {
  time result;
  result.fixedpoint = ((us / USEC_PER_SEC) << 32) | us2fractional(uint32_t(us % USEC_PER_SEC));
  return result;
}

time from_ms(uint64_t ms) {
  time result;
  result.fixedpoint = ((ms / MSEC_PER_SEC) << 32) | ms2fractional(uint32_t(ms % MSEC_PER_SEC));
  return result;
}

#if __GNUC_PREREQ(8, 0) && (defined(__MINGW__) || defined(__MINGW32__) || defined(__MINGW64__))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif /* GCC/MINGW */

time now_realtime() {
#if defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS)
  static void(WINAPI * query_time)(LPFILETIME);
  if (unlikely(!query_time)) {
    HMODULE hModule = GetModuleHandle(TEXT("kernel32.dll"));
    if (hModule)
      query_time = (void(WINAPI *)(LPFILETIME))GetProcAddress(hModule, "GetSystemTimePreciseAsFileTime");
    if (!query_time)
      query_time = GetSystemTimeAsFileTime;
  }

  FILETIME filetime;
  query_time(&filetime);
  uint64_t ns100 = (uint64_t)filetime.dwHighDateTime << 32 | filetime.dwLowDateTime;
  return from_ns((ns100 - UINT64_C(116444736000000000)) * 100u);
#else
  struct timespec ts;
  if (unlikely(clock_gettime(CLOCK_REALTIME, &ts)))
    failure_perror("clock_gettime(CLOCK_REALTIME", errno);

  return from_timespec(ts);
#endif
}

time now_monotonic() {
#if defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS)
  static uint64_t reciprocal;
  static LARGE_INTEGER Frequency;
  if (reciprocal == 0) {
    if (!QueryPerformanceFrequency(&Frequency))
      failure_perror("QueryPerformanceFrequency()", GetLastError());
    reciprocal = (((UINT64_C(1) << 48) + Frequency.QuadPart / 2 + 1) / Frequency.QuadPart);
    assert(reciprocal);
  }

  LARGE_INTEGER Counter;
  if (!QueryPerformanceCounter(&Counter))
    failure_perror("QueryPerformanceCounter()", GetLastError());

  time result;
  result.fixedpoint = (Counter.QuadPart / Frequency.QuadPart) << 32;
  uint64_t mod = Counter.QuadPart % Frequency.QuadPart;
  result.fixedpoint += (mod * reciprocal) >> 16;
  return result;
#else
  struct timespec ts;
  if (unlikely(clock_gettime(CLOCK_MONOTONIC, &ts)))
    failure_perror("clock_gettime(CLOCK_MONOTONIC)", errno);

  return from_timespec(ts);
#endif
}

#if __GNUC_PREREQ(8, 0) && (defined(__MINGW__) || defined(__MINGW32__) || defined(__MINGW64__))
#pragma GCC diagnostic pop
#endif /* GCC/MINGW */

} /* namespace chrono */
