// glibc's older math symbols (Linux: the game and the updater).
//
// glibc 2.43 and 2.44 gave these functions new symbol versions under the same names, and a build on a current
// distribution binds to those: the programs then would not start with an older glibc (Ubuntu 24.04 / Linux Mint 22:
// 2.39; SteamOS, Debian 13: 2.41). The link sends every call (also FFmpeg's, SDL's and ImGui's) here instead (Makefile
// GLIBC_WRAP: -Wl,--wrap), and these call the baseline versions for each host ABI. The Makefile checks after the link that
// nothing newer than GLIBC_MAX is left.
float glibc_old_sqrtf(float), glibc_old_acosf(float), glibc_old_asinf(float), glibc_old_atan2f(float, float);
float glibc_old_log10f(float);
double glibc_old_sinh(double), glibc_old_cosh(double);
#if defined(__aarch64__)
#define MATH_GLIBC "GLIBC_2.17"
#elif defined(__x86_64__)
#define MATH_GLIBC "GLIBC_2.2.5"
#else
#error "Unsupported glibc architecture"
#endif
__asm__(".symver glibc_old_sqrtf, sqrtf@" MATH_GLIBC);
__asm__(".symver glibc_old_acosf, acosf@" MATH_GLIBC);
__asm__(".symver glibc_old_asinf, asinf@" MATH_GLIBC);
__asm__(".symver glibc_old_atan2f, atan2f@" MATH_GLIBC);
__asm__(".symver glibc_old_log10f, log10f@" MATH_GLIBC);
__asm__(".symver glibc_old_sinh, sinh@" MATH_GLIBC);
__asm__(".symver glibc_old_cosh, cosh@" MATH_GLIBC);
float __wrap_sqrtf(float x) { return glibc_old_sqrtf(x); }
float __wrap_acosf(float x) { return glibc_old_acosf(x); }
float __wrap_asinf(float x) { return glibc_old_asinf(x); }
float __wrap_atan2f(float y, float x) { return glibc_old_atan2f(y, x); }
float __wrap_log10f(float x) { return glibc_old_log10f(x); }
double __wrap_sinh(double x) { return glibc_old_sinh(x); }
double __wrap_cosh(double x) { return glibc_old_cosh(x); }
