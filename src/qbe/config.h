/* simple(v0.55): host default target, selected at build time by the
 * Makefile (which regenerates this file). Checked in so a fresh clone
 * builds without a configure step. */
#if defined(__APPLE__)
#  if defined(__aarch64__)
#    define Deftgt T_arm64_apple
#  else
#    define Deftgt T_amd64_apple
#  endif
#elif defined(__aarch64__)
#  define Deftgt T_arm64
#elif defined(__riscv)
#  define Deftgt T_rv64
#else
#  define Deftgt T_amd64_sysv
#endif
