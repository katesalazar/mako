/*!
 * mpi.c - multi-precision integers for mako
 * Copyright (c) 2020, Christopher Jeffrey (MIT License).
 * https://github.com/chjj/mako
 *
 * A from-scratch reimplementation of GMP.
 *
 * References:
 *
 *   [KNUTH] The Art of Computer Programming,
 *           Volume 2, Seminumerical Algorithms
 *     Donald E. Knuth
 *     https://www-cs-faculty.stanford.edu/~knuth/taocp.html
 *
 *   [MONT] Efficient Software Implementations of Modular Exponentiation
 *     Shay Gueron
 *     https://eprint.iacr.org/2011/239.pdf
 *
 *   [DIV] Improved division by invariant integers
 *     Niels Möller, Torbjörn Granlund
 *     https://gmplib.org/~tege/division-paper.pdf
 *
 *   [JACOBI] A Binary Algorithm for the Jacobi Symbol
 *     J. Shallit, J. Sorenson
 *     https://www.researchgate.net/publication/2273750
 *
 *   [HANDBOOK] Handbook of Applied Cryptography
 *     A. Menezes, P. van Oorschot, S. Vanstone
 *
 *   [LUCAS] Lucas Pseudoprimes
 *     R. Baillie, S. Wagstaff
 *     https://www.ams.org/journals/mcom/1980-35-152/S0025-5718-1980-0583518-6/S0025-5718-1980-0583518-6.pdf
 *
 *   [BPSW] The Baillie-PSW Primality Test
 *     Thomas R. Nicely
 *     https://web.archive.org/web/20130828131627/http://www.trnicely.net/misc/bpsw.html
 *
 *   [ARITH] Modern Computer Arithmetic
 *     Richard P. Brent, Paul Zimmermann
 *     https://members.loria.fr/PZimmermann/mca/pub226.html
 */

#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>

#include <mako/mpi.h>

#include "internal.h"

/*
 * Options
 */

#undef MP_USE_DIV_2BY1_ASM
#undef MP_USE_DIV_3BY2_ASM
#undef MP_USE_DIV_3BY2

/*
 * Wide Type
 */

#if MP_LIMB_BITS == 64
#  ifdef BTC_HAVE_INT128
typedef btc_uint128_t mp_wide_t;
#    define MP_HAVE_WIDE
#  endif
#else
typedef uint64_t mp_wide_t;
#  define MP_HAVE_WIDE
#endif

BTC_BARRIER(mp_limb_t, mp_limb)

/*
 * Alloca Compat
 */

#if defined(__TINYC__)
/* include <stddef.h> */
#  define mp_alloca alloca
#elif defined(__NWCC__)
/* Ignore. alloca is hacked in as malloc/free. */
#elif defined(__GNUC__) || BTC_HAS_BUILTIN(__builtin_alloca)
#  define mp_alloca __builtin_alloca
#elif defined(__sun) && defined(__SVR4)
#  include <alloca.h>
#  define mp_alloca alloca
#elif defined(__MVS__)
/* include <stdlib.h> */
#  define mp_alloca alloca
#elif defined(__xlC__)
#  define mp_alloca __alloca
#elif defined(_MSC_VER) && defined(_WIN32)
#  include <malloc.h>
#  define mp_alloca _alloca
#elif defined(__BORLANDC__)
#  include <malloc.h>
#  define mp_alloca alloca
#elif defined(__WATCOMC__)
#  include <malloc.h>
#  define mp_alloca alloca
#elif defined(__DMC__) || (defined(__SC__) && defined(__ZTC__))
/* include <stdlib.h> */
#  define mp_alloca _alloca
#elif defined(__DECC) && defined(__VMS)
#  define mp_alloca __ALLOCA
#endif

/*
 * Assembly Compat
 */

#if defined(BTC_HAVE_ASM)
#  if defined(__amd64__) || defined(__amd64) \
   || defined(__x86_64__) || defined(__x86_64)
#    if MP_LIMB_BITS == 64
#      define MP_HAVE_ASM_X64
#    endif
#  elif defined(__i386__) || defined(__i386) || defined(i386)
#    if MP_LIMB_BITS == 32
#      define MP_HAVE_ASM_X86
#    endif
#  endif
#endif

#if defined(BTC_MSVC) && _MSC_VER >= 1400 /* VS 2005 */
/* Determine whether we can use MSVC inline ASM. */
#  if MP_LIMB_BITS == 64 && (defined(_M_AMD64) || defined(_M_X64))
#    pragma code_seg(".text")
#    define MP_MSVC_CODE __declspec(allocate(".text") align(16))
#    define MP_MSVC_ASM_X64
#  elif MP_LIMB_BITS == 32 && defined(_M_IX86)
#    define MP_MSVC_CDECL __cdecl
#    define MP_MSVC_ASM_X86
#  endif
#endif

#ifndef MP_MSVC_CDECL
#  define MP_MSVC_CDECL
#endif

/*
 * Backend Selection
 */

#if defined(MP_HAVE_ASM_X64) && defined(BTC_GNUC)
/* For some reason clang sucks at inlining ASM, but
   is extremely good at generating 128 bit carry code.
   GCC is the exact opposite! */
#  define MP_FAST_ASM_X64
#endif

#if defined(MP_HAVE_WIDE) && defined(__clang__)
/* Clang 5.0 and above produce efficient
   carry code with wider types and shifts. */
#  if defined(__apple_build_version__)
#    if __apple_build_version__ >= 9020039
#      define MP_FAST_WIDE
#    endif
#  elif defined(__clang_major__)
#    if __clang_major__ >= 5
#      define MP_FAST_WIDE
#    endif
#  endif
#endif

/*
 * Macros
 */

#define MP_MIN(x, y) ((x) < (y) ? (x) : (y))
#define MP_MAX(x, y) ((x) > (y) ? (x) : (y))
#define MP_ABS(x) ((x) < 0 ? -(x) : (x))

#if defined(mp_alloca)
/* Max stack allocation size for alloca: */
/* 1024 bytes (two 4096 bit RSA moduli). */
#  define mp_alloca_max ((2 * 4096) / MP_LIMB_BITS + 2)
#  define mp_alloca_limbs(n) ((mp_limb_t *)mp_alloca((n) * sizeof(mp_limb_t)))
#  define mp_alloc_vla(n) \
     ((n) > mp_alloca_max ? mp_alloc_limbs(n) : mp_alloca_limbs(n))
#  define mp_free_vla(p, n) \
     do { if ((n) > mp_alloca_max) mp_free_limbs(p); } while (0)
#  define mp_alloca_str(n) ((char *)mp_alloca(n))
#  define mp_alloc_vls(n) ((n) > 1024 ? mp_alloc_str(n) : mp_alloca_str(n))
#  define mp_free_vls(p, n) do { if ((n) > 1024) mp_free_str(p); } while (0)
#else
#  define mp_alloca_max 0
#  define mp_alloc_vla(n) mp_alloc_limbs(n)
#  define mp_free_vla(p, n) mp_free_limbs(p)
#  define mp_alloc_vls(n) mp_alloc_str(n)
#  define mp_free_vls(p, n) mp_free_str(p)
#endif

/*
 * Builtins
 */

#if MP_LIMB_MAX == UINT_MAX
#  if BTC_GNUC_PREREQ(3, 4) || BTC_HAS_BUILTIN(__builtin_popcount)
#    define mp_builtin_popcount __builtin_popcount
#  endif
#  if BTC_GNUC_PREREQ(3, 4) || BTC_HAS_BUILTIN(__builtin_clz)
#    define mp_builtin_clz __builtin_clz
#  endif
#  if BTC_GNUC_PREREQ(3, 4) || BTC_HAS_BUILTIN(__builtin_ctz)
#    define mp_builtin_ctz __builtin_ctz
#  endif
#elif MP_LIMB_MAX == ULONG_MAX
#  if BTC_GNUC_PREREQ(3, 4) || BTC_HAS_BUILTIN(__builtin_popcountl)
#    define mp_builtin_popcount __builtin_popcountl
#  endif
#  if BTC_GNUC_PREREQ(3, 4) || BTC_HAS_BUILTIN(__builtin_clzl)
#    define mp_builtin_clz __builtin_clzl
#  endif
#  if BTC_GNUC_PREREQ(3, 4) || BTC_HAS_BUILTIN(__builtin_ctzl)
#    define mp_builtin_ctz __builtin_ctzl
#  endif
#else /* MP_LIMB_MAX == ULLONG_MAX */
#  if BTC_GNUC_PREREQ(3, 4) || BTC_HAS_BUILTIN(__builtin_popcountll)
#    define mp_builtin_popcount __builtin_popcountll
#  endif
#  if BTC_GNUC_PREREQ(3, 4) || BTC_HAS_BUILTIN(__builtin_clzll)
#    define mp_builtin_clz __builtin_clzll
#  endif
#  if BTC_GNUC_PREREQ(3, 4) || BTC_HAS_BUILTIN(__builtin_ctzll)
#    define mp_builtin_ctz __builtin_ctzll
#  endif
#endif

#ifdef __PCC__
/* PCC tries to call out to a
   function that isn't linked. */
#  undef mp_builtin_popcount
#endif

/*
 * Intrinsics
 */

#if defined(BTC_MSVC) && _MSC_VER >= 1400 /* VS 2005 */
#  include <intrin.h>
#  if MP_LIMB_MAX == ULONG_MAX
#    pragma intrinsic(_BitScanReverse)
#    pragma intrinsic(_BitScanForward)
#    define mp_intrin_bsr _BitScanReverse
#    define mp_intrin_bsf _BitScanForward
#  elif MP_LIMB_BITS == 64 && (defined(_M_AMD64)  \
                            || defined(_M_X64)    \
                            || defined(_M_ARM64))
#    pragma intrinsic(_BitScanReverse64)
#    pragma intrinsic(_BitScanForward64)
#    define mp_intrin_bsr _BitScanReverse64
#    define mp_intrin_bsf _BitScanForward64
#  endif
#  if MP_LIMB_BITS == 64 && (defined(_M_AMD64) || defined(_M_X64))
#    pragma intrinsic(_umul128)
#    define MP_HAVE_UMUL128
#  endif
#  if MP_LIMB_BITS == 64 && defined(_M_ARM64)
#    pragma intrinsic(__umulh)
#    define MP_HAVE_UMULH
#  endif
#endif

#if defined(BTC_MSVC) && _MSC_VER >= 1920 /* VS 2019 RTM */
#  include <immintrin.h>
#  if MP_LIMB_BITS == 32 && MP_LIMB_MAX == UINT_MAX && defined(_M_IX86)
#    pragma intrinsic(_udiv64)
#    define MP_HAVE_UDIV64
#  endif
#  if MP_LIMB_BITS == 64 && (defined(_M_AMD64) || defined(_M_X64))
#    pragma intrinsic(_udiv128)
#    define MP_HAVE_UDIV128
#  endif
#endif

/*
 * Arithmetic Macros
 */

#if defined(MP_FAST_ASM_X64)

/* [z, c] = x + y */
#define mp_add(z, c, x, y) \
  __asm__ (                \
    "addq %q3, %q0\n"      \
    "movq $0, %q1\n"       \
    "setc %b1\n"           \
    : "=&r" (z), "=r" (c)  \
    : "%0" (x), "1" (y)    \
    : "cc"                 \
  )

/* [z, c] = x + c */
#define mp_add_x4(zp, c, xp) \
  __asm__ __volatile__ (     \
    "shrq %q0\n"             \
    "movq (%q2), %%r8\n"     \
    "adcq $0, %%r8\n"        \
    "movq %%r8, (%q1)\n"     \
    "movq 8(%q2), %%r8\n"    \
    "adcq $0, %%r8\n"        \
    "movq %%r8, 8(%q1)\n"    \
    "movq 16(%q2), %%r8\n"   \
    "adcq $0, %%r8\n"        \
    "movq %%r8, 16(%q1)\n"   \
    "movq 24(%q2), %%r8\n"   \
    "adcq $0, %%r8\n"        \
    "movq %%r8, 24(%q1)\n"   \
    "setb %b0\n"             \
    : "+&r" (c)              \
    : "r" (zp), "r" (xp)     \
    : "cc", "memory",        \
      "r8"                   \
  )

/* [z, c] = x - y */
#define mp_sub(z, c, x, y) \
  __asm__ (                \
    "subq %q3, %q0\n"      \
    "movq $0, %q1\n"       \
    "setc %b1\n"           \
    : "=&r" (z), "=r" (c)  \
    : "0" (x), "1" (y)     \
    : "cc"                 \
  )

/* [z, c] = x - c */
#define mp_sub_x4(zp, c, xp) \
  __asm__ __volatile__ (     \
    "shrq %q0\n"             \
    "movq (%q2), %%r8\n"     \
    "sbbq $0, %%r8\n"        \
    "movq %%r8, (%q1)\n"     \
    "movq 8(%q2), %%r8\n"    \
    "sbbq $0, %%r8\n"        \
    "movq %%r8, 8(%q1)\n"    \
    "movq 16(%q2), %%r8\n"   \
    "sbbq $0, %%r8\n"        \
    "movq %%r8, 16(%q1)\n"   \
    "movq 24(%q2), %%r8\n"   \
    "sbbq $0, %%r8\n"        \
    "movq %%r8, 24(%q1)\n"   \
    "setb %b0\n"             \
    : "+&r" (c)              \
    : "r" (zp), "r" (xp)     \
    : "cc", "memory",        \
      "r8"                   \
  )

/* [hi, lo] = x * y */
#define mp_mul(hi, lo, x, y) \
  __asm__ (                  \
    "mulq %q3\n"             \
    : "=a" (lo), "=d" (hi)   \
    : "%0" (x), "rm" (y)     \
    : "cc"                   \
  )

/* [hi, lo] = x^2 */
#define mp_sqr(hi, lo, x)  \
  __asm__ (                \
    "mulq %%rax\n"         \
    : "=a" (lo), "=d" (hi) \
    : "0" (x)              \
    : "cc"                 \
  )

/* [z, c] = x + y + c */
#define mp_add_1(z, c, x, y) \
  __asm__ (                  \
    "addq %q1, %q0\n"        \
    "setc %b1\n"             \
    "addq %q3, %q0\n"        \
    "adcq $0, %q1\n"         \
    : "=&r" (z), "+&r" (c)   \
    : "%0" (x), "rm" (y)     \
    : "cc"                   \
  )

/* [z, c] = x + y + c */
#define mp_add_1x4(zp, c, xp, yp) \
  __asm__ __volatile__ (          \
    "shrq %q0\n"                  \
    "movq (%q2), %%r8\n"          \
    "adcq (%q3), %%r8\n"          \
    "movq %%r8, (%q1)\n"          \
    "movq 8(%q2), %%r8\n"         \
    "adcq 8(%q3), %%r8\n"         \
    "movq %%r8, 8(%q1)\n"         \
    "movq 16(%q2), %%r8\n"        \
    "adcq 16(%q3), %%r8\n"        \
    "movq %%r8, 16(%q1)\n"        \
    "movq 24(%q2), %%r8\n"        \
    "adcq 24(%q3), %%r8\n"        \
    "movq %%r8, 24(%q1)\n"        \
    "setb %b0\n"                  \
    : "+&r" (c)                   \
    : "r" (zp), "%r" (xp),        \
      "r" (yp)                    \
    : "cc", "memory",             \
      "r8"                        \
  )

/* [z, c] = x - y - c = x - (y + c) */
#define mp_sub_1(z, c, x, y) \
  __asm__ (                  \
    "subq %q1, %q0\n"        \
    "setc %b1\n"             \
    "subq %q3, %q0\n"        \
    "adcq $0, %q1\n"         \
    : "=&r" (z), "+&r" (c)   \
    : "0" (x), "rm" (y)      \
    : "cc"                   \
  )

/* [z, c] = x - y - c = x - (y + c) */
#define mp_sub_1x4(zp, c, xp, yp) \
  __asm__ __volatile__ (          \
    "shrq %q0\n"                  \
    "movq (%q2), %%r8\n"          \
    "sbbq (%q3), %%r8\n"          \
    "movq %%r8, (%q1)\n"          \
    "movq 8(%q2), %%r8\n"         \
    "sbbq 8(%q3), %%r8\n"         \
    "movq %%r8, 8(%q1)\n"         \
    "movq 16(%q2), %%r8\n"        \
    "sbbq 16(%q3), %%r8\n"        \
    "movq %%r8, 16(%q1)\n"        \
    "movq 24(%q2), %%r8\n"        \
    "sbbq 24(%q3), %%r8\n"        \
    "movq %%r8, 24(%q1)\n"        \
    "setb %b0\n"                  \
    : "+&r" (c)                   \
    : "r" (zp), "r" (xp),         \
      "r" (yp)                    \
    : "cc", "memory",             \
      "r8"                        \
  )

/* [z, c] = x * y + c */
#define mp_mul_1(z, c, x, y)  \
  __asm__ (                   \
    "mulq %q3\n"              \
    "addq %q4, %%rax\n"       \
    "adcq $0, %%rdx\n"        \
    : "=&a" (z), "=&d" (c)    \
    : "%0" (x), "rm" (y),     \
      "rm" (c)                \
    : "cc"                    \
  )

/* [z, c] = z + x * y + c */
#define mp_addmul_1(z, c, x, y) \
  __asm__ (                     \
    "mulq %q3\n"                \
    "addq %q5, %%rax\n"         \
    "adcq $0, %%rdx\n"          \
    "addq %q4, %%rax\n"         \
    "adcq $0, %%rdx\n"          \
    : "=&a" (z), "=&d" (c)      \
    : "%0" (x), "rm" (y),       \
      "rm" (z), "rm" (c)        \
    : "cc"                      \
  )

/* [z, c] = z - x * y - c = z - (x * y + c) */
#define mp_submul_1(z, c, x, y) \
  __asm__ (                     \
    "movq %q2, %%rax\n"         \
    "mulq %q3\n"                \
    "addq %q4, %%rax\n"         \
    "adcq $0, %%rdx\n"          \
    "subq %%rax, %q0\n"         \
    "adcq $0, %%rdx\n"          \
    : "+r" (z), "=&d" (c)       \
    : "%rm" (x), "rm" (y),      \
      "rm" (c)                  \
    : "cc", "rax"               \
  )

/* [z, c] = ~x + c */
#define mp_neg_1(z, c, x)  \
  __asm__ (                \
    "notq %q0\n"           \
    "addq %q1, %q0\n"      \
    "setc %b1\n"           \
    : "=&r" (z), "+&r" (c) \
    : "0" (x)              \
    : "cc"                 \
  )

/* [z, c] = ~x + c */
#define mp_neg_1x4(zp, c, xp) \
  __asm__ __volatile__ (      \
    "shrq %q0\n"              \
    "movq (%q2), %%r8\n"      \
    "notq %%r8\n"             \
    "adcq $0, %%r8\n"         \
    "movq %%r8, (%q1)\n"      \
    "movq 8(%q2), %%r8\n"     \
    "notq %%r8\n"             \
    "adcq $0, %%r8\n"         \
    "movq %%r8, 8(%q1)\n"     \
    "movq 16(%q2), %%r8\n"    \
    "notq %%r8\n"             \
    "adcq $0, %%r8\n"         \
    "movq %%r8, 16(%q1)\n"    \
    "movq 24(%q2), %%r8\n"    \
    "notq %%r8\n"             \
    "adcq $0, %%r8\n"         \
    "movq %%r8, 24(%q1)\n"    \
    "setb %b0\n"              \
    : "+&r" (c)               \
    : "r" (zp), "r" (xp)      \
    : "cc", "memory",         \
      "r8"                    \
  )

#elif defined(MP_FAST_WIDE) /* !MP_FAST_ASM_X64 */

/* [z, c] = x + y */
#define mp_add(z, c, x, y) do {        \
  mp_wide_t _w = (mp_wide_t)(x) + (y); \
  (c) = _w >> MP_LIMB_BITS;            \
  (z) = _w;                            \
} while (0)

/* [z, c] = x - y */
#define mp_sub(z, c, x, y) do {        \
  mp_wide_t _w = (mp_wide_t)(x) - (y); \
  (c) = -(_w >> MP_LIMB_BITS);         \
  (z) = _w;                            \
} while (0)

/* [hi, lo] = x * y */
#define mp_mul(hi, lo, x, y) do {      \
  mp_wide_t _w = (mp_wide_t)(x) * (y); \
  (hi) = _w >> MP_LIMB_BITS;           \
  (lo) = _w;                           \
} while (0)

/* [hi, lo] = x^2 */
#define mp_sqr(hi, lo, x) do {         \
  mp_wide_t _w = (mp_wide_t)(x) * (x); \
  (hi) = _w >> MP_LIMB_BITS;           \
  (lo) = _w;                           \
} while (0)

/* [z, c] = x + y + c */
#define mp_add_1(z, c, x, y) do {            \
  mp_wide_t _w = (mp_wide_t)(x) + (y) + (c); \
  (c) = _w >> MP_LIMB_BITS;                  \
  (z) = _w;                                  \
} while (0)

/* [z, c] = x - y - c = x - (y + c) */
#define mp_sub_1(z, c, x, y) do {            \
  mp_wide_t _w = (mp_wide_t)(x) - (y) - (c); \
  (c) = -(_w >> MP_LIMB_BITS);               \
  (z) = _w;                                  \
} while (0)

/* [z, c] = x * y + c */
#define mp_mul_1(z, c, x, y) do {            \
  mp_wide_t _w = (mp_wide_t)(x) * (y) + (c); \
  (c) = _w >> MP_LIMB_BITS;                  \
  (z) = _w;                                  \
} while (0)

/* [z, c] = z + x * y + c */
#define mp_addmul_1(z, c, x, y) do {               \
  mp_wide_t _w = (z) + (mp_wide_t)(x) * (y) + (c); \
  (c) = _w >> MP_LIMB_BITS;                        \
  (z) = _w;                                        \
} while (0)

/* [z, c] = z - x * y - c = z - (x * y + c) */
#define mp_submul_1(z, c, x, y) do {               \
  mp_wide_t _w = (z) - (mp_wide_t)(x) * (y) - (c); \
  (c) = -(_w >> MP_LIMB_BITS);                     \
  (z) = _w;                                        \
} while (0)

/* [z, c] = ~x + c */
#define mp_neg_1(z, c, x) do {          \
  mp_wide_t _w = ~(x) + (mp_wide_t)(c); \
  (c) = _w >> MP_LIMB_BITS;             \
  (z) = _w;                             \
} while (0)

#else /* !MP_FAST_WIDE */

/* [z, c] = x + y */
#define mp_add(z, c, x, y) do { \
  mp_limb_t _z = (x) + (y);     \
  (c) = (_z < (y));             \
  (z) = _z;                     \
} while (0)

/* [z, c] = x - y */
#define mp_sub(z, c, x, y) do { \
  mp_limb_t _z = (x) - (y);     \
  (c) = (_z > (x));             \
  (z) = _z;                     \
} while (0)

#if defined(MP_HAVE_WIDE)

/* [hi, lo] = x * y */
#define mp_mul(hi, lo, x, y) do {      \
  mp_wide_t _w = (mp_wide_t)(x) * (y); \
  (hi) = _w >> MP_LIMB_BITS;           \
  (lo) = _w;                           \
} while (0)

/* [hi, lo] = x^2 */
#define mp_sqr(hi, lo, x) do {         \
  mp_wide_t _w = (mp_wide_t)(x) * (x); \
  (hi) = _w >> MP_LIMB_BITS;           \
  (lo) = _w;                           \
} while (0)

#elif defined(MP_HAVE_UMUL128) /* !MP_HAVE_WIDE */

/* [hi, lo] = x * y */
#define mp_mul(hi, lo, x, y) do { \
  (lo) = _umul128(x, y, &(hi));   \
} while (0)

/* [hi, lo] = x^2 */
#define mp_sqr(hi, lo, x) do {  \
  (lo) = _umul128(x, x, &(hi)); \
} while (0)

#elif defined(MP_HAVE_UMULH) /* !MP_HAVE_WIDE */

/* [hi, lo] = x * y */
#define mp_mul(hi, lo, x, y) do { \
  mp_limb_t _lo = (x) * (y);      \
  (hi) = __umulh(x, y);           \
  (lo) = _lo;                     \
} while (0)

/* [hi, lo] = x^2 */
#define mp_sqr(hi, lo, x) do { \
  mp_limb_t _lo = (x) * (x);   \
  (hi) = __umulh(x, x);        \
  (lo) = _lo;                  \
} while (0)

#elif defined(MP_HAVE_ASM_X64) /* !MP_HAVE_WIDE */

/* [hi, lo] = x * y */
#define mp_mul(hi, lo, x, y) \
  __asm__ (                  \
    "mulq %q3\n"             \
    : "=a" (lo), "=d" (hi)   \
    : "%0" (x), "rm" (y)     \
    : "cc"                   \
  )

/* [hi, lo] = x^2 */
#define mp_sqr(hi, lo, x)  \
  __asm__ (                \
    "mulq %%rax\n"         \
    : "=a" (lo), "=d" (hi) \
    : "0" (x)              \
    : "cc"                 \
  )

#else /* !MP_HAVE_WIDE */

/* [hi, lo] = x * y (muldwu.c in Hacker's Delight) */
#define mp_mul(hi, lo, x, y) do {       \
  mp_limb_t _u0, _u1, _v0, _v1, _k, _t; \
  mp_limb_t _w1, _w2, _w3;              \
                                        \
  _u0 = (x) >> MP_LOW_BITS;             \
  _u1 = (x) & MP_LOW_MASK;              \
  _v0 = (y) >> MP_LOW_BITS;             \
  _v1 = (y) & MP_LOW_MASK;              \
                                        \
  _t = _u1 * _v1;                       \
  _w3 = _t & MP_LOW_MASK;               \
  _k = _t >> MP_LOW_BITS;               \
                                        \
  _t = _u0 * _v1 + _k;                  \
  _w2 = _t & MP_LOW_MASK;               \
  _w1 = _t >> MP_LOW_BITS;              \
                                        \
  _t = _u1 * _v0 + _w2;                 \
  _k = _t >> MP_LOW_BITS;               \
                                        \
  (hi) = _u0 * _v0 + _w1 + _k;          \
  (lo) = (_t << MP_LOW_BITS) + _w3;     \
} while (0)

/* [hi, lo] = x^2 (muldwu.c in Hacker's Delight) */
#define mp_sqr(hi, lo, x) do {      \
  mp_limb_t _u0, _u1, _u2, _k, _t;  \
  mp_limb_t _w1, _w2, _w3;          \
                                    \
  _u0 = (x) >> MP_LOW_BITS;         \
  _u1 = (x) & MP_LOW_MASK;          \
  _u2 = _u0 * _u1;                  \
                                    \
  _t = _u1 * _u1;                   \
  _w3 = _t & MP_LOW_MASK;           \
  _k = _t >> MP_LOW_BITS;           \
                                    \
  _t = _u2 + _k;                    \
  _w2 = _t & MP_LOW_MASK;           \
  _w1 = _t >> MP_LOW_BITS;          \
                                    \
  _t = _u2 + _w2;                   \
  _k = _t >> MP_LOW_BITS;           \
                                    \
  (hi) = _u0 * _u0 + _w1 + _k;      \
  (lo) = (_t << MP_LOW_BITS) + _w3; \
} while (0)

#endif /* !MP_HAVE_WIDE */

/* [z, c] = x + y + c */
#define mp_add_1(z, c, x, y) do { \
  mp_limb_t _z = (y) + (c);       \
  mp_limb_t _c = (_z < (c));      \
  _z += (x);                      \
  _c += (_z < (x));               \
  (c) = _c;                       \
  (z) = _z;                       \
} while (0)

/* [z, c] = x - y - c = x - (y + c) */
#define mp_sub_1(z, c, x, y) do { \
  mp_limb_t _z = (y) + (c);       \
  mp_limb_t _c = (_z < (c));      \
  _z = (x) - _z;                  \
  _c += (_z > (x));               \
  (c) = _c;                       \
  (z) = _z;                       \
} while (0)

/* [z, c] = x * y + c */
#define mp_mul_1(z, c, x, y) do { \
  mp_limb_t _hi, _lo;             \
  mp_mul(_hi, _lo, x, y);         \
  _lo += (c);                     \
  _hi += (_lo < (c));             \
  (c) = _hi;                      \
  (z) = _lo;                      \
} while (0)

/* [z, c] = z + x * y + c */
#define mp_addmul_1(z, c, x, y) do { \
  mp_limb_t _hi, _lo;                \
  mp_mul(_hi, _lo, x, y);            \
  _lo += (c);                        \
  _hi += (_lo < (c));                \
  _lo += (z);                        \
  _hi += (_lo < (z));                \
  (c) = _hi;                         \
  (z) = _lo;                         \
} while (0)

/* [z, c] = z - x * y - c = z - (x * y + c) */
#define mp_submul_1(z, c, x, y) do { \
  mp_limb_t _hi, _lo;                \
  mp_mul(_hi, _lo, x, y);            \
  _lo += (c);                        \
  _hi += (_lo < (c));                \
  _lo = (z) - _lo;                   \
  _hi += (_lo > (z));                \
  (c) = _hi;                         \
  (z) = _lo;                         \
} while (0)

/* [z, c] = ~x + c */
#define mp_neg_1(z, c, x) do { \
  mp_limb_t _z = ~(x) + (c);   \
  (c) = (_z < (c));            \
  (z) = _z;                    \
} while (0)

#endif /* !MP_FAST_WIDE */

/*
 * MPN Macros
 */

#define MPN_SWAP(xp, xn, yp, yn) do { \
  mp_limb_t *_tp = (xp);              \
  mp_size_t _tn = (xn);               \
                                      \
  (xp) = (yp);                        \
  (xn) = (yn);                        \
  (yp) = _tp;                         \
  (yn) = _tn;                         \
} while (0)

#define MPN_SHIFT_ZEROES(z, xp, xn) do { \
  mp_bits_t _tz;                         \
                                         \
  ASSERT((xn) > 0);                      \
  ASSERT((xp)[(xn) - 1] != 0);           \
                                         \
  (z) = 0;                               \
                                         \
  while ((xp)[0] == 0) {                 \
    (z) += MP_LIMB_BITS;                 \
    (xp)++;                              \
    (xn)--;                              \
  }                                      \
                                         \
  _tz = mp_ctz((xp)[0]);                 \
                                         \
  if (_tz > 0) {                         \
    mpn_rshift(xp, xp, xn, _tz);         \
    (xn) -= ((xp)[(xn) - 1] == 0);       \
    (z) += _tz;                          \
  }                                      \
} while (0)

/*
 * MPZ Macros
 */

#define MPZ_CSWAP(x, xn, y, yn) do { \
  mpz_srcptr _t = (x);               \
  mp_size_t _tn = (xn);              \
                                     \
  (x) = (y);                         \
  (xn) = (yn);                       \
  (y) = _t;                          \
  (yn) = _tn;                        \
} while (0)

/*
 * Types
 */

typedef struct mp_divisor_s {
  mp_limb_t *up;
  mp_limb_t *vp;
  mp_limb_t inv;
  mp_bits_t shift;
  mp_size_t size;
  mp_limb_t tmp;
} mp_divisor_t;

/*
 * Prototypes
 */

static void
mpz_roset_n(mpz_ptr z, const mp_limb_t *xp, mp_size_t xs);

/*
 * Allocation
 */

static mp_limb_t *
mp_alloc_limbs(mp_size_t size) {
  mp_limb_t *ptr;

  CHECK(size > 0);

  ptr = (mp_limb_t *)malloc(size * sizeof(mp_limb_t));

  if (ptr == NULL)
    btc_abort(); /* LCOV_EXCL_LINE */

  return ptr;
}

static mp_limb_t *
mp_realloc_limbs(mp_limb_t *ptr, mp_size_t size) {
  CHECK(size > 0);

  ptr = (mp_limb_t *)realloc(ptr, size * sizeof(mp_limb_t));

  if (ptr == NULL)
    btc_abort(); /* LCOV_EXCL_LINE */

  return ptr;
}

static void
mp_free_limbs(mp_limb_t *ptr) {
  free(ptr);
}

static char *
mp_alloc_str(size_t size) {
  char *ptr;

  CHECK(size != 0);

  ptr = (char *)malloc(size);

  if (ptr == NULL)
    btc_abort(); /* LCOV_EXCL_LINE */

  return ptr;
}

static void
mp_free_str(char *ptr) {
  free(ptr);
}

/*
 * Helpers
 */

static BTC_INLINE mp_bits_t
mp_popcount(mp_limb_t x) {
#if defined(MP_HAVE_ASM_X64) && defined(__POPCNT__)
  /* Explicitly built with POPCNT support (-mpopcnt). */
  mp_limb_t z;

  __asm__ (
    "popcntq %q1, %q0\n"
    : "=r" (z)
    : "rm" (x)
    : "cc"
  );

  return z;
#elif defined(mp_builtin_popcount)
  return mp_builtin_popcount(x);
#else
  /* https://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel */
  /* https://en.wikipedia.org/wiki/Popcount#Efficient_implementation */
#if MP_LIMB_BITS == 64
  static const mp_limb_t a = MP_LIMB_C(0x5555555555555555);
  static const mp_limb_t b = MP_LIMB_C(0x3333333333333333);
  static const mp_limb_t c = MP_LIMB_C(0x0f0f0f0f0f0f0f0f);
  static const mp_limb_t d = MP_LIMB_C(0x0101010101010101);
#else
  static const mp_limb_t a = MP_LIMB_C(0x55555555);
  static const mp_limb_t b = MP_LIMB_C(0x33333333);
  static const mp_limb_t c = MP_LIMB_C(0x0f0f0f0f);
  static const mp_limb_t d = MP_LIMB_C(0x01010101);
#endif

  x = x - ((x >> 1) & a);
  x = (x & b) + ((x >> 2) & b);
  x = ((x + (x >> 4)) & c) * d;

  return x >> (MP_LIMB_BITS - 8);
#endif
}

static BTC_INLINE mp_bits_t
mp_clz(mp_limb_t x) {
#if defined(MP_HAVE_ASM_X64)
  mp_limb_t z;

  if (x == 0)
    return MP_LIMB_BITS;

  __asm__ (
    "bsrq %q1, %q0\n"
    : "=r" (z)
    : "rm" (x)
    : "cc"
  );

  return (MP_LIMB_BITS - 1) - z;
#elif defined(mp_builtin_clz)
  if (x == 0)
    return MP_LIMB_BITS;

  return mp_builtin_clz(x);
#elif defined(mp_intrin_bsr)
  unsigned long z;

  if (!mp_intrin_bsr(&z, x))
    return MP_LIMB_BITS;

  return (MP_LIMB_BITS - 1) - z;
#else
  /* http://aggregate.org/MAGIC/#Leading%20Zero%20Count */
  x |= (x >> 1);
  x |= (x >> 2);
  x |= (x >> 4);
  x |= (x >> 8);
  x |= (x >> 16);
#if MP_LIMB_BITS == 64
  x |= (x >> 32);
#endif
  return MP_LIMB_BITS - mp_popcount(x);
#endif
}

static BTC_INLINE mp_bits_t
mp_ctz(mp_limb_t x) {
#if defined(MP_HAVE_ASM_X64)
  mp_limb_t z;

  if (x == 0)
    return MP_LIMB_BITS;

  __asm__ (
    "bsfq %q1, %q0\n"
    : "=r" (z)
    : "rm" (x)
    : "cc"
  );

  return z;
#elif defined(mp_builtin_ctz)
  if (x == 0)
    return MP_LIMB_BITS;

  /* Fun fact: Here, gcc will emit a tzcnt
   * instruction on all x86 platforms.
   *
   * At first glance this seems like an
   * issue as tzcnt is only available on
   * Haswell and later. However, tzcnt's
   * encoding (f3 0f bc) is identical to
   * encoding rep (f3) and bsf (0f bc).
   *
   * The rep instruction is effectively
   * a no-op in this context, and as such
   * it is safe to encode tzcnt for older
   * microarchitectures.
   *
   * Note that clang always emits a bsf
   * instruction (unless -march=native is
   * passed).
   */
  return mp_builtin_ctz(x);
#elif defined(mp_intrin_bsf)
  unsigned long z;

  if (!mp_intrin_bsf(&z, x))
    return MP_LIMB_BITS;

  return z;
#else
  /* http://aggregate.org/MAGIC/#Trailing%20Zero%20Count */
  return mp_popcount((x & -x) - 1);
#endif
}

static BTC_INLINE mp_bits_t
mp_bitlen(mp_limb_t x) {
#if defined(MP_HAVE_ASM_X64)
  mp_limb_t z;

  if (x == 0)
    return 0;

  __asm__ (
    "bsrq %q1, %q0\n"
    : "=r" (z)
    : "rm" (x)
    : "cc"
  );

  return z + 1;
#else
  return MP_LIMB_BITS - mp_clz(x);
#endif
}

static BTC_INLINE int
mp_mul_gt_2(mp_limb_t u, mp_limb_t v, mp_limb_t y1, mp_limb_t y0) {
  mp_limb_t x1, x0;

  mp_mul(x1, x0, u, v);

  return x1 > y1 || (x1 == y1 && x0 > y0);
}

static BTC_INLINE mp_size_t
mp_size_cast(size_t n) {
  CHECK(n <= (size_t)MP_SIZE_MAX);
  return n;
}

static BTC_INLINE mp_limb_t
mp_limb_cast(mp_long_t x) {
  if (UNLIKELY(x == MP_LONG_MIN))
    return MP_LIMB_HI;

  return MP_ABS(x);
}

static BTC_INLINE mp_long_t
mp_long_cast(mp_limb_t x, mp_size_t sign) {
  if (sign < 0) {
    if (UNLIKELY(x == MP_LIMB_HI))
      return MP_LONG_MIN;

    return -(mp_long_t)(x & (MP_LIMB_HI - 1));
  }

  return x & (MP_LIMB_HI - 1);
}

static BTC_INLINE int
mp_isspace(int ch) {
  /* '\t', '\n', '\v', '\f', '\r', ' ' */
  static const char spaces[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 1, 0
  };
  return spaces[ch & 0xff];
}

static mp_size_t
mp_str_limbs(const char *str, int base) {
  mp_limb_t max, limb_pow;
  mp_size_t limb_len;
  mp_size_t len = 0;

  while (*str)
    len += !mp_isspace(*str++);

  if (len == 0)
    len = 1;

  if (base < 2)
    base = 2;
  else if (base > 62)
    base = 62;

  if ((base & (base - 1)) == 0)
    return (len * mp_bitlen(base - 1) + MP_LIMB_BITS - 1) / MP_LIMB_BITS;

  max = MP_LIMB_MAX / base;
  limb_pow = base;
  limb_len = 1;

  while (limb_pow <= max) {
    limb_pow *= base;
    limb_len += 1;
  }

  return (len + limb_len - 1) / limb_len;
}

static int
mp_host_endian(void) {
  static const mp_limb_t x = 1;
  return *((const unsigned char *)&x) == 0 ? 1 : -1;
}

static BTC_INLINE mp_limb_t
mp_import_le(const unsigned char *xp) {
#if MP_LIMB_BITS == 64
  return ((mp_limb_t)xp[7] << 56)
       | ((mp_limb_t)xp[6] << 48)
       | ((mp_limb_t)xp[5] << 40)
       | ((mp_limb_t)xp[4] << 32)
       | ((mp_limb_t)xp[3] << 24)
       | ((mp_limb_t)xp[2] << 16)
       | ((mp_limb_t)xp[1] <<  8)
       | ((mp_limb_t)xp[0] <<  0);
#else
  return ((mp_limb_t)xp[3] << 24)
       | ((mp_limb_t)xp[2] << 16)
       | ((mp_limb_t)xp[1] <<  8)
       | ((mp_limb_t)xp[0] <<  0);
#endif
}

static BTC_INLINE mp_limb_t
mp_import_be(const unsigned char *xp) {
#if MP_LIMB_BITS == 64
  return ((mp_limb_t)xp[0] << 56)
       | ((mp_limb_t)xp[1] << 48)
       | ((mp_limb_t)xp[2] << 40)
       | ((mp_limb_t)xp[3] << 32)
       | ((mp_limb_t)xp[4] << 24)
       | ((mp_limb_t)xp[5] << 16)
       | ((mp_limb_t)xp[6] <<  8)
       | ((mp_limb_t)xp[7] <<  0);
#else
  return ((mp_limb_t)xp[0] << 24)
       | ((mp_limb_t)xp[1] << 16)
       | ((mp_limb_t)xp[2] <<  8)
       | ((mp_limb_t)xp[3] <<  0);
#endif
}

static BTC_INLINE void
mp_export_le(unsigned char *zp, mp_limb_t x) {
#if MP_LIMB_BITS == 64
  zp[0] = (x >>  0) & 0xff;
  zp[1] = (x >>  8) & 0xff;
  zp[2] = (x >> 16) & 0xff;
  zp[3] = (x >> 24) & 0xff;
  zp[4] = (x >> 32) & 0xff;
  zp[5] = (x >> 40) & 0xff;
  zp[6] = (x >> 48) & 0xff;
  zp[7] = (x >> 56) & 0xff;
#else
  zp[0] = (x >>  0) & 0xff;
  zp[1] = (x >>  8) & 0xff;
  zp[2] = (x >> 16) & 0xff;
  zp[3] = (x >> 24) & 0xff;
#endif
}

static BTC_INLINE void
mp_export_be(unsigned char *zp, mp_limb_t x) {
#if MP_LIMB_BITS == 64
  zp[7] = (x >>  0) & 0xff;
  zp[6] = (x >>  8) & 0xff;
  zp[5] = (x >> 16) & 0xff;
  zp[4] = (x >> 24) & 0xff;
  zp[3] = (x >> 32) & 0xff;
  zp[2] = (x >> 40) & 0xff;
  zp[1] = (x >> 48) & 0xff;
  zp[0] = (x >> 56) & 0xff;
#else
  zp[3] = (x >>  0) & 0xff;
  zp[2] = (x >>  8) & 0xff;
  zp[1] = (x >> 16) & 0xff;
  zp[0] = (x >> 24) & 0xff;
#endif
}

/*
 * Globals
 */

const int mp_bits_per_limb = MP_LIMB_BITS;

/*
 * MPN Interface
 */

/*
 * Initialization
 */

void
mpn_zero(mp_limb_t *zp, mp_size_t zn) {
  mp_size_t i;

  for (i = 0; i < zn; i++)
    zp[i] = 0;
}

/*
 * Uninitialization
 */

#ifdef __cplusplus
extern "C"
#endif
void
btc_memzero(void *, size_t);

void
mpn_cleanse(mp_limb_t *zp, mp_size_t zn) {
  btc_memzero(zp, zn * sizeof(mp_limb_t));
}

/*
 * Internal
 */

static BTC_INLINE mp_size_t
mpn_strip(const mp_limb_t *xp, mp_size_t xn) {
  while (xn > 0 && xp[xn - 1] == 0)
    xn -= 1;

  return xn;
}

/*
 * Assignment
 */

void
mpn_set_1(mp_limb_t *zp, mp_size_t zn, mp_limb_t x) {
  ASSERT(zn > 0);

  zp[0] = x;

  mpn_zero(zp + 1, zn - 1);
}

void
mpn_copyi(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn) {
  mp_size_t i;

  for (i = 0; i < xn; i++)
    zp[i] = xp[i];
}

void
mpn_copyd(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn) {
  mp_size_t i;

  for (i = xn - 1; i >= 0; i--)
    zp[i] = xp[i];
}

/*
 * Comparison
 */

int
mpn_zero_p(const mp_limb_t *xp, mp_size_t xn) {
  mp_size_t i;

  for (i = 0; i < xn; i++) {
    if (xp[i] != 0)
      return 0;
  }

  return 1;
}

int
mpn_cmp(const mp_limb_t *xp, const mp_limb_t *yp, mp_size_t n) {
  mp_size_t i;

  for (i = n - 1; i >= 0; i--) {
    if (xp[i] != yp[i])
      return xp[i] < yp[i] ? -1 : 1;
  }

  return 0;
}

static BTC_INLINE int
mpn_cmp2(const mp_limb_t *xp, mp_size_t xn,
         const mp_limb_t *yp, mp_size_t yn) {
  if (xn != yn)
    return xn < yn ? -1 : 1;

  return mpn_cmp(xp, yp, xn);
}

static BTC_INLINE int
mpn_cmp_1(const mp_limb_t *xp, mp_size_t xn, mp_limb_t y) {
  mp_limb_t x;

  if (xn > 1)
    return 1;

  x = xn > 0 ? xp[0] : 0;

  if (x != y)
    return x < y ? -1 : 1;

  return 0;
}

/*
 * Addition
 */

mp_limb_t
mpn_add_1(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_limb_t y) {
  mp_limb_t c = y;
  mp_limb_t x, z;
  mp_size_t i;

  for (i = 0; i < xn && c != 0; i++) {
    /* [z, c] = x + c */
    x = xp[i];
    z = x + c;
    c = (z < c);
    zp[i] = z;
  }

  if (zp != xp) {
    for (; i < xn; i++)
      zp[i] = xp[i];
  }

  return c;
}

mp_limb_t
mpn_add_n(mp_limb_t *zp, const mp_limb_t *xp,
                         const mp_limb_t *yp,
                         mp_size_t n) {
  mp_limb_t c = 0;

  switch (n & 3) {
    case 3:
      mp_add_1(*zp, c, *xp, *yp); zp++; xp++; yp++;
    case 2:
      mp_add_1(*zp, c, *xp, *yp); zp++; xp++; yp++;
    case 1:
      mp_add_1(*zp, c, *xp, *yp); zp++; xp++; yp++;
  }

  n >>= 2;

  while (n--) {
    /* [z, c] = x + y + c */
#if defined(mp_add_1x4)
    mp_add_1x4(zp, c, xp, yp);
#else
    mp_add_1(zp[0], c, xp[0], yp[0]);
    mp_add_1(zp[1], c, xp[1], yp[1]);
    mp_add_1(zp[2], c, xp[2], yp[2]);
    mp_add_1(zp[3], c, xp[3], yp[3]);
#endif

    zp += 4;
    xp += 4;
    yp += 4;
  }

  return c;
}

mp_limb_t
mpn_add(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                       const mp_limb_t *yp, mp_size_t yn) {
  mp_limb_t c;

  CHECK(xn >= yn);

  c = mpn_add_n(zp, xp, yp, yn);

  if (xn > yn)
    c = mpn_add_1(zp + yn, xp + yn, xn - yn, c);

  return c;
}

/*
 * Secure Addition
 */

mp_limb_t
mpn_sec_add_1(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_limb_t y) {
  mp_limb_t c;

  if (UNLIKELY(xn == 0))
    return y;

  mp_add(*zp, c, *xp, y); zp++; xp++; xn--;

  switch (xn & 3) {
    case 3:
      mp_add(*zp, c, *xp, c); zp++; xp++;
    case 2:
      mp_add(*zp, c, *xp, c); zp++; xp++;
    case 1:
      mp_add(*zp, c, *xp, c); zp++; xp++;
  }

  xn >>= 2;

  while (xn--) {
    /* [z, c] = x + c */
#if defined(mp_add_x4)
    mp_add_x4(zp, c, xp);
#else
    mp_add(zp[0], c, xp[0], c);
    mp_add(zp[1], c, xp[1], c);
    mp_add(zp[2], c, xp[2], c);
    mp_add(zp[3], c, xp[3], c);
#endif

    zp += 4;
    xp += 4;
  }

  return c;
}

mp_limb_t
mpn_sec_add(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                           const mp_limb_t *yp, mp_size_t yn) {
  mp_limb_t c;

  CHECK(xn >= yn);

  c = mpn_add_n(zp, xp, yp, yn);

  if (xn > yn)
    c = mpn_sec_add_1(zp + yn, xp + yn, xn - yn, c);

  return c;
}

/*
 * Subtraction
 */

mp_limb_t
mpn_sub_1(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_limb_t y) {
  mp_limb_t c = y;
  mp_limb_t x, z;
  mp_size_t i;

  for (i = 0; i < xn && c != 0; i++) {
    /* [z, c] = x - c */
    x = xp[i];
    z = x - c;
    c = (z > x);
    zp[i] = z;
  }

  if (zp != xp) {
    for (; i < xn; i++)
      zp[i] = xp[i];
  }

  return c;
}

mp_limb_t
mpn_sub_n(mp_limb_t *zp, const mp_limb_t *xp,
                         const mp_limb_t *yp,
                         mp_size_t n) {
  mp_limb_t c = 0;

  switch (n & 3) {
    case 3:
      mp_sub_1(*zp, c, *xp, *yp); zp++; xp++; yp++;
    case 2:
      mp_sub_1(*zp, c, *xp, *yp); zp++; xp++; yp++;
    case 1:
      mp_sub_1(*zp, c, *xp, *yp); zp++; xp++; yp++;
  }

  n >>= 2;

  while (n--) {
    /* [z, c] = x - y - c = x - (y + c) */
#if defined(mp_sub_1x4)
    mp_sub_1x4(zp, c, xp, yp);
#else
    mp_sub_1(zp[0], c, xp[0], yp[0]);
    mp_sub_1(zp[1], c, xp[1], yp[1]);
    mp_sub_1(zp[2], c, xp[2], yp[2]);
    mp_sub_1(zp[3], c, xp[3], yp[3]);
#endif

    zp += 4;
    xp += 4;
    yp += 4;
  }

  return c;
}

mp_limb_t
mpn_sub(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                       const mp_limb_t *yp, mp_size_t yn) {
  mp_limb_t c;

  CHECK(xn >= yn);

  c = mpn_sub_n(zp, xp, yp, yn);

  if (xn > yn)
    c = mpn_sub_1(zp + yn, xp + yn, xn - yn, c);

  return c;
}

static void
mpn_sub_mod(mp_limb_t *zp, const mp_limb_t *xp,
                           const mp_limb_t *yp,
                           const mp_limb_t *mp,
                           mp_size_t mn) {
  if (mpn_cmp(xp, yp, mn) >= 0) {
    /* z = x - y */
    mpn_sub_n(zp, xp, yp, mn);
  } else {
    /* z = m - (y - x) */
    mpn_sub_n(zp, yp, xp, mn);
    mpn_sub_n(zp, mp, zp, mn);
  }
}

/*
 * Secure Subtraction
 */

mp_limb_t
mpn_sec_sub_1(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_limb_t y) {
  mp_limb_t c;

  if (UNLIKELY(xn == 0))
    return y;

  mp_sub(*zp, c, *xp, y); zp++; xp++; xn--;

  switch (xn & 3) {
    case 3:
      mp_sub(*zp, c, *xp, c); zp++; xp++;
    case 2:
      mp_sub(*zp, c, *xp, c); zp++; xp++;
    case 1:
      mp_sub(*zp, c, *xp, c); zp++; xp++;
  }

  xn >>= 2;

  while (xn--) {
    /* [z, c] = x - c */
#if defined(mp_sub_x4)
    mp_sub_x4(zp, c, xp);
#else
    mp_sub(zp[0], c, xp[0], c);
    mp_sub(zp[1], c, xp[1], c);
    mp_sub(zp[2], c, xp[2], c);
    mp_sub(zp[3], c, xp[3], c);
#endif

    zp += 4;
    xp += 4;
  }

  return c;
}

mp_limb_t
mpn_sec_sub(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                           const mp_limb_t *yp, mp_size_t yn) {
  mp_limb_t c;

  CHECK(xn >= yn);

  c = mpn_sub_n(zp, xp, yp, yn);

  if (xn > yn)
    c = mpn_sec_sub_1(zp + yn, xp + yn, xn - yn, c);

  return c;
}

/*
 * Multiplication
 */

mp_limb_t
mpn_mul_1(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_limb_t y) {
  mp_limb_t c = 0;

  switch (xn & 3) {
    case 3:
      mp_mul_1(*zp, c, *xp, y); zp++; xp++;
    case 2:
      mp_mul_1(*zp, c, *xp, y); zp++; xp++;
    case 1:
      mp_mul_1(*zp, c, *xp, y); zp++; xp++;
  }

  xn >>= 2;

  while (xn--) {
    /* [z, c] = x * y + c */
    mp_mul_1(zp[0], c, xp[0], y);
    mp_mul_1(zp[1], c, xp[1], y);
    mp_mul_1(zp[2], c, xp[2], y);
    mp_mul_1(zp[3], c, xp[3], y);

    zp += 4;
    xp += 4;
  }

  return c;
}

mp_limb_t
mpn_addmul_1(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_limb_t y) {
  mp_limb_t c = 0;

  switch (xn & 3) {
    case 3:
      mp_addmul_1(*zp, c, *xp, y); zp++; xp++;
    case 2:
      mp_addmul_1(*zp, c, *xp, y); zp++; xp++;
    case 1:
      mp_addmul_1(*zp, c, *xp, y); zp++; xp++;
  }

  xn >>= 2;

  while (xn--) {
    /* [z, c] = z + x * y + c */
    mp_addmul_1(zp[0], c, xp[0], y);
    mp_addmul_1(zp[1], c, xp[1], y);
    mp_addmul_1(zp[2], c, xp[2], y);
    mp_addmul_1(zp[3], c, xp[3], y);

    zp += 4;
    xp += 4;
  }

  return c;
}

mp_limb_t
mpn_submul_1(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_limb_t y) {
  mp_limb_t c = 0;

  switch (xn & 3) {
    case 3:
      mp_submul_1(*zp, c, *xp, y); zp++; xp++;
    case 2:
      mp_submul_1(*zp, c, *xp, y); zp++; xp++;
    case 1:
      mp_submul_1(*zp, c, *xp, y); zp++; xp++;
  }

  xn >>= 2;

  while (xn--) {
    /* [z, c] = z - x * y - c = z - (x * y + c) */
    mp_submul_1(zp[0], c, xp[0], y);
    mp_submul_1(zp[1], c, xp[1], y);
    mp_submul_1(zp[2], c, xp[2], y);
    mp_submul_1(zp[3], c, xp[3], y);

    zp += 4;
    xp += 4;
  }

  return c;
}

void
mpn_mul_n(mp_limb_t *zp, const mp_limb_t *xp,
                         const mp_limb_t *yp,
                         mp_size_t n) {
  mpn_mul(zp, xp, n, yp, n);
}

void
mpn_mul(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                       const mp_limb_t *yp, mp_size_t yn) {
  mp_size_t i;

  if (UNLIKELY(yn == 0)) {
    mpn_zero(zp, xn);
    return;
  }

  zp[xn] = mpn_mul_1(zp, xp, xn, yp[0]);

  for (i = 1; i < yn; i++)
    zp[xn + i] = mpn_addmul_1(zp + i, xp, xn, yp[i]);
}

void
mpn_sqr(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_limb_t *scratch) {
  /* `2 * xn` limbs are required for scratch. */
  mp_limb_t *tp = scratch;
  mp_size_t i;

  if (UNLIKELY(xn == 0))
    return;

  mp_sqr(zp[1], zp[0], xp[0]);

  if (xn == 1)
    return;

  tp[0] = 0;

  mp_sqr(zp[3], zp[2], xp[1]);
  mp_mul(tp[2], tp[1], xp[0], xp[1]);

  for (i = 2; i < xn; i++) {
    mp_sqr(zp[2 * i + 1], zp[2 * i + 0], xp[i]);

    tp[2 * i - 1] = 0;
    tp[2 * i - 0] = mpn_addmul_1(tp + i, xp, i, xp[i]);
  }

  tp[2 * xn - 1] = mpn_lshift(tp + 1, tp + 1, 2 * xn - 2, 1);

  ASSERT(mpn_add_n(zp, zp, tp, 2 * xn) == 0);
}

/*
 * Multiply + Shift
 */

mp_limb_t
mpn_mulshift(mp_limb_t *zp, const mp_limb_t *xp,
                            const mp_limb_t *yp,
                            mp_size_t n,
                            mp_bits_t bits,
                            mp_limb_t *scratch) {
  /* Compute `z = round((x * y) / 2^bits)`.
   *
   * Constant time assuming `bits` is constant.
   *
   * `2 * n` limbs are required for scratch.
   */
  mp_size_t s = bits / MP_LIMB_BITS;
  mp_bits_t r = bits % MP_LIMB_BITS;
  mp_limb_t *tp = scratch;
  mp_size_t tn = n * 2;
  mp_size_t zn = tn - s;
  mp_limb_t b;

  /* Ensure L <= bits < 2 * L. */
  ASSERT(s >= n && s < n * 2);
  ASSERT(zn > 0 && zn <= n);

  /* t = x * y */
  mpn_mul_n(tp, xp, yp, n);

  /* b = (t >> (bits - 1)) & 1 */
  b = mpn_tstbit(tp, bits - 1);

  /* z = t >> bits */
  if (r != 0)
    mpn_rshift(zp, tp + s, zn, r);
  else
    mpn_copyi(zp, tp + s, zn);

  mpn_zero(zp + zn, n - zn);

  /* z += b */
  return mpn_sec_add_1(zp, zp, n, b);
}

/*
 * Division Helpers
 */

static void MP_MSVC_CDECL
mp_div(mp_limb_t *q, mp_limb_t *r,
       mp_limb_t n1, mp_limb_t n0,
       mp_limb_t d) {
#if defined(MP_HAVE_ASM_X64)
  __asm__ (
    "divq %q4\n"
    : "=a" (*q), "=d" (*r)
    : "0" (n0), "1" (n1), "rm" (d)
    : "cc"
  );
#elif defined(MP_HAVE_ASM_X86)
  __asm__ (
    "divl %k4\n"
    : "=a" (*q), "=d" (*r)
    : "0" (n0), "1" (n1), "rm" (d)
    : "cc"
  );
#elif defined(MP_HAVE_UDIV128)
  *q = _udiv128(n1, n0, d, r);
#elif defined(MP_HAVE_UDIV64)
  mp_wide_t n = ((mp_wide_t)n1 << MP_LIMB_BITS) | n0;

  *q = _udiv64(n, d, (unsigned int *)r);
#elif defined(MP_MSVC_ASM_X64)
  /* Utilize a clever hack[1][2] from fahickman/r128[3] in
   * order to implement the _udiv128 and _udiv64 intrinsics
   * on older versions of MSVC, keeping in mind windows'
   * x64 __fastcall calling convention[4]. We modify the
   * x64 assembly here to replicate the _udiv128 argument
   * order.
   *
   * [1] https://github.com/fahickman/r128/blob/cf2e88f/r128.h#L798
   * [2] https://github.com/fahickman/r128/blob/cf2e88f/r128.h#L683
   * [3] https://github.com/fahickman/r128
   * [4] https://docs.microsoft.com/en-us/cpp/build/x64-calling-convention
   */
  typedef mp_limb_t udiv128_f(mp_limb_t n1, mp_limb_t n0,
                              mp_limb_t d, mp_limb_t *r);

  static MP_MSVC_CODE const unsigned char udiv128_code[] = {
    /* %rcx = n1, %rdx = n0, %r8 = d, %r9 = *r */
    0x48, 0x89, 0xd0, /* movq %rdx, %rax */
    0x48, 0x89, 0xca, /* movq %rcx, %rdx */
    0x49, 0xf7, 0xf0, /* divq %r8 */
    0x49, 0x89, 0x11, /* movq %rdx, (%r9) */
    0xc3              /* retq */
  };

  *q = ((udiv128_f *)udiv128_code)(n1, n0, d, r);
#elif defined(MP_MSVC_ASM_X86)
  mp_limb_t q0, r0;

  _asm {
    mov eax, n0
    mov edx, n1
    div d
    mov q0, eax
    mov r0, edx
  }

  *q = q0;
  *r = r0;
#else
  /* Code adapted from the `divlu2` function
   * in Hacker's Delight[1].
   *
   * Having this here allows us to avoid linking
   * to libgcc or libgcc-like libs on platforms
   * which don't support a wide division.
   *
   * [1] https://gist.github.com/chjj/d59b19c32b2ccbb1a7b397cd77cc7025
   */
  static const mp_limb_t b = MP_LIMB_C(1) << MP_LOW_BITS;
  mp_limb_t un1, un0, vn1, vn0, q1, q0;
  mp_limb_t un32, un21, un10, rhat;
  mp_bits_t s;

  if (n1 == 0) {
    *q = n0 / d;
    *r = n0 - (*q) * d;
    return;
  }

  if (n1 >= d)
    btc_abort(); /* LCOV_EXCL_LINE */

  /* Normalize divisor. */
  s = mp_clz(d);
  d <<= s;

  /* Break divisor up into two half-limb digits. */
  vn1 = d >> MP_LOW_BITS;
  vn0 = d & MP_LOW_MASK;

  /* Shift dividend left. */
  if (s != 0) {
    un32 = (n1 << s) | (n0 >> (MP_LIMB_BITS - s));
    un10 = n0 << s;
  } else {
    un32 = n1;
    un10 = n0;
  }

  /* Break right half of dividend into two digits. */
  un1 = un10 >> MP_LOW_BITS;
  un0 = un10 & MP_LOW_MASK;

  /* Compute the first quotient digit, q1. */
  q1 = un32 / vn1;
  rhat = un32 - q1 * vn1;

  while (q1 >= b || q1 * vn0 > rhat * b + un1) {
    q1 -= 1;
    rhat += vn1;

    if (rhat >= b)
      break;
  }

  /* Multiply and subtract. */
  un21 = un32 * b + un1 - q1 * d;

  /* Compute the second quotient digit, q0. */
  q0 = un21 / vn1;
  rhat = un21 - q0 * vn1;

  while (q0 >= b || q0 * vn0 > rhat * b + un0) {
    q0 -= 1;
    rhat += vn1;

    if (rhat >= b)
      break;
  }

  *q = q1 * b + q0;
  *r = (un21 * b + un0 - q0 * d) >> s;
#endif
}

static mp_limb_t
mp_inv_2by1(mp_limb_t d) {
  /* [DIV] Page 2, Section III.
   *
   * The approximate reciprocal is defined as:
   *
   *   v = ((B^2 - 1) / d) - B
   *
   * Unfortunately, the numerator here is too
   * large for hardware instructions.
   *
   * Instead, we can compute:
   *
   *   v = (B^2 - 1 - d * B) / d
   *
   * Which happens to be equivalent and allows
   * us to do a normalized division using
   * hardware instructions.
   *
   * Or, as described in the paper:
   *
   *   v = (B - 1 - d, B - 1) / d
   *
   * A more programmatic way of expressing
   * this would be (where L = log2(B)):
   *
   *   v = ~(d << L) / d
   *
   * Or, in x86-64 assembly:
   *
   *   mov $0, %rax
   *   mov %[d], %rdx
   *   not %rax
   *   not %rdx
   *   div %[d]
   *   mov %rax, %[v]
   *
   * This trick was utilized by the golang
   * developers when switching away from a
   * more specialized inverse function. See
   * the discussion here[1][2]. As an aside,
   * they seem to think (incorrectly?) that
   * the proof presented for the 2-by-1
   * division in [DIV] is flawed.
   *
   * Note that `d` must be normalized.
   *
   * [1] https://go-review.googlesource.com/c/go/+/250417
   * [2] https://go-review.googlesource.com/c/go/+/250417/comment/380e8f18_ad97735c/
   */
  mp_limb_t q, r;

  ASSERT(d >= MP_LIMB_HI);

  mp_div(&q, &r, ~d, MP_LIMB_MAX, d);

  return q;
}

static BTC_INLINE void
mp_div_2by1(mp_limb_t *q, mp_limb_t *r,
            mp_limb_t u1, mp_limb_t u0,
            mp_limb_t d, mp_limb_t v) {
  /* [DIV] Algorithm 4, Page 4, Section A.
   *
   * The 2-by-1 division is defined by
   * Möller & Granlund as:
   *
   *   (q1, q0) <- v * u1
   *   (q1, q0) <- (q1, q0) + (u1, u0)
   *
   *   q1 <- (q1 + 1) mod B
   *
   *   r <- (u0 - q1 * d) mod B
   *
   *   if r > q0 (unpredictable)
   *     q1 <- (q1 - 1) mod B
   *     r <- (r + d) mod B
   *
   *   if r >= d (unlikely)
   *     q1 <- q1 + 1
   *     r <- r - d
   *
   *   return q1, r
   *
   * Note that this function expects the
   * divisor to be normalized and does not
   * de-normalize the remainder.
   */
#if defined(MP_HAVE_ASM_X64) && defined(MP_USE_DIV_2BY1_ASM)
  /* Register Layout:
   *
   *   %q0 = q1 (*q)
   *   %q1 = r0 (*r)
   *   %q2 = u1
   *   %q3 = u0
   *   %q4 = d
   *   %q5 = v
   *   %rax = q1t (after step 3)
   *   %rdx = r0t (after step 3)
   *   %r8 = q0
   */
  __asm__ __volatile__ (
    /* (q1, q0) = v * u1 + (u1, u0) */
    "movq %q2, %%rax\n"   /* rax = u1 */
    "mulq %q5\n"          /* (rdx, rax) = v * rax */
    "movq %%rax, %%r8\n"  /* q0 = rax */
    "movq %%rdx, %q0\n"   /* q1 = rdx */
    "addq %q3, %%r8\n"    /* q0 += u0 */
    "adcq %q2, %q0\n"     /* q1 += u1 + cf */

    /* q1 += 1 */
    "addq $1, %q0\n"      /* q1 += 1 */

    /* r0 = u0 - q1 * d */
    "movq %q0, %%rax\n"   /* rax = q1 */
    "imulq %q4, %%rax\n"  /* rax = d * rax */
    "movq %q3, %q1\n"     /* r0 = u0 */
    "subq %%rax, %q1\n"   /* r0 -= rax */

    /* q1 -= 1 if r0 > q0 */
    /* r0 += d if r0 > q0 */
    "movq %q0, %%rax\n"   /* q1t = q1 */
    "movq %q1, %%rdx\n"   /* r0t = r0 */
    "subq $1, %%rax\n"    /* q1t -= 1 */
    "addq %q4, %%rdx\n"   /* r0t += d */
    "cmpq %%r8, %q1\n"    /* cmp(r0, q0) */
    "cmovaq %%rax, %q0\n" /* q1 = q1t if r0 > q0 */
    "cmovaq %%rdx, %q1\n" /* r0 = r1t if r0 > q0 */

    /* q1 += 1 if r0 >= d */
    /* r0 -= d if r0 >= d */
    "cmpq %q4, %q1\n"     /* cmp(r0, d) */
    "jb 1f\n"             /* skip if r0 < d */
    "addq $1, %q0\n"      /* q1 += 1 if r0 >= d */
    "subq %q4, %q1\n"     /* r0 -= d if r0 >= d */
    "1:\n"
    : "=&r" (*q), "=&r" (*r)
    : "rm" (u1), "rm" (u0),
      "rm" (d), "rm" (v)
    : "cc", "rax", "rdx", "r8"
  );
#else /* !MP_HAVE_ASM_X64 */
  mp_limb_t q0, q1, r0;

  mp_mul(q1, q0, v, u1);

  q0 += u0;
  q1 += u1 + (q0 < u0);

  /* At this point, we have computed:
   *
   *   q = (((B^2 - 1) / d) - B) * (u / B) + u
   *     = ((B^2 - 1) * u) / (B * d)
   *
   * On an 8-bit machine, this implies:
   *
   *   q = (u * 0xffff) / (d << 8)
   *
   * For example, if we want to compute:
   *
   *   [q, r] = 0x421 / 0x83 = [0x08, 0x09]
   *
   * We first compute:
   *
   *   q = 0x420fbdf / 0x8300 = 0x0811
   *
   * Note that the actual quotient is
   * in the high bits of the result.
   *
   * Our remainder is trickier. We now
   * compute:
   *
   *   r = u0 - (q1 + 1) * d
   *     = 0x21 - 0x09 * 0x83
   *     = -0x047a (allowed to underflow)
   *     = 0x86 mod B
   *
   * Since 0x86 > 0x11, the first branch
   * is triggered, computing:
   *
   *   r = r + d
   *     = 0x86 + 0x83
   *     = 0x09 mod B
   */
  q1 += 1;

  r0 = u0 - q1 * d;

  if (UNPREDICTABLE(r0 > q0)) {
    q1 -= 1;
    r0 += d;
  }

  if (UNLIKELY(r0 >= d)) {
    q1 += 1;
    r0 -= d;
  }

  *q = q1;
  *r = r0;
#endif /* !MP_HAVE_ASM_X64 */
}

BTC_UNUSED static mp_limb_t
mp_inv_3by2(mp_limb_t d1, mp_limb_t d0) {
  /* [DIV] Algorithm 6, Page 6, Section A.
   *
   * The approximate reciprocal is defined as:
   *
   *   v = ((B^3 - 1) / (d1, d0)) - B
   *
   * According to Möller & Granlund, this can
   * be implemented as follows:
   *
   *   v <- reciprocal_word(d1)
   *   p <- d1 * v mod B
   *   p <- (p + d0) mod B
   *
   *   if p < d0
   *     v <- v - 1
   *     if p >= d1
   *       v <- v - 1
   *       p <- p - d1
   *     p <- (p - d1) mod B
   *
   *   (t1, t0) <- v * d0
   *
   *   p <- (p + t1) mod B
   *
   *   if p < t1
   *     v <- v - 1
   *     if (p, t0) >= (d1, d0)
   *       v <- v - 1
   *
   *   return v
   *
   * Note that (d1, d0) must be normalized.
   */
  mp_limb_t v = mp_inv_2by1(d1);
  mp_limb_t p = d1 * v;
  mp_limb_t t1, t0;

  p += d0;

  if (p < d0) {
    v -= 1;

    if (p >= d1) {
      v -= 1;
      p -= d1;
    }

    p -= d1;
  }

  mp_mul(t1, t0, v, d0);

  p += t1;

  if (p < t1) {
    v -= 1;

    if (p > d1 || (p == d1 && t0 >= d0))
      v -= 1;
  }

  return v;
}

BTC_UNUSED static BTC_INLINE void
mp_div_3by2(mp_limb_t *q, mp_limb_t *k1, mp_limb_t *k0,
            mp_limb_t u2, mp_limb_t u1, mp_limb_t u0,
            mp_limb_t d1, mp_limb_t d0, mp_limb_t v) {
  /* [DIV] Algorithm 5, Page 5, Section IV.
   *
   * The 3-by-2 division is defined by
   * Möller & Granlund as:
   *
   *   (q1, q0) <- v * u2
   *   (q1, q0) <- (q1, q0) + (u2, u1)
   *
   *   r1 <- (u1 - q1 * d1) mod B
   *
   *   (t1, t0) <- d0 * q1
   *   (r1, r0) <- ((r1, u0) - (t1, t0) - (d1, d0)) mod B^2
   *
   *   q1 <- (q1 + 1) mod B
   *
   *   if r1 >= q0 (unpredictable)
   *     q1 <- (q1 - 1) mod B
   *     (r1, r0) <- ((r1, r0) + (d1, d0)) mod B^2
   *
   *   if (r1, r0) >= (d1, d0) (unlikely)
   *     q1 <- q1 + 1
   *     (r1, r0) <- (r1, r0) - (d1, d0)
   *
   *   return q1, (r1, r0)
   *
   * Note that this function expects the
   * divisor to be normalized and does not
   * de-normalize the remainder.
   */
#if defined(MP_HAVE_ASM_X64) && defined(MP_USE_DIV_3BY2_ASM)
  /* Register Layout:
   *
   *   %q0 = q1 (*q)
   *   %q1 = r1 (*k1)
   *   %q2 = r0 (*k0)
   *   %q3 = u2
   *   %q4 = u1
   *   %q5 = u0
   *   %q6 = d1
   *   %q7 = d0
   *   %q8 = v
   *   %rax = t0 (after step 3)
   *   %rdx = t1 (after step 3)
   *   %rax = q1t (after step 6)
   *   %rdx = r0t (after step 6)
   *   %r8 = q0
   *   %r9 = r1t (after step 6)
   */
  __asm__ __volatile__ (
    /* (q1, q0) = v * u2 + (u2, u1) */
    "movq %q3, %%rax\n"    /* rax = u2 */
    "mulq %q8\n"           /* (rdx, rax) = v * rax */
    "movq %%rax, %%r8\n"   /* q0 = rax */
    "movq %%rdx, %q0\n"    /* q1 = rdx */
    "addq %q4, %%r8\n"     /* q0 += u1 */
    "adcq %q3, %q0\n"      /* q1 += u2 + cf */

    /* r1 = u1 - d1 * q1 */
    "movq %q0, %%rax\n"    /* rax = q1 */
    "imulq %q6, %%rax\n"   /* rax = d1 * rax */
    "movq %q4, %q1\n"      /* r1 = u1 */
    "subq %%rax, %q1\n"    /* r1 -= rax */

    /* (t1, t0) = d0 * q1 */
    "movq %q0, %%rax\n"    /* rax = q1 */
    "mulq %q7\n"           /* (rax, rdx) = d0 * rax */
                           /* (t1, t0) = (rdx, rax) */

    /* (r1, r0) = (r1, u0) - ((t1, t0) + (d1, d0)) */
    "movq %q5, %q2\n"      /* r0 = u0 */
    "subq %%rax, %q2\n"    /* r0 -= t0 */
    "sbbq %%rdx, %q1\n"    /* r1 -= t1 + cf */
    "subq %q7, %q2\n"      /* r0 -= d0 */
    "sbbq %q6, %q1\n"      /* r1 -= d1 + cf */

    /* q1 += 1 */
    "addq $1, %q0\n" /* q1 += 1 */

    /* q1 -= 1 if r1 >= q0 */
    /* (r1, r0) += (d1, d0) if r1 >= q0 */
    "movq %q0, %%rax\n"    /* q1t = q1 */
    "movq %q2, %%rdx\n"    /* r0t = r0 */
    "movq %q1, %%r9\n"     /* r1t = r1 */
    "subq $1, %%rax\n"     /* q1t -= 1 */
    "addq %q7, %%rdx\n"    /* r0t += d0 */
    "adcq %q6, %%r9\n"     /* r1t += d1 + cf */
    "cmpq %%r8, %q1\n"     /* cmp(r1, q0) */
    "cmovaeq %%rax, %q0\n" /* q1 = q1t if r1 >= q0 */
    "cmovaeq %%rdx, %q2\n" /* r0 = r0t if r1 >= q0 */
    "cmovaeq %%r9, %q1\n"  /* r1 = r1t if r1 >= q0 */

    /* q1 += 1 if (r1, r0) >= (d1, d0) */
    /* (r1, r0) -= (d1, d0) if (r1, r0) >= (d1, d0) */
    "cmpq %q6, %q1\n"      /* cmp(r1, d1) */
    "jb 2f\n"              /* skip if r1 < d1 */
    "ja 1f\n"              /* do if r1 > d1 */
    "cmpq %q7, %q2\n"      /* cmp(r0, d0) */
    "jb 2f\n"              /* skip if r0 < d0 */
    "1:\n"
    "addq $1, %q0\n"       /* q1 += 1 */
    "subq %q7, %q2\n"      /* r0 -= d0 */
    "sbbq %q6, %q1\n"      /* r1 -= d1 + cf */
    "2:\n"
    : "=&r" (*q), "=&r" (*k1), "=&r" (*k0)
    : "rm" (u2), "rm" (u1), "rm" (u0),
      "rm" (d1), "rm" (d0), "rm" (v)
    : "cc", "rax", "rdx", "r8", "r9"
  );
#else /* !MP_HAVE_ASM_X64 */
  mp_limb_t q1, q0, r1, r0, t1, t0;

  mp_mul(q1, q0, v, u2);

  q0 += u1;
  q1 += u2 + (q0 < u1);

  r0 = u0;
  r1 = u1 - d1 * q1;

  mp_mul(t1, t0, d0, q1);

  r1 -= t1 + (r0 < t0);
  r0 -= t0;

  r1 -= d1 + (r0 < d0);
  r0 -= d0;

  /* At this point, we have computed:
   *
   *   q = (((B^3 - 1) / d) - B) * (u / B^2) + (u / B)
   *     = ((B^3 - 1) * u) / (B^2 * d)
   *
   * On an 8-bit machine, this implies:
   *
   *   q = (u * 0xffffff) / (d << 16)
   *
   * For example, if we want to compute:
   *
   *   [q, r] = 0x421 / 0x83 = [0x08, 0x09]
   *
   * We first compute:
   *
   *   q = 0x0420fffbdf / 0x830000 = 0x0811
   *
   * Note that the actual quotient is
   * in the high bits of the result.
   *
   * Our remainder is trickier. We now
   * compute:
   *
   *   r = (u1 - q1 * d1) mod B
   *     = 0x04 - 0x08 * 0x00
   *     = 0x04
   *
   *   r = (r * B + u0 - (d0 * q1) - d) mod B^2
   *     = 0x0400 + 0x21 - 0x83 * 0x08 - 0x83
   *     = -0x7a (allowed to underflow)
   *     = 0xff86 mod B^2
   *
   * Since 0xff >= 0x11, the first branch
   * is triggered, computing:
   *
   *   r = r + d
   *     = 0xff86 + 0x83
   *     = 0x09 mod B^2
   */
  q1 += 1;

  if (UNPREDICTABLE(r1 >= q0)) {
    q1 -= 1;
    r0 += d0;
    r1 += d1 + (r0 < d0);
  }

  if (UNLIKELY(r1 > d1 || (r1 == d1 && r0 >= d0))) {
    q1 += 1;
    r1 -= d1 + (r0 < d0);
    r0 -= d0;
  }

  *q = q1;
  *k1 = r1;
  *k0 = r0;
#endif /* !MP_HAVE_ASM_X64 */
}

static mp_limb_t
mp_inv_mod(mp_limb_t d) {
  /* Compute d^-1 mod B.
   *
   * This is necessary for exact division
   * algorithms as described by the GMP
   * documentation[1].
   *
   * Lemire and Reynolds describe a simple
   * method for computing inverses modulo
   * a machine word base[2][3].
   *
   * There may be a more optimal method used
   * in GMP, but I'm too afraid to check lest
   * it taints our clean room reimplementation.
   *
   * [1] https://gmplib.org/manual/Exact-Division
   * [2] https://lemire.me/blog/2017/09/18/computing-the-inverse-of-odd-integers
   * [3] https://marc-b-reynolds.github.io/math/2017/09/18/ModInverse.html
   */
  mp_limb_t m = d;

  ASSERT((d & 1) != 0);

  m *= 2 - d * m;
  m *= 2 - d * m;
  m *= 2 - d * m;
  m *= 2 - d * m;
#if MP_LIMB_BITS == 64
  m *= 2 - d * m;
#endif

  return m;
}

/*
 * Division Engine
 */

static void
mpn_divmod_init_1(mp_divisor_t *den, mp_limb_t d) {
  mp_bits_t shift;

  if (d == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  shift = mp_clz(d);

  den->vp = &den->tmp;
  den->vp[0] = d << shift;
  den->inv = mp_inv_2by1(den->vp[0]);
  den->shift = shift;
}

#define mpn_divmod_init(den, nn, dp, dn) do { \
  (den)->up = mp_alloc_vla((nn) + 1);         \
  (den)->vp = mp_alloc_vla(dn);               \
  mpn_divmod_precomp(den, dp, dn);            \
} while (0)

#define mpn_divmod_clear(den, nn, dn) do { \
  mp_free_vla((den)->up, (nn) + 1);        \
  mp_free_vla((den)->vp, dn);              \
} while (0)

static void
mpn_divmod_precomp(mp_divisor_t *den, const mp_limb_t *dp, mp_size_t dn) {
  mp_bits_t shift;

  if (dn == 0 || dp[dn - 1] == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  /* Normalize the denominator. */
  shift = mp_clz(dp[dn - 1]);

  if (dn == 1) {
    den->vp[0] = dp[0] << shift;
  } else {
    if (shift != 0)
      mpn_lshift(den->vp, dp, dn, shift);
    else
      mpn_copyi(den->vp, dp, dn);
  }

  /* Compute inverse of top limb. */
#if defined(MP_USE_DIV_3BY2)
  if (dn == 1)
    den->inv = mp_inv_2by1(den->vp[dn - 1]);
  else
    den->inv = mp_inv_3by2(den->vp[dn - 1], den->vp[dn - 2]);
#else
  den->inv = mp_inv_2by1(den->vp[dn - 1]);
#endif

  den->shift = shift;
  den->size = dn;
}

static void
mpn_divmod_small_2by1(mp_limb_t *qp, mp_limb_t *rp,
                      const mp_limb_t *np, mp_size_t nn,
                      const mp_divisor_t *den) {
  /* [DIV] Algorithm 7, Page 7, Section C. */
  mp_bits_t s = den->shift;
  mp_limb_t d = den->vp[0];
  mp_limb_t m = den->inv;
  mp_limb_t q, n1, n0;
  mp_limb_t r = 0;
  mp_size_t j;

  for (j = nn - 1; j >= 0; j--) {
    n1 = r;
    n0 = np[j];

    if (s != 0) {
      n1 = (n1 << s) | (n0 >> (MP_LIMB_BITS - s));
      n0 <<= s;
    }

    /* [q, r] = (n1, n0) / d */
    mp_div_2by1(&q, &r, n1, n0, d, m);

    r >>= s;

    if (qp != NULL)
      qp[j] = q;
  }

  if (rp != NULL)
    rp[0] = r;
}

BTC_UNUSED static void
mpn_divmod_small_3by2(mp_limb_t *qp, mp_limb_t *rp,
                      const mp_limb_t *np, mp_size_t nn,
                      mp_divisor_t *den) {
  /* [DIV] Algorithm 7, Page 7, Section C. */
  mp_limb_t n2, n1, n0, q, r1, r0;
  mp_bits_t s = den->shift;
  mp_limb_t d1 = den->vp[1];
  mp_limb_t d0 = den->vp[0];
  mp_limb_t *up = den->up;
  mp_limb_t m = den->inv;
  mp_size_t j;

  /* Normalize. */
  if (s != 0) {
    up[nn] = mpn_lshift(up, np, nn, s);
  } else {
    mpn_copyi(up, np, nn);
    up[nn] = 0;
  }

  r1 = up[nn - 0];
  r0 = up[nn - 1];

  for (j = nn - 2; j >= 0; j--) {
    n2 = r1;
    n1 = r0;
    n0 = up[j];

    /* [q, (r1, r0)] = (n2, n1, n0) / (d1, d0) */
    mp_div_3by2(&q, &r1, &r0, n2, n1, n0, d1, d0, m);

    if (qp != NULL)
      qp[j] = q;
  }

  /* Unnormalize. */
  if (rp != NULL) {
    if (s != 0) {
      r0 = (r0 >> s) | (r1 << (MP_LIMB_BITS - s));
      r1 >>= s;
    }

    rp[0] = r0;
    rp[1] = r1;
  }
}

BTC_UNUSED static void
mpn_divmod_large_2by1(mp_limb_t *qp, mp_limb_t *rp,
                      const mp_limb_t *np, mp_size_t nn,
                      mp_divisor_t *den) {
  /* Division of nonnegative integers.
   *
   * [KNUTH] Algorithm D, Page 272, Section 4.3.1.
   *
   * Originally based on the `divmnu64` function[1]
   * in Hacker's Delight, the code below has taken
   * on some modifications:
   *
   *   1. The 2-by-1 inverse is used instead of
   *      hardware division[2].
   *   2. Wide limb arithmetic is avoided in the
   *      quotient estimation loop.
   *   3. The `qhat >= B` case is accounted for
   *      separately.
   *
   * The last two make this more similar to Knuth's
   * conceptual implementation of Algorithm D, which
   * was more wary of machine word size.
   *
   * [1] https://gist.github.com/chjj/d59b19c32b2ccbb1a7b397cd77cc7025
   * [2] [DIV] Algorithm 4, Page 4, Section A.
   */
  const mp_limb_t *vp = den->vp;
  mp_limb_t *up = den->up;
  mp_limb_t m = den->inv;
  mp_size_t dn = den->size;
  mp_limb_t qhat, rhat, c;
  mp_size_t j;

  /* D1. Normalize. */
  if (den->shift != 0) {
    up[nn] = mpn_lshift(up, np, nn, den->shift);
  } else {
    mpn_copyi(up, np, nn);
    up[nn] = 0;
  }

  /* D2. Initialize j. */
  for (j = nn - dn; j >= 0; j--) {
    /* D3. Calculate qhat. */
    if (UNLIKELY(up[j + dn] == vp[dn - 1])) {
      /* This algorithm always ensures:
       *
       *   up[j + dn] <= vp[dn - 1]
       *
       * It's the equals part we need to worry about.
       *
       * In this case, qhat would overflow beyond the
       * limb width. The `divmnu64` code would subtract
       * until qhat equals the maximum word. We can
       * simply assign it instead. In other words,
       * covers the `qhat >= B` check in `divmnu64`.
       *
       * Knuth's code (but not algorithm) implements
       * this, skipping beyond the estimation loop and
       * setting qhat to B-1 if the division overflows.
       * Furthermore, it sets rhat to up[j+dn-1] and
       * then jumps into the middle of the estimation
       * loop, bypassing the `qhat -= 1` instruction.
       *
       * See line 040 (JOV 1F) for the beginning of
       * this behavior. I've never seen this explained
       * anywhere, but I suspect Knuth does this for
       * the following reasons...
       *
       * In the case of `qhat == B` we have:
       *
       *   qhat = B
       *   rhat = up[j + dn - 1] mod vp[dn - 1]
       *
       * Since `vp` is normalized, we could compute:
       *
       *   rhat = up[j + dn - 1];
       *
       *   if (rhat >= vp[dn - 1])
       *     rhat -= vp[dn - 1];
       *
       * However, since we set qhat to B-1, we have:
       *
       *   qhat = B - 1
       *   rhat = (up[j + dn - 1] mod vp[dn - 1]) + vp[dn - 1]
       *
       * Which would change the computation to:
       *
       *   rhat = up[j + dn - 1];
       *
       *   if (rhat < vp[dn - 1])
       *      rhat += vp[dn - 1];
       *
       * The estimation loop may still not be entered,
       * as it _now_ hinges on the assumption of:
       *
       *   vp[dn - 2] > rhat (with qhat = B)
       *
       * So we must bypass the initial check, and
       * since we already computed `qhat -= 1` we must
       * enter into the middle of the loop.
       *
       * To emulate this behavior (while avoiding a
       * goto statement), we could add the following
       * lines below:
       *
       *   rhat = up[j + dn - 1];
       *
       *   if (rhat < vp[dn - 1]) {
       *     rhat += vp[dn - 1];
       *     qhat -= mp_mul_gt_2(qhat, vp[dn - 2], rhat, up[j + dn - 2]);
       *   }
       *
       * But, to keep the code simple, we allow for
       * the overestimation, and let the "Add back"
       * step handle the adjustment.
       */
      qhat = MP_LIMB_MAX;
    } else {
      /* [qhat, rhat] = (up[j + dn], up[j + dn - 1]) / vp[dn - 1] */
      mp_div_2by1(&qhat, &rhat, up[j + dn], up[j + dn - 1], vp[dn - 1], m);

      /* Repeat while: qhat * vp[dn - 2] > (rhat, up[j + dn - 2])
                until: rhat >= B */
      while (mp_mul_gt_2(qhat, vp[dn - 2], rhat, up[j + dn - 2])) {
        qhat -= 1;
        rhat += vp[dn - 1];

        if (rhat < vp[dn - 1])
          break;
      }
    }

    /* D4. Multiply and subtract. */
    c = mpn_submul_1(up + j, vp, dn, qhat);

    mp_sub(up[j + dn], c, up[j + dn], c);

    /* D5. Test remainder. */
    if (c != 0) {
      /* D6. Add back. */
      up[j + dn] += mpn_add_n(up + j, up + j, vp, dn);

      qhat -= 1;
    }

    if (qp != NULL)
      qp[j] = qhat;

    /* D7. Loop on j. */
  }

  /* D8. Unnormalize. */
  if (rp != NULL) {
    if (den->shift != 0)
      mpn_rshift(rp, up, dn, den->shift);
    else
      mpn_copyi(rp, up, dn);
  }
}

BTC_UNUSED static void
mpn_divmod_large_3by2(mp_limb_t *qp, mp_limb_t *rp,
                      const mp_limb_t *np, mp_size_t nn,
                      mp_divisor_t *den) {
  /* Division of nonnegative integers.
   *
   * [KNUTH] Algorithm D, Page 272, Section 4.3.1.
   *
   * Modified for 3-by-2 division[1].
   *
   * Read the above comments before reading this.
   *
   * This function should be faster than the 2-by-1
   * version for three reasons in particular:
   *
   *   1. It does not require a quotient estimation
   *      loop after our division.
   *
   *   2. The "Add back" step is less likely to be
   *      triggered.
   *
   *   3. Since our 2-limb remainder (r1, r0) is
   *      computed with our division, we can skip
   *      two iterations in the multiply+subtract
   *      step (since all it is doing is computing
   *      a remainder anyway).
   *
   * This implementation originally overlooked the
   * final optimization, but it seems to appear in
   * other libraries[2] which implement the 3-by-2
   * division.
   *
   * Note that although this function _should_ be
   * faster, it currently seems slower in practice.
   *
   * [1] [DIV] Algorithm 5, Page 5, Section IV.
   * [2] https://github.com/chfast/intx/blob/58e8907/include/intx/intx.hpp#L824
   */
  mp_limb_t n2, n1, n0, q, r2, r1, r0, c;
  const mp_limb_t *vp = den->vp;
  mp_limb_t *up = den->up;
  mp_limb_t m = den->inv;
  mp_size_t dn = den->size;
  mp_limb_t d1 = vp[dn - 1];
  mp_limb_t d0 = vp[dn - 2];
  mp_size_t j;

  /* D1. Normalize. */
  if (den->shift != 0) {
    up[nn] = mpn_lshift(up, np, nn, den->shift);
  } else {
    mpn_copyi(up, np, nn);
    up[nn] = 0;
  }

  /* D2. Initialize j. */
  for (j = nn - dn; j >= 0; j--) {
    n2 = up[j + dn - 0];
    n1 = up[j + dn - 1];
    n0 = up[j + dn - 2];

    /* D3. Calculate qhat. */
    if (UNLIKELY(n2 == d1 && n1 == d0)) {
      /* This differs from Knuth's algorithm in the
       * following way...
       *
       * Because we skip the estimation loop, we
       * have our exact remainder available from
       * inside the else clause. We must calculate
       * it for this clause for reasons stated in
       * the above description.
       *
       * As the result of this operation we should
       * have computed:
       *
       *   q = B
       *   r = n0
       *
       * Note that unlike the 2-by-1 function, no
       * modulo is necessary for `r` as `r` is
       * always less than `(d1, d0)`.
       *
       * However, our quotient must be reduced, and
       * this means our remainder must be updated
       * as well. This gives us:
       *
       *   q = B - 1
       *   r = (0, n0) + (d1, d0)
       *
       * This is strange as it gives us a potential
       * 3-word remainder, particularly in the case
       * where the following conditions are true:
       *
       *   - d0 >= B/2
       *   - d1 == B-1
       *   - n0 >= B/2
       *   - n1 >= B/2 (implied by d0)
       *   - n2 == B-1 (implied by d1)
       *
       * An 8-bit example (with B=0x100):
       *
       *   n = 0xff8197
       *   d = 0xff81
       *   n / d = B
       *   n % d = 0x97 = n0
       *   n0 + d = n % d + d = 0x97 + d = 0x010018
       *   n - (B - 1) * d = 0x010018
       *
       *   r0 = 0x97 - (0x81 * 0xff + 0x00) = 0x18
       *   r1 = 0x81 - (0xff * 0xff + 0x80) = 0x00
       *   r2 = 0xff - (0x00 * 0xff + 0x02) = 0x01
       *
       * We must account for the extra word in the
       * multiply and subtract step below.
       */
      q = MP_LIMB_MAX;
      r0 = n0 + d0;
      r1 = d1 + (r0 < d0);
      r2 = (r1 < d1);
    } else {
      /* [q, (r1, r0)] = (n2, n1, n0) / (d1, d0) */
      mp_div_3by2(&q, &r1, &r0, n2, n1, n0, d1, d0, m);

      /* No high limb. */
      r2 = 0;
    }

    /* D4. Multiply and subtract. */
    c = mpn_submul_1(up + j, vp, dn - 2, q);

    /* Utilize the remainder for a small speedup
     * in avoiding 2 iterations of submul.
     *
     * For our last 2(+1) iterations of submul,
     * we want to compute:
     *
     *   up[j + dn - 2] -= vp[dn - 2] * q + c
     *   up[j + dn - 1] -= vp[dn - 1] * q + c
     *   up[j + dn - 0] -= vp[dn - 0] * q + c
     *
     * We're in luck because this was already
     * mostly computed with our divison, meaning:
     *
     *   r0 = up[j + dn - 2] - vp[dn - 2] * q
     *   r1 = up[j + dn - 1] - vp[dn - 1] * q - c
     *   r2 = up[j + dn - 0] - vp[dn - 0] * q - c
     *
     * Therefore, we only need to compute:
     *
     *   up[j + dn - 2] = r0 - c
     *   up[j + dn - 1] = r1 - c
     *   up[j + dn - 0] = r2 - c
     */
    mp_sub(up[j + dn - 2], c, r0, c);
    mp_sub(up[j + dn - 1], c, r1, c);
    mp_sub(up[j + dn - 0], c, r2, c);

    /* D5. Test remainder. */
    if (UNLIKELY(c != 0)) {
      /* D6. Add back. */
      up[j + dn] += mpn_add_n(up + j, up + j, vp, dn);

      q -= 1;
    }

    if (qp != NULL)
      qp[j] = q;

    /* D7. Loop on j. */
  }

  /* D8. Unnormalize. */
  if (rp != NULL) {
    if (den->shift != 0)
      mpn_rshift(rp, up, dn, den->shift);
    else
      mpn_copyi(rp, up, dn);
  }
}

static BTC_INLINE mp_limb_t
mpn_divmod_inner_1(mp_limb_t *qp, const mp_limb_t *np,
                                  mp_size_t nn,
                                  const mp_divisor_t *den) {
  mp_limb_t r;

  mpn_divmod_small_2by1(qp, &r, np, nn, den);

  return r;
}

static BTC_INLINE void
mpn_divmod_inner(mp_limb_t *qp, mp_limb_t *rp,
                 const mp_limb_t *np, mp_size_t nn,
                 mp_divisor_t *den) {
  mp_size_t dn = den->size;

  if (nn < dn)
    btc_abort(); /* LCOV_EXCL_LINE */

#if defined(MP_USE_DIV_3BY2)
  if (dn == 1)
    mpn_divmod_small_2by1(qp, rp, np, nn, den);
  else if (dn == 2)
    mpn_divmod_small_3by2(qp, rp, np, nn, den);
  else
    mpn_divmod_large_3by2(qp, rp, np, nn, den);
#else
  if (dn == 1)
    mpn_divmod_small_2by1(qp, rp, np, nn, den);
  else
    mpn_divmod_large_2by1(qp, rp, np, nn, den);
#endif
}

static BTC_INLINE void
mpn_mod_inner(mp_limb_t *rp, const mp_limb_t *np,
                             mp_size_t nn,
                             mp_divisor_t *den) {
  mpn_divmod_inner(NULL, rp, np, nn, den);
}

/*
 * Division
 */

mp_limb_t
mpn_divmod_1(mp_limb_t *qp, const mp_limb_t *np, mp_size_t nn, mp_limb_t d) {
  mp_divisor_t den;
  mp_limb_t q, r;

  if (d == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (nn == 0)
    return 0;

  if (nn == 1) {
    q = np[0] / d;
    r = np[0] - q * d;

    if (qp != NULL)
      qp[0] = q;

    return r;
  }

  mpn_divmod_init_1(&den, d);

  return mpn_divmod_inner_1(qp, np, nn, &den);
}

void
mpn_div_1(mp_limb_t *qp, const mp_limb_t *np, mp_size_t nn, mp_limb_t d) {
  mpn_divmod_1(qp, np, nn, d);
}

mp_limb_t
mpn_mod_1(const mp_limb_t *np, mp_size_t nn, mp_limb_t d) {
  return mpn_divmod_1(NULL, np, nn, d);
}

void
mpn_divmod(mp_limb_t *qp, mp_limb_t *rp,
           const mp_limb_t *np, mp_size_t nn,
           const mp_limb_t *dp, mp_size_t dn) {
  mp_divisor_t den;

  if (dn == 0 || nn < dn)
    btc_abort(); /* LCOV_EXCL_LINE */

  mpn_divmod_init(&den, nn, dp, dn);
  mpn_divmod_inner(qp, rp, np, nn, &den);
  mpn_divmod_clear(&den, nn, dn);
}

void
mpn_div(mp_limb_t *qp, const mp_limb_t *np, mp_size_t nn,
                       const mp_limb_t *dp, mp_size_t dn) {
  mpn_divmod(qp, NULL, np, nn, dp, dn);
}

void
mpn_mod(mp_limb_t *rp, const mp_limb_t *np, mp_size_t nn,
                       const mp_limb_t *dp, mp_size_t dn) {
  mpn_divmod(NULL, rp, np, nn, dp, dn);
}

/*
 * Exact Division
 */

void
mpn_divexact_1(mp_limb_t *qp, const mp_limb_t *np, mp_size_t nn, mp_limb_t d) {
  CHECK(mpn_divmod_1(qp, np, nn, d) == 0);
}

void
mpn_divexact(mp_limb_t *qp, const mp_limb_t *np, mp_size_t nn,
                            const mp_limb_t *dp, mp_size_t dn) {
  mp_limb_t *rp;

  if (dn == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  rp = mp_alloc_vla(dn);

  mpn_divmod(qp, rp, np, nn, dp, dn);

  CHECK(mpn_strip(rp, dn) == 0);

  mp_free_vla(rp, dn);
}

/*
 * Roots
 */

mp_size_t
mpn_sqrtrem(mp_limb_t *zp, mp_limb_t *rp, const mp_limb_t *xp, mp_size_t xn) {
  mp_size_t sn, rn, zn;
  mpz_t x, s, r;

  if (zp == xp)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (xn == 0 || xp[xn - 1] == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  mpz_roset_n(x, xp, xn);

  mpz_init(s);
  mpz_init(r);

  mpz_sqrtrem(s, r, x);

  sn = MP_ABS(s->size);
  rn = MP_ABS(r->size);
  zn = (xn + 1) / 2;

  CHECK(sn <= zn);
  CHECK(rn <= xn);

  mpn_copyi(zp, s->limbs, sn);
  mpn_zero(zp + sn, zn - sn);

  if (rp != NULL) {
    mpn_copyi(rp, r->limbs, rn);
    mpn_zero(rp + rn, xn - rn);
  }

  mpz_clear(s);
  mpz_clear(r);

  return rn;
}

int
mpn_perfect_square_p(const mp_limb_t *xp, mp_size_t xn) {
  mpz_t x;

  if (xn == 0 || xp[xn - 1] == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  mpz_roset_n(x, xp, xn);

  return mpz_perfect_square_p(x);
}

/*
 * AND
 */

void
mpn_and_n(mp_limb_t *zp, const mp_limb_t *xp,
                         const mp_limb_t *yp,
                         mp_size_t n) {
  mp_size_t i;

  for (i = 0; i < n; i++)
    zp[i] = xp[i] & yp[i];
}

/*
 * OR
 */

void
mpn_ior_n(mp_limb_t *zp, const mp_limb_t *xp,
                         const mp_limb_t *yp,
                         mp_size_t n) {
  mp_size_t i;

  for (i = 0; i < n; i++)
    zp[i] = xp[i] | yp[i];
}

/*
 * XOR
 */

void
mpn_xor_n(mp_limb_t *zp, const mp_limb_t *xp,
                         const mp_limb_t *yp,
                         mp_size_t n) {
  mp_size_t i;

  for (i = 0; i < n; i++)
    zp[i] = xp[i] ^ yp[i];
}

/*
 * AND+NOT
 */

void
mpn_andn_n(mp_limb_t *zp, const mp_limb_t *xp,
                          const mp_limb_t *yp,
                          mp_size_t n) {
  mp_size_t i;

  for (i = 0; i < n; i++)
    zp[i] = xp[i] & ~yp[i];
}

/*
 * OR+NOT
 */

void
mpn_iorn_n(mp_limb_t *zp, const mp_limb_t *xp,
                          const mp_limb_t *yp,
                          mp_size_t n) {
  mp_size_t i;

  for (i = 0; i < n; i++)
    zp[i] = xp[i] | ~yp[i];
}

/*
 * NOT+AND
 */

void
mpn_nand_n(mp_limb_t *zp, const mp_limb_t *xp,
                          const mp_limb_t *yp,
                          mp_size_t n) {
  mp_size_t i;

  for (i = 0; i < n; i++)
    zp[i] = ~(xp[i] & yp[i]);
}

/*
 * NOT+OR
 */

void
mpn_nior_n(mp_limb_t *zp, const mp_limb_t *xp,
                          const mp_limb_t *yp,
                          mp_size_t n) {
  mp_size_t i;

  for (i = 0; i < n; i++)
    zp[i] = ~(xp[i] | yp[i]);
}

/*
 * NOT+XOR
 */

void
mpn_xnor_n(mp_limb_t *zp, const mp_limb_t *xp,
                          const mp_limb_t *yp,
                          mp_size_t n) {
  mp_size_t i;

  for (i = 0; i < n; i++)
    zp[i] = ~(xp[i] ^ yp[i]);
}

/*
 * NOT
 */

void
mpn_com(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn) {
  mp_size_t i;

  for (i = 0; i < xn; i++)
    zp[i] = ~xp[i];
}

/*
 * Left Shift
 */

mp_limb_t
mpn_lshift(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_bits_t bits) {
  mp_limb_t c;
  mp_size_t i;

  ASSERT(xn > 0);
  ASSERT(bits > 0 && bits < MP_LIMB_BITS);

  c = xp[xn - 1] >> (MP_LIMB_BITS - bits);

  for (i = xn - 1; i >= 1; i--)
    zp[i] = (xp[i] << bits) | (xp[i - 1] >> (MP_LIMB_BITS - bits));

  zp[0] = xp[0] << bits;

  return c;
}

/*
 * Right Shift
 */

mp_limb_t
mpn_rshift(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_bits_t bits) {
  mp_limb_t c;
  mp_size_t i;

  ASSERT(xn > 0);
  ASSERT(bits > 0 && bits < MP_LIMB_BITS);

  c = xp[0] << (MP_LIMB_BITS - bits);

  for (i = 0; i < xn - 1; i++)
    zp[i] = (xp[i + 1] << (MP_LIMB_BITS - bits)) | (xp[i] >> bits);

  zp[xn - 1] = xp[xn - 1] >> bits;

  return c >> (MP_LIMB_BITS - bits);
}

/*
 * Bit Manipulation
 */

mp_limb_t
mpn_getbit(const mp_limb_t *xp, mp_size_t xn, mp_bits_t pos) {
  mp_size_t s = pos / MP_LIMB_BITS;

  if (s >= xn)
    return 0;

  return (xp[s] >> (pos % MP_LIMB_BITS)) & 1;
}

mp_limb_t
mpn_getbits(const mp_limb_t *xp, mp_size_t xn, mp_bits_t pos, mp_bits_t width) {
  mp_size_t s = pos / MP_LIMB_BITS;
  mp_bits_t r, m;
  mp_limb_t z, b;

  ASSERT(width < MP_LIMB_BITS);

  if (s >= xn)
    return 0;

  r = pos % MP_LIMB_BITS;
  z = (xp[s] >> r) & MP_MASK(width);

  if (r + width > MP_LIMB_BITS && s + 1 < xn) {
    m = r + width - MP_LIMB_BITS;
    b = xp[s + 1] & MP_MASK(m);

    z |= b << (MP_LIMB_BITS - r);
  }

  return z;
}

int
mpn_tstbit(const mp_limb_t *xp, mp_bits_t pos) {
  return (xp[pos / MP_LIMB_BITS] >> (pos % MP_LIMB_BITS)) & 1;
}

void
mpn_setbit(mp_limb_t *zp, mp_bits_t pos) {
  zp[pos / MP_LIMB_BITS] |= MP_LIMB_C(1) << (pos % MP_LIMB_BITS);
}

void
mpn_clrbit(mp_limb_t *zp, mp_bits_t pos) {
  zp[pos / MP_LIMB_BITS] &= ~(MP_LIMB_C(1) << (pos % MP_LIMB_BITS));
}

void
mpn_combit(mp_limb_t *zp, mp_bits_t pos) {
  mp_size_t s = pos / MP_LIMB_BITS;
  mp_bits_t r = pos % MP_LIMB_BITS;
  mp_limb_t b = (zp[s] >> r) & 1;

  zp[s] &= ~(MP_LIMB_C(1) << r);
  zp[s] |= (b ^ 1) << r;
}

static mp_bits_t
mpn_scan(const mp_limb_t *xp, mp_size_t xn,
         mp_limb_t m, mp_limb_t c, mp_bits_t pos) {
  mp_size_t s = pos / MP_LIMB_BITS;
  mp_bits_t r, b;
  mp_limb_t z;

  m -= 1;

  if (s >= xn)
    return m == 0 ? MP_BITS_MAX : pos;

  mp_sub(z, c, xp[s], c);

  r = pos % MP_LIMB_BITS;
  z = (z ^ m) & ~MP_MASK(r);

  if (z != 0) {
    b = mp_ctz(z);

    if (b >= r)
      return s * MP_LIMB_BITS + b;
  }

  for (s++; s < xn; s++) {
    mp_sub(z, c, xp[s], c);

    if (z == m)
      continue;

    return s * MP_LIMB_BITS + mp_ctz(z ^ m);
  }

  return m == 0 ? MP_BITS_MAX : s * MP_LIMB_BITS;
}

mp_bits_t
mpn_scan0(const mp_limb_t *xp, mp_size_t xn, mp_bits_t pos) {
  return mpn_scan(xp, xn, 0, 0, pos);
}

mp_bits_t
mpn_scan1(const mp_limb_t *xp, mp_size_t xn, mp_bits_t pos) {
  return mpn_scan(xp, xn, 1, 0, pos);
}

mp_bits_t
mpn_popcount(const mp_limb_t *xp, mp_size_t xn) {
  mp_bits_t c = 0;
  mp_size_t i;

  for (i = 0; i < xn; i++)
    c += mp_popcount(xp[i]);

  return c;
}

mp_bits_t
mpn_hamdist(const mp_limb_t *xp, const mp_limb_t *yp, mp_size_t n) {
  mp_bits_t c = 0;
  mp_size_t i;

  for (i = 0; i < n; i++)
    c += mp_popcount(xp[i] ^ yp[i]);

  return c;
}

void
mpn_mask(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_bits_t bits) {
  mp_size_t zn = bits / MP_LIMB_BITS;
  mp_bits_t lo = bits % MP_LIMB_BITS;

  if (zn >= xn) {
    if (zp != xp)
      mpn_copyi(zp, xp, xn);
    return;
  }

  if (zp != xp)
    mpn_copyi(zp, xp, zn);

  if (lo != 0) {
    zp[zn] = xp[zn] & MP_MASK(lo);
    zn += 1;
  }

  if (xn > zn)
    mpn_zero(zp + zn, xn - zn);
}

/*
 * Negation
 */

mp_limb_t
mpn_neg(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn) {
  mp_limb_t c = 1;

  switch (xn & 3) {
    case 3:
      mp_neg_1(*zp, c, *xp); zp++; xp++;
    case 2:
      mp_neg_1(*zp, c, *xp); zp++; xp++;
    case 1:
      mp_neg_1(*zp, c, *xp); zp++; xp++;
  }

  xn >>= 2;

  while (xn--) {
    /* [z, c] = ~x + c */
#if defined(mp_neg_1x4)
    mp_neg_1x4(zp, c, xp);
#else
    mp_neg_1(zp[0], c, xp[0]);
    mp_neg_1(zp[1], c, xp[1]);
    mp_neg_1(zp[2], c, xp[2]);
    mp_neg_1(zp[3], c, xp[3]);
#endif

    zp += 4;
    xp += 4;
  }

  return c ^ 1;
}

/*
 * Weak Reduction
 */

int
mpn_reduce_weak(mp_limb_t *zp, const mp_limb_t *xp,
                               const mp_limb_t *np,
                               mp_size_t n,
                               mp_limb_t hi,
                               mp_limb_t *scratch) {
  /* `n` limbs are required for scratch. */
  mp_limb_t *tp = scratch;
  mp_limb_t c = mpn_sub_n(tp, xp, np, n);

  c = (hi < c); /* [, c] = hi - c */

  mpn_cnd_select(zp, xp, tp, n, c == 0);

  return c == 0;
}

/*
 * Barrett Reduction
 */

void
mpn_barrett(mp_limb_t *mp, const mp_limb_t *np,
                           mp_size_t n,
                           mp_size_t shift,
                           mp_limb_t *scratch) {
  /* Barrett precomputation.
   *
   * [HANDBOOK] Page 603, Section 14.3.3.
   *
   * `shift + 1` limbs are required for scratch.
   *
   * Must have `shift - n + 1` limbs at mp.
   */
  mp_limb_t *xp = scratch;
  mp_size_t xn = shift + 1;

  CHECK(n > 0);
  CHECK(shift >= n * 2);

  /* m = 2^(shift * L) / n */
  mpn_zero(xp, shift);

  xp[shift] = 1;

  mpn_div(xp, xp, xn, np, n);

  CHECK(mpn_strip(xp, xn - n + 1) == shift - n + 1);

  mpn_copyi(mp, xp, shift - n + 1);
}

void
mpn_reduce(mp_limb_t *zp, const mp_limb_t *xp,
                          const mp_limb_t *mp,
                          const mp_limb_t *np,
                          mp_size_t n,
                          mp_size_t shift,
                          mp_limb_t *scratch) {
  /* Barrett reduction.
   *
   * [HANDBOOK] Algorithm 14.42, Page 604, Section 14.3.3.
   *
   * `1 + shift + mn` limbs are required for scratch.
   *
   * In other words: `2 * (shift + 1) - n` limbs.
   */
  mp_size_t mn = shift - n + 1;
  mp_limb_t *qp = scratch;
  mp_limb_t *hp = scratch + 1;

  /* h = x * m */
  mpn_mul(hp, xp, shift, mp, mn);

  /* h = h >> (shift * L) */
  hp += shift;

  /* q = x - h * n */
  mpn_mul(qp, hp, mn, np, n);
  mpn_sub_n(qp, xp, qp, shift);

  /* q = q - n if q >= n */
  mpn_reduce_weak(zp, qp, np, n, qp[n], hp);
}

/*
 * Montgomery Multiplication (logic from golang)
 */

void
mpn_mont(mp_limb_t *kp,
         mp_limb_t *rp,
         const mp_limb_t *mp,
         mp_size_t n,
         mp_limb_t *scratch) {
  /* Montgomery precomputation.
   *
   * [HANDBOOK] Page 600, Section 14.3.2.
   *
   * `2 * n + 1` limbs are required for scratch.
   */
  mp_limb_t *xp = scratch;
  mp_size_t xn = n * 2 + 1;

  CHECK(n > 0);

  /* k = -m^-1 mod 2^L */
  kp[0] = -mp_inv_mod(mp[0]);

  /* r = 2^(2 * n * L) mod m */
  mpn_zero(xp, n * 2);

  xp[n * 2] = 1;

  mpn_mod(rp, xp, xn, mp, n);
}

static BTC_INLINE mp_limb_t
mpn_montmul_inner(const mp_limb_t *xp,
                  const mp_limb_t *yp,
                  const mp_limb_t *mp,
                  mp_size_t n,
                  mp_limb_t k,
                  mp_limb_t *scratch) {
  /* Montgomery multiplication.
   *
   * [MONT] Algorithm 4 & 5, Page 5, Section 3.
   *
   * `2 * n` limbs are required for scratch.
   */
  mp_limb_t *tp = scratch;
  mp_limb_t c1, c2, c3, cx, cy;
  mp_size_t i;

  ASSERT(n > 0);

  c2 = mpn_mul_1(tp, xp, n, yp[0]);
  c3 = mpn_addmul_1(tp, mp, n, tp[0] * k);

  mp_add(tp[n], c1, c2, c3);

  for (i = 1; i < n; i++) {
    c2 = mpn_addmul_1(tp + i, xp, n, yp[i]);
    c3 = mpn_addmul_1(tp + i, mp, n, tp[i] * k);

    mp_add(cx, c2, c1, c2);
    mp_add(cy, c3, cx, c3);

    c1 = c2 | c3;

    tp[n + i] = cy;
  }

  return c1;
}

void
mpn_montmul(mp_limb_t *zp, const mp_limb_t *xp,
                           const mp_limb_t *yp,
                           const mp_limb_t *mp,
                           mp_size_t n,
                           mp_limb_t k,
                           mp_limb_t *scratch) {
  /* Word-by-Word Almost Montgomery Multiplication.
   *
   * [MONT] Algorithm 4, Page 5, Section 3.
   */
  mp_limb_t *tp = scratch;
  mp_limb_t c = mpn_montmul_inner(xp, yp, mp, n, k, tp);

  if (c != 0)
    mpn_sub_n(zp, tp + n, mp, n);
  else
    mpn_copyi(zp, tp + n, n);
}

void
mpn_sec_montmul(mp_limb_t *zp, const mp_limb_t *xp,
                               const mp_limb_t *yp,
                               const mp_limb_t *mp,
                               mp_size_t n,
                               mp_limb_t k,
                               mp_limb_t *scratch) {
  /* Word-by-Word Montgomery Multiplication.
   *
   * [MONT] Algorithm 4, Page 5, Section 3.
   */
  mp_limb_t *tp = scratch;
  mp_limb_t c = mpn_montmul_inner(xp, yp, mp, n, k, tp);

  mpn_reduce_weak(zp, tp + n, mp, n, c, tp);
}

/*
 * Number Theoretic Functions
 */

mp_size_t
mpn_gcd(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                       const mp_limb_t *yp, mp_size_t yn,
                       mp_limb_t *scratch) {
  /* Binary GCD algorithm.
   *
   * [KNUTH] Algorithm B, Page 338, Section 4.5.2.
   */
  mp_limb_t *up = &scratch[0];
  mp_limb_t *vp = &scratch[xn];
  mp_size_t un = xn;
  mp_size_t vn = yn;
  mp_bits_t r, bits;
  mp_bits_t s = 0;
  mp_size_t zn;
  mp_limb_t c;

  if (xn == 0 || xp[xn - 1] == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (yn == 0 || yp[yn - 1] == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (xn < yn)
    btc_abort(); /* LCOV_EXCL_LINE */

  mpn_copyi(up, xp, xn);
  mpn_copyi(vp, yp, yn);

  while ((up[0] | vp[0]) == 0) {
    up++;
    un--;
    vp++;
    vn--;
    s++;
  }

  r = mp_ctz(up[0] | vp[0]);

  if (r > 0) {
    mpn_rshift(up, up, un, r);
    mpn_rshift(vp, vp, vn, r);

    un -= (up[un - 1] == 0);
    vn -= (vp[vn - 1] == 0);
  }

  while (un != 0) {
    MPN_SHIFT_ZEROES(bits, up, un);
    MPN_SHIFT_ZEROES(bits, vp, vn);

    if (mpn_cmp2(up, un, vp, vn) >= 0) {
      mpn_sub(up, up, un, vp, vn);
      un = mpn_strip(up, un);
    } else {
      mpn_sub(vp, vp, vn, up, un);
      vn = mpn_strip(vp, vn);
    }
  }

  zn = vn + s;

  if (r > 0) {
    c = mpn_lshift(zp + s, vp, vn, r);

    if (c != 0)
      zp[zn++] = c;
  } else {
    mpn_copyd(zp + s, vp, vn);
  }

  ASSERT(zn <= yn);

  mpn_zero(zp, s);
  mpn_zero(zp + zn, yn - zn);

  return zn;
}

mp_limb_t
mpn_gcd_1(const mp_limb_t *xp, mp_size_t xn, mp_limb_t y, mp_limb_t *scratch) {
  /* Binary GCD algorithm.
   *
   * [KNUTH] Algorithm B, Page 338, Section 4.5.2.
   */
  mp_limb_t *up = &scratch[0];
  mp_size_t un = xn;
  mp_bits_t s, bits;
  mp_limb_t v;

  if (xn == 0 || xp[xn - 1] == 0 || y == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  s = mp_ctz(xp[0] | y);

  if (s > 0) {
    mpn_rshift(up, xp, xn, s);
    un -= (up[un - 1] == 0);
  } else {
    mpn_copyi(up, xp, xn);
  }

  v = y >> s;

  while (un != 0) {
    MPN_SHIFT_ZEROES(bits, up, un);

    v >>= mp_ctz(v);

    if (un > 1 || up[0] >= v) {
      mpn_sub_1(up, up, un, v);
      un -= (up[un - 1] == 0);
    } else {
      v -= up[0];
    }
  }

  return v << s;
}

mp_size_t
mpn_gcdext(mp_limb_t *gp,
           mp_limb_t *sp, mp_size_t *sn,
           const mp_limb_t *xp, mp_size_t xn,
           const mp_limb_t *yp, mp_size_t yn) {
  mpz_t x, y, g, b;
  mp_size_t gn, bn;

  if (xn == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (yn == 0 || yp[yn - 1] == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (xn < yn)
    btc_abort(); /* LCOV_EXCL_LINE */

  mpz_roset_n(x, xp, mpn_strip(xp, xn));
  mpz_roset_n(y, yp, yn);

  mpz_init(g);
  mpz_init(b);

  mpz_gcdext(g, b, NULL, x, y);
  mpz_rem(b, b, y);

  gn = MP_ABS(g->size);
  bn = MP_ABS(b->size);

  CHECK(gn <= yn);
  CHECK(bn <= yn + 1);

  mpn_copyi(gp, g->limbs, gn);
  mpn_zero(gp + gn, yn - gn);

  mpn_copyi(sp, b->limbs, bn);
  mpn_zero(sp + bn, yn + 1 - bn);

  *sn = b->size;

  mpz_clear(g);
  mpz_clear(b);

  return gn;
}

int
mpn_invert(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                          const mp_limb_t *yp, mp_size_t yn,
                          mp_limb_t *scratch) {
  /* Penk's right shift binary EGCD.
   *
   * [KNUTH] Exercise 4.5.2.39, Page 646.
   */
  mp_limb_t *up = &scratch[0 * (yn + 1)];
  mp_limb_t *vp = &scratch[1 * (yn + 1)];
  mp_limb_t *ap = &scratch[2 * (yn + 1)];
  mp_limb_t *bp = &scratch[3 * (yn + 1)];
  mp_size_t un, vn;
  mp_bits_t uz, vz;

  if (xn > 0 && xp[xn - 1] == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (yn == 0 || yp[yn - 1] == 0 || (yp[0] & 1) == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (xn > yn)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (yn == 1 && yp[0] == 1) {
    mpn_zero(zp, yn);
    return 0;
  }

  mpn_copyi(up, xp, xn);
  mpn_copyi(vp, yp, yn);

  mpn_set_1(ap, yn + 1, 1);
  mpn_set_1(bp, yn + 1, 0);

  un = xn;
  vn = yn;

  while (un != 0) {
    MPN_SHIFT_ZEROES(uz, up, un);
    MPN_SHIFT_ZEROES(vz, vp, vn);

    while (uz--) {
      if (ap[0] & 1)
        ap[yn] = mpn_add_n(ap, ap, yp, yn);

      mpn_rshift(ap, ap, yn + ap[yn], 1);
    }

    while (vz--) {
      if (bp[0] & 1)
        bp[yn] = mpn_add_n(bp, bp, yp, yn);

      mpn_rshift(bp, bp, yn + bp[yn], 1);
    }

    if (mpn_cmp2(up, un, vp, vn) >= 0) {
      mpn_sub(up, up, un, vp, vn);
      mpn_sub_mod(ap, ap, bp, yp, yn);

      un = mpn_strip(up, un);
    } else {
      mpn_sub(vp, vp, vn, up, un);
      mpn_sub_mod(bp, bp, ap, yp, yn);

      vn = mpn_strip(vp, vn);
    }
  }

  if (vn != 1 || vp[0] != 1) {
    mpn_zero(zp, yn);
    return 0;
  }

  mpn_copyi(zp, bp, yn);

  return 1;
}

int
mpn_invert_n(mp_limb_t *zp, const mp_limb_t *xp,
                            const mp_limb_t *yp,
                            mp_size_t n,
                            mp_limb_t *scratch) {
  mp_size_t xn = mpn_strip(xp, n);
  mp_size_t yn = n;

  return mpn_invert(zp, xp, xn, yp, yn, scratch);
}

int
mpn_sec_invert(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                              const mp_limb_t *mp, mp_size_t mn,
                              mp_limb_t *scratch) {
  mp_limb_t *yp = &scratch[0 * mn];
  mp_limb_t *tp = &scratch[1 * mn];
  mp_size_t yn = mn;

  if (mn == 0 || mp[mn - 1] == 0 || (mp[0] & 1) == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (mn == 1 && mp[0] == 1) {
    mpn_zero(zp, mn);
    return 0;
  }

  mpn_sub_1(yp, mp, mn, 2);

  yn -= (yp[yn - 1] == 0);

  mpn_sec_powm(zp, xp, xn, yp, yn, mp, mn, tp);

  return mpn_sec_zero_p(zp, mn) ^ 1;
}

int
mpn_sec_invert_n(mp_limb_t *zp, const mp_limb_t *xp,
                                const mp_limb_t *yp,
                                mp_size_t n,
                                mp_limb_t *scratch) {
  return mpn_sec_invert(zp, xp, n, yp, n, scratch);
}

int
mpn_jacobi(const mp_limb_t *xp, mp_size_t xn,
           const mp_limb_t *yp, mp_size_t yn,
           mp_limb_t *scratch) {
  /* Binary Jacobi Symbol.
   *
   * [JACOBI] Page 3, Section 3.
   */
  mp_limb_t *up = &scratch[0 * yn];
  mp_limb_t *vp = &scratch[1 * yn];
  mp_size_t un, vn;
  mp_bits_t bits;
  int j = 1;

  if (xn > 0 && xp[xn - 1] == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (yn == 0 || yp[yn - 1] == 0 || (yp[0] & 1) == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (xn > yn)
    btc_abort(); /* LCOV_EXCL_LINE */

  mpn_copyi(up, xp, xn);
  mpn_copyi(vp, yp, yn);

  un = xn;
  vn = yn;

  while (un != 0) {
    MPN_SHIFT_ZEROES(bits, up, un);

    if (bits & 1) {
      if ((vp[0] & 7) == 3 || (vp[0] & 7) == 5)
        j = -j;
    }

    if (mpn_cmp2(up, un, vp, vn) < 0) {
      MPN_SWAP(up, un, vp, vn);

      if ((up[0] & 3) == 3 && (vp[0] & 3) == 3)
        j = -j;
    }

    mpn_sub(up, up, un, vp, vn);

    un = mpn_strip(up, un);

    if (un > 0) {
      mpn_rshift(up, up, un, 1);
      un -= (up[un - 1] == 0);
    }

    if ((vp[0] & 7) == 3 || (vp[0] & 7) == 5)
      j = -j;
  }

  if (vn != 1 || vp[0] != 1)
    return 0;

  return j;
}

int
mpn_jacobi_n(const mp_limb_t *xp,
             const mp_limb_t *yp,
             mp_size_t n,
             mp_limb_t *scratch) {
  mp_size_t xn = mpn_strip(xp, n);
  mp_size_t yn = n;

  return mpn_jacobi(xp, xn, yp, yn, scratch);
}

static void
mpn_div_powm(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                            const mp_limb_t *yp, mp_size_t yn,
                            const mp_limb_t *mp, mp_size_t mn,
                            mp_limb_t *scratch) {
  /* Sliding window with division. */
  mp_limb_t *ap = &scratch[0 * mn]; /* mn */
  mp_limb_t *rp = &scratch[1 * mn]; /* mn */
  mp_limb_t *sp = &scratch[2 * mn]; /* 2 * mn */
  mp_limb_t *tp = &scratch[4 * mn]; /* 2 * mn */
  mp_limb_t *wp = &scratch[6 * mn]; /* wnd_size * mn */
  mp_bits_t i, j, len, width, shift;
  mp_size_t sn = mn * 2;
  mp_divisor_t den;
  mp_limb_t bits;

  len = yn * MP_LIMB_BITS - mp_clz(yp[yn - 1]);

  mpn_copyi(ap, xp, xn);
  mpn_zero(ap + xn, mn - xn);

  mpn_divmod_init(&den, sn, mp, mn);

  if (yn > 2) {
    mpn_sqr(sp, ap, mn, tp);
    mpn_mod_inner(rp, sp, sn, &den);

#define WND(i) (&wp[(i) * mn])

    mpn_copyi(WND(0), ap, mn);

    for (i = 1; i < MP_SLIDE_SIZE; i++) {
      mpn_mul_n(sp, WND(i - 1), rp, mn);
      mpn_mod_inner(WND(i), sp, sn, &den);
    }

    i = len;

    while (i >= MP_SLIDE_WIDTH) {
      width = MP_SLIDE_WIDTH;
      bits = mpn_getbits(yp, yn, i - width, width);

      if (bits < MP_SLIDE_SIZE) {
        mpn_sqr(sp, rp, mn, tp);
        mpn_mod_inner(rp, sp, sn, &den);
        i -= 1;
        continue;
      }

      shift = mp_ctz(bits);
      width -= shift;
      bits >>= shift;

      if (i == len) {
        mpn_copyi(rp, WND(bits >> 1), mn);
      } else {
        for (j = 0; j < width; j++) {
          mpn_sqr(sp, rp, mn, tp);
          mpn_mod_inner(rp, sp, sn, &den);
        }

        mpn_mul_n(sp, rp, WND(bits >> 1), mn);
        mpn_mod_inner(rp, sp, sn, &den);
      }

#undef WND

      i -= width;
    }
  } else {
    mpn_copyi(rp, ap, mn);

    i = len - 1;
  }

  for (i -= 1; i >= 0; i--) {
    mpn_sqr(sp, rp, mn, tp);
    mpn_mod_inner(rp, sp, sn, &den);

    if (mpn_tstbit(yp, i)) {
      mpn_mul_n(sp, rp, ap, mn);
      mpn_mod_inner(rp, sp, sn, &den);
    }
  }

  if (mpn_cmp(rp, mp, mn) >= 0)
    mpn_mod_inner(zp, rp, mn, &den);
  else
    mpn_copyi(zp, rp, mn);

  mpn_divmod_clear(&den, sn, mn);
}

static void
mpn_mont_powm(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                             const mp_limb_t *yp, mp_size_t yn,
                             const mp_limb_t *mp, mp_size_t mn,
                             mp_limb_t *scratch) {
  /* Sliding window with montgomery. */
  mp_limb_t *ap = &scratch[0 * mn]; /* mn */
  mp_limb_t *rp = &scratch[1 * mn]; /* mn */
  mp_limb_t *tp = &scratch[2 * mn]; /* 2 * mn + 1 */
  mp_limb_t *rr = &scratch[4 * mn + 1]; /* mn */
  mp_limb_t *wp = &scratch[5 * mn + 1]; /* wnd_size * mn */
  mp_bits_t i, j, len, width, shift;
  mp_limb_t k, bits;

  len = yn * MP_LIMB_BITS - mp_clz(yp[yn - 1]);

  mpn_copyi(ap, xp, xn);
  mpn_zero(ap + xn, mn - xn);

  mpn_mont(&k, rr, mp, mn, tp);

  mpn_montmul(ap, ap, rr, mp, mn, k, tp);

  if (yn > 2) {
    mpn_montmul(rp, ap, ap, mp, mn, k, tp);

#define WND(i) (&wp[(i) * mn])

    mpn_copyi(WND(0), ap, mn);

    for (i = 1; i < MP_SLIDE_SIZE; i++)
      mpn_montmul(WND(i), WND(i - 1), rp, mp, mn, k, tp);

    i = len;

    while (i >= MP_SLIDE_WIDTH) {
      width = MP_SLIDE_WIDTH;
      bits = mpn_getbits(yp, yn, i - width, width);

      if (bits < MP_SLIDE_SIZE) {
        mpn_montmul(rp, rp, rp, mp, mn, k, tp);
        i -= 1;
        continue;
      }

      shift = mp_ctz(bits);
      width -= shift;
      bits >>= shift;

      if (i == len) {
        mpn_copyi(rp, WND(bits >> 1), mn);
      } else {
        for (j = 0; j < width; j++)
          mpn_montmul(rp, rp, rp, mp, mn, k, tp);

        mpn_montmul(rp, rp, WND(bits >> 1), mp, mn, k, tp);
      }

#undef WND

      i -= width;
    }
  } else {
    mpn_copyi(rp, ap, mn);

    i = len - 1;
  }

  for (i -= 1; i >= 0; i--) {
    mpn_montmul(rp, rp, rp, mp, mn, k, tp);

    if (mpn_tstbit(yp, i))
      mpn_montmul(rp, rp, ap, mp, mn, k, tp);
  }

  mpn_set_1(rr, mn, 1);
  mpn_montmul(rp, rp, rr, mp, mn, k, tp);

  if (mpn_cmp(rp, mp, mn) >= 0) {
    mpn_sub_n(rp, rp, mp, mn);

    if (mpn_cmp(rp, mp, mn) >= 0)
      mpn_mod(rp, rp, mn, mp, mn);
  }

  mpn_copyi(zp, rp, mn);
}

void
mpn_powm(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                        const mp_limb_t *yp, mp_size_t yn,
                        const mp_limb_t *mp, mp_size_t mn,
                        mp_limb_t *scratch) {
  /* Top limb must be non-zero. */
  if (mn == 0 || mp[mn - 1] == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  /* Ensure x <= m. */
  if (xn > mn)
    btc_abort(); /* LCOV_EXCL_LINE */

  /* x^y mod 1 = 0 */
  if (mn == 1 && mp[0] == 1) {
    mpn_zero(zp, mn);
    return;
  }

  /* Strip exponent. */
  yn = mpn_strip(yp, yn);

  /* x^0 mod m = 1 */
  if (yn == 0) {
    mpn_set_1(zp, mn, 1);
    return;
  }

  /* 0^y mod m = 0 */
  if (xn == 0) {
    mpn_zero(zp, mn);
    return;
  }

  if (yn > 1 && (mp[0] & 1) != 0) {
    /* Montgomery multiplication. */
    mpn_mont_powm(zp, xp, xn, yp, yn, mp, mn, scratch);
  } else {
    /* Division (faster for smaller exponents). */
    mpn_div_powm(zp, xp, xn, yp, yn, mp, mn, scratch);
  }
}

void
mpn_sec_powm(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn,
                            const mp_limb_t *yp, mp_size_t yn,
                            const mp_limb_t *mp, mp_size_t mn,
                            mp_limb_t *scratch) {
  /* Fixed window montgomery. */
  mp_limb_t *rp = &scratch[0 * mn]; /* mn */
  mp_limb_t *tp = &scratch[1 * mn]; /* 2 * mn + 1 */
  mp_limb_t *sp = &scratch[3 * mn + 1]; /* mn */
  mp_limb_t *rr = &scratch[4 * mn + 1]; /* mn */
  mp_limb_t *wp = &scratch[5 * mn + 1]; /* wnd_size * mn */
  mp_bits_t i, steps;
  mp_limb_t j, k, b;

  if (mn == 0 || mp[mn - 1] == 0 || (mp[0] & 1) == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (xn > mn)
    btc_abort(); /* LCOV_EXCL_LINE */

  mpn_copyi(rp, xp, xn);
  mpn_zero(rp + xn, mn - xn);

  mpn_mont(&k, rr, mp, mn, tp);

#define WND(i) (&wp[(i) * mn])

  mpn_set_1(WND(0), mn, 1);
  mpn_sec_montmul(WND(0), WND(0), rr, mp, mn, k, tp);
  mpn_sec_montmul(WND(1), rp, rr, mp, mn, k, tp);

  for (i = 2; i < MP_FIXED_SIZE; i++)
    mpn_sec_montmul(WND(i), WND(i - 1), WND(1), mp, mn, k, tp);

  steps = ((yn * MP_LIMB_BITS) + MP_FIXED_WIDTH - 1) / MP_FIXED_WIDTH;

  mpn_copyi(rp, WND(0), mn);
  mpn_zero(sp, mn);

  for (i = steps - 1; i >= 0; i--) {
    b = mpn_getbits(yp, yn, i * MP_FIXED_WIDTH, MP_FIXED_WIDTH);

    for (j = 0; j < MP_FIXED_SIZE; j++)
      mpn_cnd_select(sp, sp, WND(j), mn, j == b);

    if (i == steps - 1) {
      mpn_copyi(rp, sp, mn);
    } else {
      for (j = 0; j < MP_FIXED_WIDTH; j++)
        mpn_sec_montmul(rp, rp, rp, mp, mn, k, tp);

      mpn_sec_montmul(rp, rp, sp, mp, mn, k, tp);
    }
  }

#undef WND

  mpn_set_1(rr, mn, 1);
  mpn_sec_montmul(zp, rp, rr, mp, mn, k, tp);
}

/*
 * Primes
 */

/* First 172 primes (2-1021). */
static const mp_limb_t mp_primes[] = {
#if MP_LIMB_BITS == 32
  MP_LIMB_C(0xa08a28ac), MP_LIMB_C(0x28208a20),
  MP_LIMB_C(0x02088288), MP_LIMB_C(0x800228a2),
  MP_LIMB_C(0x20a00a08), MP_LIMB_C(0x80282088),
  MP_LIMB_C(0x800800a2), MP_LIMB_C(0x08028228),
  MP_LIMB_C(0x0a20a082), MP_LIMB_C(0x22880020),
  MP_LIMB_C(0x28020800), MP_LIMB_C(0x88208082),
  MP_LIMB_C(0x02022020), MP_LIMB_C(0x08828028),
  MP_LIMB_C(0x8008a202), MP_LIMB_C(0x20880880),
  MP_LIMB_C(0x20000a00), MP_LIMB_C(0x0a082008),
  MP_LIMB_C(0x82820802), MP_LIMB_C(0x00800a20),
  MP_LIMB_C(0x0028208a), MP_LIMB_C(0x20080822),
  MP_LIMB_C(0x20808020), MP_LIMB_C(0x02208088),
  MP_LIMB_C(0x20080022), MP_LIMB_C(0x28a00a00),
  MP_LIMB_C(0x8a200080), MP_LIMB_C(0x008a2000),
  MP_LIMB_C(0x00808800), MP_LIMB_C(0x02082202),
  MP_LIMB_C(0x80820880), MP_LIMB_C(0x28220020)
#else
  MP_LIMB_C(0x28208a20a08a28ac),
  MP_LIMB_C(0x800228a202088288),
  MP_LIMB_C(0x8028208820a00a08),
  MP_LIMB_C(0x08028228800800a2),
  MP_LIMB_C(0x228800200a20a082),
  MP_LIMB_C(0x8820808228020800),
  MP_LIMB_C(0x0882802802022020),
  MP_LIMB_C(0x208808808008a202),
  MP_LIMB_C(0x0a08200820000a00),
  MP_LIMB_C(0x00800a2082820802),
  MP_LIMB_C(0x200808220028208a),
  MP_LIMB_C(0x0220808820808020),
  MP_LIMB_C(0x28a00a0020080022),
  MP_LIMB_C(0x008a20008a200080),
  MP_LIMB_C(0x0208220200808800),
  MP_LIMB_C(0x2822002080820880)
#endif
};

static mp_size_t
mpn_sieve_size(mp_limb_t n) {
  mp_limb_t zn;

  CHECK(n < MP_LOW_MASK * MP_LOW_MASK);
  CHECK(n <= MP_LIMB_MAX - MP_LIMB_BITS);
  CHECK(n <= MP_BITS_MAX);

  /* ((n + 1) + (L - 1)) / L */
  zn = (n + MP_LIMB_BITS) / MP_LIMB_BITS;

  CHECK(zn <= MP_SIZE_MAX);

  return zn;
}

static void
mpn_sieve(mp_limb_t *zp, mp_limb_t n) {
  /* Sieve of Eratosthenes. */
  mp_limb_t zn = (n + MP_LIMB_BITS) / MP_LIMB_BITS;
  mp_bits_t lo = (n + 1) % MP_LIMB_BITS;
  mp_limb_t i, p;

  if (n < 1024) {
    mpn_copyi(zp, mp_primes, zn);
    return;
  }

  for (i = 0; i < zn; i++)
    zp[i] = MP_LIMB_MAX;

  for (p = 2; p * p <= n; p++) {
    if (mpn_tstbit(zp, p)) {
      for (i = p * p; i <= n; i += p)
        mpn_clrbit(zp, i);
    }
  }

  zp[0] &= ~MP_LIMB_C(3);

  if (lo != 0)
    zp[zn - 1] &= MP_MASK(lo);
}

/*
 * Helpers
 */

mp_bits_t
mpn_ctz(const mp_limb_t *xp, mp_size_t xn) {
  mp_size_t i;

  for (i = 0; i < xn; i++) {
    if (xp[i] != 0)
      return i * MP_LIMB_BITS + mp_ctz(xp[i]);
  }

  return xn * MP_LIMB_BITS;
}

mp_bits_t
mpn_bitlen(const mp_limb_t *xp, mp_size_t xn) {
  mp_size_t i;

  for (i = xn - 1; i >= 0; i--) {
    if (xp[i] != 0)
      return i * MP_LIMB_BITS + mp_bitlen(xp[i]);
  }

  return 0;
}

size_t
mpn_bytelen(const mp_limb_t *xp, mp_size_t xn) {
  return (mpn_bitlen(xp, xn) + 7) / 8;
}

size_t
mpn_sizeinbase(const mp_limb_t *xp, mp_size_t xn, int base) {
  size_t len = 0;

  if (base < 2)
    btc_abort(); /* LCOV_EXCL_LINE */

  xn = mpn_strip(xp, xn);

  if (xn == 0)
    return 1;

  if ((base & (base - 1)) == 0) {
    mp_bits_t bits = xn * MP_LIMB_BITS - mp_clz(xp[xn - 1]);
    mp_bits_t width = mp_bitlen(base - 1);

    len = (bits + width - 1) / width;
  } else {
    mp_limb_t *tp = mp_alloc_vla(xn);
    mp_size_t tn = xn;
    mp_divisor_t den;

    mpn_copyi(tp, xp, xn);

    mpn_divmod_init_1(&den, base);

    do {
      mpn_divmod_inner_1(tp, tp, tn, &den);
      tn -= (tp[tn - 1] == 0);
      len += 1;
    } while (tn != 0);

    mp_free_vla(tp, xn);
  }

  return len;
}

/*
 * Constant Time
 */

void
mpn_cnd_zero(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_limb_t cnd) {
  mp_limb_t m = -mp_limb_barrier(cnd != 0);
  mp_size_t i;

  for (i = 0; i < xn; i++)
    zp[i] = xp[i] & ~m;
}

void
mpn_cnd_select(mp_limb_t *zp, const mp_limb_t *xp,
                              const mp_limb_t *yp,
                              mp_size_t n,
                              mp_limb_t cnd) {
  mp_limb_t m = -mp_limb_barrier(cnd != 0);
  mp_size_t i;

  for (i = 0; i < n; i++)
    zp[i] = (xp[i] & ~m) | (yp[i] & m);
}

void
mpn_cnd_swap(mp_limb_t *xp, mp_limb_t *yp, mp_size_t n, mp_limb_t cnd) {
  mp_limb_t m = -mp_limb_barrier(cnd != 0);
  mp_limb_t w;
  mp_size_t i;

  for (i = 0; i < n; i++) {
    w = (xp[i] ^ yp[i]) & m;

    xp[i] ^= w;
    yp[i] ^= w;
  }
}

mp_limb_t
mpn_cnd_add_n(mp_limb_t *zp, const mp_limb_t *xp,
                             const mp_limb_t *yp,
                             mp_size_t n,
                             mp_limb_t cnd) {
  mp_limb_t m = -mp_limb_barrier(cnd != 0);
  mp_limb_t c = 0;
  mp_limb_t y;
  mp_size_t i;

  for (i = 0; i < n; i++) {
    y = yp[i] & m;

    mp_add_1(zp[i], c, xp[i], y);
  }

  return c;
}

mp_limb_t
mpn_cnd_sub_n(mp_limb_t *zp, const mp_limb_t *xp,
                             const mp_limb_t *yp,
                             mp_size_t n,
                             mp_limb_t cnd) {
  mp_limb_t m = -mp_limb_barrier(cnd != 0);
  mp_limb_t c = 0;
  mp_limb_t y;
  mp_size_t i;

  for (i = 0; i < n; i++) {
    y = yp[i] & m;

    mp_sub_1(zp[i], c, xp[i], y);
  }

  return c;
}

mp_limb_t
mpn_cnd_neg(mp_limb_t *zp, const mp_limb_t *xp, mp_size_t xn, mp_limb_t cnd) {
  mp_limb_t m = -mp_limb_barrier(cnd != 0);
  mp_limb_t c = 1 & m;
  mp_limb_t z;
  mp_size_t i;

  for (i = 0; i < xn; i++) {
    /* [z, c] = ~x + c */
    z = (xp[i] ^ m) + c;
    c = (z < c);
    zp[i] = z;
  }

  return (c ^ 1) & m;
}

void
mpn_sec_tabselect(mp_limb_t *zp,
                  const mp_limb_t *tp,
                  mp_size_t n,
                  mp_size_t nents,
                  mp_size_t which) {
  mp_size_t i;

  for (i = 0; i < nents; i++)
    mpn_cnd_select(zp, zp, tp + i * n, n, i == which);
}

int
mpn_sec_zero_p(const mp_limb_t *xp, mp_size_t xn) {
  /* Compute (x == 0) in constant time. */
  mp_limb_t w = 0;
  mp_size_t i;

  for (i = 0; i < xn; i++)
    w |= xp[i];

  w = (w >> 1) | (w & 1);

  return (w - 1) >> (MP_LIMB_BITS - 1);
}

int
mpn_sec_equal_p(const mp_limb_t *xp, const mp_limb_t *yp, mp_size_t n) {
  /* Compute (x == y) in constant time. */
  mp_limb_t w = 0;
  mp_size_t i;

  for (i = 0; i < n; i++)
    w |= xp[i] ^ yp[i];

  w = (w >> 1) | (w & 1);

  return (w - 1) >> (MP_LIMB_BITS - 1);
}

static int
mpn_sec_compare(const mp_limb_t *xp, const mp_limb_t *yp, mp_size_t n) {
  /* Compare in constant time. */
  mp_size_t i = n * 2;
  mp_limb_t eq = 1;
  mp_limb_t lt = 0;
  mp_limb_t a, b;

  while (i--) {
    a = (xp[i / 2] >> ((i % 2) * MP_LOW_BITS)) & MP_LOW_MASK;
    b = (yp[i / 2] >> ((i % 2) * MP_LOW_BITS)) & MP_LOW_MASK;
    lt |= eq & ((a - b) >> (MP_LIMB_BITS - 1));
    eq &= ((a ^ b) - 1) >> (MP_LIMB_BITS - 1);
  }

  return (lt << 1) | eq;
}

int
mpn_sec_lt_p(const mp_limb_t *xp, const mp_limb_t *yp, mp_size_t n) {
  /* Compute (x < y) in constant time. */
  int cmp = mpn_sec_compare(xp, yp, n);
  int lt = cmp >> 1;
  int eq = cmp & 1;

  return lt & (eq ^ 1);
}

int
mpn_sec_lte_p(const mp_limb_t *xp, const mp_limb_t *yp, mp_size_t n) {
  /* Compute (x <= y) in constant time. */
  int cmp = mpn_sec_compare(xp, yp, n);
  int lt = cmp >> 1;
  int eq = cmp & 1;

  return lt | eq;
}

int
mpn_sec_gt_p(const mp_limb_t *xp, const mp_limb_t *yp, mp_size_t n) {
  /* Compute (x > y) in constant time. */
  int cmp = mpn_sec_compare(xp, yp, n);
  int lt = cmp >> 1;
  int eq = cmp & 1;

  return (lt | eq) ^ 1;
}

int
mpn_sec_gte_p(const mp_limb_t *xp, const mp_limb_t *yp, mp_size_t n) {
  /* Compute (x >= y) in constant time. */
  int cmp = mpn_sec_compare(xp, yp, n);
  int lt = cmp >> 1;
  int eq = cmp & 1;

  return (lt ^ 1) | eq;
}

int
mpn_sec_cmp(const mp_limb_t *xp, const mp_limb_t *yp, mp_size_t n) {
  /* Compute mpn_cmp(x, y) in constant time. */
  int cmp = mpn_sec_compare(xp, yp, n);
  int lt = cmp >> 1;
  int eq = cmp & 1;

  return (1 - 2 * lt) * (1 - eq);
}

/*
 * Import
 */

void
mpn_import(mp_limb_t *zp, mp_size_t zn,
           const unsigned char *xp, size_t xn,
           int endian) {
  mp_size_t i = 0;
  mp_limb_t z;

  if (endian == 0)
    endian = mp_host_endian();

  if (endian == 1) {
    xp += xn;

    while (i < zn && xn >= MP_LIMB_BYTES) {
      xp -= MP_LIMB_BYTES;
      xn -= MP_LIMB_BYTES;
      zp[i++] = mp_import_be(xp);
    }

    if (i < zn && xn > 0) {
      xp -= xn;
      z = 0;

      do {
        z <<= 8;
        z |= *xp++;
      } while (--xn);

      zp[i++] = z;
    }
  } else if (endian == -1) {
    while (i < zn && xn >= MP_LIMB_BYTES) {
      zp[i++] = mp_import_le(xp);
      xp += MP_LIMB_BYTES;
      xn -= MP_LIMB_BYTES;
    }

    if (i < zn && xn > 0) {
      xp += xn;
      z = 0;

      do {
        z <<= 8;
        z |= *--xp;
      } while (--xn);

      zp[i++] = z;
    }
  } else {
    btc_abort(); /* LCOV_EXCL_LINE */
  }

  while (i < zn)
    zp[i++] = 0;
}

/*
 * Export
 */

void
mpn_export(unsigned char *zp, size_t zn,
           const mp_limb_t *xp, mp_size_t xn,
           int endian) {
  mp_size_t i = 0;
  mp_limb_t x;

  if (endian == 0)
    endian = mp_host_endian();

  if (endian == 1) {
    zp += zn;

    while (i < xn && zn >= MP_LIMB_BYTES) {
      zp -= MP_LIMB_BYTES;
      zn -= MP_LIMB_BYTES;
      mp_export_be(zp, xp[i++]);
    }

    if (i < xn && zn > 0) {
      x = xp[i];

      do {
        *--zp = x & 0xff;
        x >>= 8;
      } while (--zn);
    }

    while (zn--)
      *--zp = 0;
  } else if (endian == -1) {
    while (i < xn && zn >= MP_LIMB_BYTES) {
      mp_export_le(zp, xp[i++]);
      zp += MP_LIMB_BYTES;
      zn -= MP_LIMB_BYTES;
    }

    if (i < xn && zn > 0) {
      x = xp[i];

      do {
        *zp++ = x & 0xff;
        x >>= 8;
      } while (--zn);
    }

    while (zn--)
      *zp++ = 0;
  } else {
    btc_abort(); /* LCOV_EXCL_LINE */
  }
}

/*
 * String Import
 */

static const char mp_table_36[256] = {
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
   0,  1,  2,  3,  4,  5,  6,  7,
   8,  9, 36, 36, 36, 36, 36, 36,
  36, 10, 11, 12, 13, 14, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24,
  25, 26, 27, 28, 29, 30, 31, 32,
  33, 34, 35, 36, 36, 36, 36, 36,
  36, 10, 11, 12, 13, 14, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24,
  25, 26, 27, 28, 29, 30, 31, 32,
  33, 34, 35, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 36
};

static const char mp_table_62[256] = {
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
   0,  1,  2,  3,  4,  5,  6,  7,
   8,  9, 62, 62, 62, 62, 62, 62,
  62, 10, 11, 12, 13, 14, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24,
  25, 26, 27, 28, 29, 30, 31, 32,
  33, 34, 35, 62, 62, 62, 62, 62,
  62, 36, 37, 38, 39, 40, 41, 42,
  43, 44, 45, 46, 47, 48, 49, 50,
  51, 52, 53, 54, 55, 56, 57, 58,
  59, 60, 61, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62,
  62, 62, 62, 62, 62, 62, 62, 62
};

int
mpn_set_str(mp_limb_t *zp, mp_size_t zn, const char *str, int base) {
  const char *table = base <= 36 ? mp_table_36 : mp_table_62;
  mp_bits_t shift = 0;
  mp_size_t n = 0;
  mp_limb_t c;
  int ch;

  if (str == NULL)
    goto fail;

  if (base < 2 || base > 62)
    goto fail;

  if ((base & (base - 1)) == 0)
    shift = mp_bitlen(base - 1);

  while (*str) {
    ch = *str++;

    if (mp_isspace(ch))
      continue;

    ch = table[ch & 0xff];

    if (UNLIKELY(ch >= base))
      goto fail;

    if (n == 0) {
      c = ch;
    } else if (shift > 0) {
      c = mpn_lshift(zp, zp, n, shift);
      zp[0] |= ch;
    } else {
      c = mpn_mul_1(zp, zp, n, base);
      c += mpn_add_1(zp, zp, n, ch);
    }

    if (c != 0) {
      if (UNLIKELY(n == zn))
        goto fail;

      zp[n++] = c;
    }
  }

  mpn_zero(zp + n, zn - n);

  return 1;
fail:
  mpn_zero(zp, zn);
  return 0;
}

/*
 * String Export
 */

size_t
mpn_get_str(char *str, const mp_limb_t *xp, mp_size_t xn, int base) {
  const char *charset;
  size_t len = 0;
  int ch;

  if (base < 2 || base > 62)
    btc_abort(); /* LCOV_EXCL_LINE */

  xn = mpn_strip(xp, xn);

  if (xn == 0) {
    str[0] = '0';
    str[1] = '\0';
    return 1;
  }

  if (base <= 36) {
    charset = "0123456789abcdefghijklmnopqrstuvwxyz";
  } else {
    charset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                        "abcdefghijklmnopqrstuvwxyz";
  }

  if (base == 2 || base == 4 || base == 16) {
    mp_bits_t shift = mp_bitlen(base - 1);
    mp_bits_t digits = MP_LIMB_BITS / shift;
    mp_limb_t mask = base - 1;
    mp_size_t i;
    mp_bits_t j;
    mp_limb_t x;

    for (i = 0; i < xn - 1; i++) {
      x = xp[i];

      for (j = 0; j < digits; j++) {
        str[len++] = charset[x & mask];
        x >>= shift;
      }
    }

    x = xp[xn - 1];

    do {
      str[len++] = charset[x & mask];
      x >>= shift;
    } while (x != 0);
  } else if ((base & (base - 1)) == 0) {
    mp_bits_t bits = xn * MP_LIMB_BITS - mp_clz(xp[xn - 1]);
    mp_bits_t width = mp_bitlen(base - 1);
    mp_bits_t pos = 0;

    do {
      ch = mpn_getbits(xp, xn, pos, width);
      str[len++] = charset[ch];
      pos += width;
    } while (pos < bits);
  } else {
    mp_limb_t *tp = mp_alloc_vla(xn);
    mp_size_t tn = xn;
    mp_divisor_t den;

    mpn_copyi(tp, xp, xn);

    mpn_divmod_init_1(&den, base);

    do {
      ch = mpn_divmod_inner_1(tp, tp, tn, &den);
      tn -= (tp[tn - 1] == 0);
      str[len++] = charset[ch];
    } while (tn != 0);

    mp_free_vla(tp, xn);
  }

  {
    size_t i = 0;
    size_t j = len - 1;
    size_t k = len >> 1;

    while (k--) {
      ch = str[i];
      str[i++] = str[j];
      str[j--] = ch;
    }
  }

  str[len] = '\0';

  return len;
}

/*
 * STDIO
 */

void
mpn_print(const mp_limb_t *xp, mp_size_t xn, int base, mp_puts_f *mp_puts) {
  size_t size = mpn_sizeinbase(xp, xn, base);
  char *str = mp_alloc_vls(size + 1);

  mpn_get_str(str, xp, xn, base);

  mp_puts(str);
  mp_free_vls(str, size + 1);
}

/*
 * RNG
 */

void
mpn_random(mp_limb_t *zp, mp_size_t zn, mp_rng_f *rng, void *arg) {
  rng(zp, zn * sizeof(mp_limb_t), arg);
}

void
mpn_randomm(mp_limb_t *zp,
            const mp_limb_t *xp, mp_size_t xn,
            mp_rng_f *rng, void *arg) {
  mp_size_t n = mpn_strip(xp, xn);
  mp_bits_t s;

  if (zp == xp)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (n > 0) {
    s = mp_clz(xp[n - 1]);

    do {
      mpn_random(zp, n, rng, arg);

      zp[n - 1] >>= s;
    } while (mpn_cmp(zp, xp, n) >= 0);
  }

  mpn_zero(zp + n, xn - n);
}

/*
 * MPZ Interface
 */

/*
 * Initialization
 */

static void
mpz_init_n(mpz_ptr z, mp_size_t n) {
  n = MP_MAX(1, n);

  z->limbs = mp_alloc_limbs(n);
  z->limbs[0] = 0;
  z->alloc = n;
  z->size = 0;
}

#define mpz_init_vla(z, n) do {  \
  mp_size_t _n = (n);            \
  _n = MP_MAX(1, _n);            \
  (z)->limbs = mp_alloc_vla(_n); \
  (z)->limbs[0] = 0;             \
  (z)->alloc = _n;               \
  (z)->size = 0;                 \
} while (0)

void
mpz_init(mpz_ptr z) {
  mpz_init_n(z, 1);
}

void
mpz_init2(mpz_ptr z, mp_bits_t bits) {
  mpz_init_n(z, (bits + MP_LIMB_BITS - 1) / MP_LIMB_BITS);
}

void
mpz_inits(mpz_ptr z, ...) {
  va_list ap;

  va_start(ap, z);

  while (z != NULL) {
    mpz_init(z);

    z = va_arg(ap, mpz_ptr);
  }

  va_end(ap);
}

void
mpz_init_set(mpz_ptr z, mpz_srcptr x) {
  mpz_init_n(z, MP_ABS(x->size));
  mpz_set(z, x);
}

void
mpz_init_set_ui(mpz_ptr z, mp_limb_t x) {
  mpz_init(z);
  mpz_set_ui(z, x);
}

void
mpz_init_set_si(mpz_ptr z, mp_long_t x) {
  mpz_init(z);
  mpz_set_si(z, x);
}

void
mpz_init_set_compact(mpz_ptr z, mp_limb_t x) {
  mpz_init(z);
  mpz_set_compact(z, x);
}

int
mpz_init_set_str(mpz_ptr z, const char *str, int base) {
  mpz_init(z);
  return mpz_set_str(z, str, base);
}

void
mpz_init_import(mpz_ptr z, const unsigned char *raw, size_t size, int endian) {
  mpz_init(z);
  mpz_import(z, raw, size, endian);
}

/*
 * Uninitialization
 */

void
mpz_clear(mpz_ptr z) {
  ASSERT(z->alloc > 0);

  mp_free_limbs(z->limbs);
}

#define mpz_clear_vla(z) do {          \
  ASSERT((z)->alloc > 0);              \
  mp_free_vla((z)->limbs, (z)->alloc); \
} while (0)

void
mpz_clears(mpz_ptr z, ...) {
  va_list ap;

  va_start(ap, z);

  while (z != NULL) {
    mpz_clear(z);

    z = va_arg(ap, mpz_ptr);
  }

  va_end(ap);
}

void
mpz_cleanse(mpz_ptr z) {
  ASSERT(z->alloc > 0);

  mpn_cleanse(z->limbs, z->alloc);
  mp_free_limbs(z->limbs);
}

#define mpz_cleanse_vla(z) do {        \
  ASSERT((z)->alloc > 0);              \
  mpn_cleanse((z)->limbs, (z)->alloc); \
  mp_free_vla((z)->limbs, (z)->alloc); \
} while (0)

void
mpz_cleanses(mpz_ptr z, ...) {
  va_list ap;

  va_start(ap, z);

  while (z != NULL) {
    mpz_cleanse(z);

    z = va_arg(ap, mpz_ptr);
  }

  va_end(ap);
}

/*
 * Internal
 */

static mp_limb_t *
mpz_grow(mpz_ptr z, mp_size_t n) {
  ASSERT(z->alloc > 0);

  if (n > z->alloc) {
    z->limbs = mp_realloc_limbs(z->limbs, n);
    z->alloc = n;
  }

  return z->limbs;
}

static mp_size_t
mpz_add_size(mpz_srcptr x, mpz_srcptr y) {
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t yn = MP_ABS(y->size);

  return MP_MAX(xn, yn) + 1;
}

/*
 * Assignment
 */

void
mpz_set(mpz_ptr z, mpz_srcptr x) {
  if (z != x) {
    mp_size_t xn = MP_ABS(x->size);
    mp_limb_t *zp = mpz_grow(z, xn);

    mpn_copyi(zp, x->limbs, xn);

    z->size = x->size;
  }
}

void
mpz_roset(mpz_ptr z, mpz_srcptr x) {
  z->limbs = (mp_limb_t *)x->limbs;
  z->alloc = 0;
  z->size = x->size;
}

static void
mpz_roset_n(mpz_ptr z, const mp_limb_t *xp, mp_size_t xs) {
  z->limbs = (mp_limb_t *)xp;
  z->alloc = 0;
  z->size = xs;
}

mpz_srcptr
mpz_roinit_n(mpz_ptr z, const mp_limb_t *xp, mp_size_t xs) {
  mp_size_t zn = mpn_strip(xp, MP_ABS(xs));

  z->limbs = (mp_limb_t *)xp;
  z->alloc = 0;
  z->size = xs < 0 ? -zn : zn;

  return z;
}

void
mpz_set_ui(mpz_ptr z, mp_limb_t x) {
  z->limbs[0] = x;
  z->size = (x != 0);
}

void
mpz_set_si(mpz_ptr z, mp_long_t x) {
  mp_size_t xn = (x != 0);

  z->limbs[0] = mp_limb_cast(x);
  z->size = x < 0 ? -xn : xn;
}

#define mpz_roinit_ui(z, x) do { \
  (z)->limbs = &(x);             \
  (z)->alloc = 0;                \
  (z)->size = ((x) != 0);        \
} while (0)

#define mpz_roinit_si(z, x)       \
  mp_limb_t _t = mp_limb_cast(x); \
  mp_size_t _n = (_t != 0);       \
                                  \
  (z)->limbs = &_t;               \
  (z)->alloc = 0;                 \
  (z)->size = (x) < 0 ? -_n : _n

void
mpz_set_compact(mpz_ptr z, mp_limb_t x) {
  mp_limb_t exponent, mantissa;
  int negative;

  if (x == 0) {
    mpz_set_ui(z, 0);
    return;
  }

  exponent = (x >> 24) & 0xff;
  negative = (x >> 23) & 1;
  mantissa = x & 0x7fffff;

  if (exponent <= 3) {
    mantissa >>= 8 * (3 - exponent);
    mpz_set_ui(z, mantissa);
  } else {
    mpz_set_ui(z, mantissa);
    mpz_mul_2exp(z, z, 8 * (exponent - 3));
  }

  if (negative)
    mpz_neg(z, z);
}

/*
 * Conversion
 */

mp_limb_t
mpz_get_ui(mpz_srcptr x) {
  if (x->size == 0)
    return 0;

  return x->limbs[0];
}

mp_long_t
mpz_get_si(mpz_srcptr x) {
  return mp_long_cast(mpz_get_ui(x), x->size);
}

mp_limb_t
mpz_get_compact(mpz_srcptr x) {
  mp_limb_t z, exponent, mantissa;
  int negative;
  mpz_t t;

  if (x->size == 0)
    return 0;

  exponent = mpz_bytelen(x);
  negative = (x->size < 0);

  if (exponent <= 3) {
    mantissa = mpz_get_ui(x);
    mantissa <<= 8 * (3 - exponent);
  } else {
    mpz_init(t);
    mpz_quo_2exp(t, x, 8 * (exponent - 3));
    mantissa = mpz_get_ui(t);
    mpz_clear(t);
  }

  if (mantissa & 0x800000) {
    mantissa >>= 8;
    exponent += 1;
  }

#if MP_LIMB_BITS == 64
  exponent &= 0xff;
  mantissa &= MP_LOW_MASK;
#endif

  z = (exponent << 24) | mantissa;

  if (negative)
    z |= 0x800000;

  return z;
}

/*
 * Comparison
 */

int
mpz_sgn(mpz_srcptr x) {
  if (x->size == 0)
    return 0;

  return x->size < 0 ? -1 : 1;
}

int
mpz_cmp(mpz_srcptr x, mpz_srcptr y) {
  if (x->size != y->size)
    return x->size < y->size ? -1 : 1;

  if (x->size < 0)
    return -mpn_cmp(x->limbs, y->limbs, -x->size);

  return mpn_cmp(x->limbs, y->limbs, x->size);
}

int
mpz_cmp_ui(mpz_srcptr x, mp_limb_t y) {
  if (x->size < 0)
    return -1;

  return mpn_cmp_1(x->limbs, x->size, y);
}

int
mpz_cmp_si(mpz_srcptr x, mp_long_t y) {
  if (y < 0) {
    if (x->size < 0)
      return -mpz_cmpabs_si(x, y);

    return 1;
  }

  return mpz_cmp_ui(x, y);
}

/*
 * Unsigned Comparison
 */

int
mpz_cmpabs(mpz_srcptr x, mpz_srcptr y) {
  return mpn_cmp2(x->limbs, MP_ABS(x->size),
                  y->limbs, MP_ABS(y->size));
}

int
mpz_cmpabs_ui(mpz_srcptr x, mp_limb_t y) {
  return mpn_cmp_1(x->limbs, MP_ABS(x->size), y);
}

int
mpz_cmpabs_si(mpz_srcptr x, mp_long_t y) {
  return mpz_cmpabs_ui(x, mp_limb_cast(y));
}

/*
 * Arithmetic Helpers
 */

static mp_size_t
mpz_addabs(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t yn = MP_ABS(y->size);
  mp_limb_t *zp;

  if (xn < yn)
    MPZ_CSWAP(x, xn, y, yn);

  zp = mpz_grow(z, xn + 1);

  zp[xn] = mpn_add(zp, x->limbs, xn, y->limbs, yn);

  return xn + (zp[xn] != 0);
}

static mp_size_t
mpz_addabs_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  mp_size_t xn = MP_ABS(x->size);
  mp_limb_t *zp = mpz_grow(z, xn + 1);

  zp[xn] = mpn_add_1(zp, x->limbs, xn, y);

  return xn + (zp[xn] != 0);
}

static mp_size_t
mpz_subabs(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t yn = MP_ABS(y->size);
  mp_limb_t *zp = mpz_grow(z, xn);

  ASSERT(mpn_sub(zp, x->limbs, xn, y->limbs, yn) == 0);

  return mpn_strip(zp, xn);
}

static mp_size_t
mpz_subabs_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  mp_size_t xn = MP_ABS(x->size);
  mp_limb_t *zp = mpz_grow(z, xn);

  ASSERT(mpn_sub_1(zp, x->limbs, xn, y) == 0);

  if (UNLIKELY(xn == 0))
    return 0;

  return xn - (zp[xn - 1] == 0);
}

/*
 * Addition
 */

void
mpz_add(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mp_size_t zn;

  if ((x->size ^ y->size) >= 0) {
    /* x + y == x + y */
    /* (-x) + (-y) == -(x + y) */
    zn = mpz_addabs(z, x, y);
  } else {
    int cmp = mpz_cmpabs(x, y);

    if (cmp == 0) {
      /* x + (-x) == 0 */
      /* (-x) + x == 0 */
      zn = 0;
    } else if (cmp < 0) {
      /* x + (-y) == -(y - x) */
      /* (-x) + y == y - x */
      zn = -mpz_subabs(z, y, x);
    } else {
      /* x + (-y) == x - y */
      /* (-x) + y == -(x - y) */
      zn = mpz_subabs(z, x, y);
    }
  }

  z->size = x->size < 0 ? -zn : zn;
}

void
mpz_add_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  mp_size_t zn;

  if (x->size >= 0) {
    /* x + y == x + y */
    zn = mpz_addabs_ui(z, x, y);
  } else {
    if (mpz_cmpabs_ui(x, y) < 0) {
      /* (-x) + y == y - x */
      z->limbs[0] = y - x->limbs[0];
      zn = 1;
    } else {
      /* (-x) + y == -(x - y) */
      zn = -mpz_subabs_ui(z, x, y);
    }
  }

  z->size = zn;
}

void
mpz_add_si(mpz_ptr z, mpz_srcptr x, mp_long_t y) {
  if (y < 0) {
    /* x + (-y) == x - y */
    /* (-x) + (-y) == (-x) - y */
    mpz_sub_ui(z, x, mp_limb_cast(y));
  } else {
    mpz_add_ui(z, x, y);
  }
}

/*
 * Subtraction
 */

void
mpz_sub(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mp_size_t zn;

  if ((x->size ^ y->size) < 0) {
    /* x - (-y) == x + y */
    /* (-x) - y == -(x + y) */
    zn = mpz_addabs(z, x, y);
  } else {
    int cmp = mpz_cmpabs(x, y);

    if (cmp == 0) {
      /* x - x == 0 */
      /* (-x) - (-x) == 0 */
      zn = 0;
    } else if (cmp < 0) {
      /* x - y == -(y - x) */
      /* (-x) - (-y) == y - x */
      zn = -mpz_subabs(z, y, x);
    } else {
      /* x - y == x - y */
      /* (-x) - (-y) == -(x - y) */
      zn = mpz_subabs(z, x, y);
    }
  }

  z->size = x->size < 0 ? -zn : zn;
}

void
mpz_sub_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  mp_size_t zn;

  if (x->size < 0) {
    /* (-x) - y == -(x + y) */
    zn = -mpz_addabs_ui(z, x, y);
  } else {
    if (x->size == 0) {
      /* 0 - y == -(y) */
      z->limbs[0] = y;
      zn = -(y != 0);
    } else if (mpz_cmpabs_ui(x, y) < 0) {
      /* x - y == -(y - x) */
      z->limbs[0] = y - x->limbs[0];
      zn = -1;
    } else {
      /* x - y == x - y */
      zn = mpz_subabs_ui(z, x, y);
    }
  }

  z->size = zn;
}

void
mpz_sub_si(mpz_ptr z, mpz_srcptr x, mp_long_t y) {
  if (y < 0) {
    /* x - (-y) == x + y */
    /* (-x) - (-y) == (-x) + y */
    mpz_add_ui(z, x, mp_limb_cast(y));
  } else {
    mpz_sub_ui(z, x, y);
  }
}

void
mpz_ui_sub(mpz_ptr z, mp_limb_t x, mpz_srcptr y) {
  /* x - y == -(y - x) */
  mpz_sub_ui(z, y, x);
  mpz_neg(z, z);
}

void
mpz_si_sub(mpz_ptr z, mp_long_t x, mpz_srcptr y) {
  /* x - y == -(y - x) */
  mpz_sub_si(z, y, x);
  mpz_neg(z, z);
}

/*
 * Multiplication
 */

void
mpz_mul(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  const mp_limb_t *xp, *yp;
  mp_size_t xn, yn, zn;
  mp_limb_t *zp, *tp;

  if (x == y) {
    mpz_sqr(z, x);
    return;
  }

  if (x->size == 0 || y->size == 0) {
    z->size = 0;
    return;
  }

  xn = MP_ABS(x->size);
  yn = MP_ABS(y->size);
  zn = xn + yn;

  if (xn < yn)
    MPZ_CSWAP(x, xn, y, yn);

  zp = mpz_grow(z, zn);
  xp = x->limbs;
  yp = y->limbs;

  if (yn == 1) {
    zp[xn] = mpn_mul_1(zp, xp, xn, yp[0]);
  } else if (zp == xp || zp == yp) {
    tp = mp_alloc_vla(zn);

    mpn_mul(tp, xp, xn, yp, yn);
    mpn_copyi(zp, tp, zn);

    mp_free_vla(tp, zn);
  } else {
    mpn_mul(zp, xp, xn, yp, yn);
  }

  zn -= (zp[zn - 1] == 0);

  z->size = (x->size ^ y->size) < 0 ? -zn : zn;
}

void
mpz_mul_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  mp_size_t xn, zn;
  mp_limb_t *zp;

  if (x->size == 0 || y == 0) {
    z->size = 0;
    return;
  }

  xn = MP_ABS(x->size);
  zn = xn + 1;
  zp = mpz_grow(z, zn);

  zp[xn] = mpn_mul_1(zp, x->limbs, xn, y);

  zn -= (zp[zn - 1] == 0);

  z->size = x->size < 0 ? -zn : zn;
}

void
mpz_mul_si(mpz_ptr z, mpz_srcptr x, mp_long_t y) {
  mpz_mul_ui(z, x, mp_limb_cast(y));

  if (y < 0)
    mpz_neg(z, z);
}

void
mpz_sqr(mpz_ptr z, mpz_srcptr x) {
  mp_size_t xn, zn, tn;
  const mp_limb_t *xp;
  mp_limb_t *zp, *tp;

  if (x->size == 0) {
    z->size = 0;
    return;
  }

  xn = MP_ABS(x->size);
  zn = xn * 2;

  zp = mpz_grow(z, zn);
  xp = x->limbs;

  if (xn == 1) {
    mp_sqr(zp[1], zp[0], xp[0]);
  } else if (zp == xp) {
    tn = zn * 2;
    tp = mp_alloc_vla(tn);

    mpn_sqr(tp, xp, xn, tp + zn);
    mpn_copyi(zp, tp, zn);

    mp_free_vla(tp, tn);
  } else if (zn <= mp_alloca_max) {
    tp = mp_alloc_vla(zn);

    mpn_sqr(zp, xp, xn, tp);

    mp_free_vla(tp, zn);
  } else {
    mpn_mul(zp, xp, xn, xp, xn);
  }

  zn -= (zp[zn - 1] == 0);

  z->size = zn;
}

void
mpz_addmul(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mpz_t xy;

  if (x->size == 0 || y->size == 0)
    return;

  mpz_init_vla(xy, MP_ABS(x->size) + MP_ABS(y->size));
  mpz_mul(xy, x, y);
  mpz_add(z, z, xy);
  mpz_clear_vla(xy);
}

void
mpz_addmul_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  mpz_t xy;

  if (x->size == 0 || y == 0)
    return;

  mpz_init_vla(xy, MP_ABS(x->size) + 1);
  mpz_mul_ui(xy, x, y);
  mpz_add(z, z, xy);
  mpz_clear_vla(xy);
}

void
mpz_addmul_si(mpz_ptr z, mpz_srcptr x, mp_long_t y) {
  if (y < 0)
    mpz_submul_ui(z, x, mp_limb_cast(y));
  else
    mpz_addmul_ui(z, x, y);
}

void
mpz_submul(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mpz_t xy;

  if (x->size == 0 || y->size == 0)
    return;

  mpz_init_vla(xy, MP_ABS(x->size) + MP_ABS(y->size));
  mpz_mul(xy, x, y);
  mpz_sub(z, z, xy);
  mpz_clear_vla(xy);
}

void
mpz_submul_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  mpz_t xy;

  if (x->size == 0 || y == 0)
    return;

  mpz_init_vla(xy, MP_ABS(x->size) + 1);
  mpz_mul_ui(xy, x, y);
  mpz_sub(z, z, xy);
  mpz_clear_vla(xy);
}

void
mpz_submul_si(mpz_ptr z, mpz_srcptr x, mp_long_t y) {
  if (y < 0)
    mpz_addmul_ui(z, x, mp_limb_cast(y));
  else
    mpz_submul_ui(z, x, y);
}

/*
 * Multiply + Shift
 */

void
mpz_mulshift(mpz_ptr z, mpz_srcptr x, mpz_srcptr y, mp_bits_t bits) {
  mp_size_t sign = x->size ^ y->size;
  mp_limb_t b;

  mpz_mul(z, x, y);

  if (bits > 0) {
    mpz_abs(z, z);

    b = mpz_tstbit(z, bits - 1);

    mpz_quo_2exp(z, z, bits);
    mpz_add_ui(z, z, b);

    if (sign < 0)
      mpz_neg(z, z);
  }
}

/*
 * Truncation Division
 */

void
mpz_quorem(mpz_ptr q, mpz_ptr r, mpz_srcptr n, mpz_srcptr d) {
  mp_size_t nn = MP_ABS(n->size);
  mp_size_t dn = MP_ABS(d->size);
  mp_size_t qs = n->size ^ d->size;
  mp_size_t rs = n->size;
  mp_size_t qn = nn - dn + 1;
  mp_size_t rn = dn;
  mp_limb_t *qp = NULL;
  mp_limb_t *rp = NULL;

  if (q == r || dn == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (mpn_cmp2(n->limbs, nn, d->limbs, dn) < 0) {
    if (r != NULL)
      mpz_set(r, n);

    if (q != NULL)
      q->size = 0;

    return;
  }

  if (q != NULL)
    qp = mpz_grow(q, qn);

  if (r != NULL)
    rp = mpz_grow(r, rn);

  mpn_divmod(qp, rp, n->limbs, nn, d->limbs, dn);

  if (q != NULL) {
    qn -= (qp[qn - 1] == 0);
    q->size = qs < 0 ? -qn : qn;
  }

  if (r != NULL) {
    rn = mpn_strip(rp, rn);
    r->size = rs < 0 ? -rn : rn;
  }
}

void
mpz_quo(mpz_ptr q, mpz_srcptr n, mpz_srcptr d) {
  mpz_quorem(q, NULL, n, d);
}

void
mpz_rem(mpz_ptr r, mpz_srcptr n, mpz_srcptr d) {
  mpz_quorem(NULL, r, n, d);
}

mp_limb_t
mpz_quo_ui(mpz_ptr q, mpz_srcptr n, mp_limb_t d) {
  mp_size_t nn = MP_ABS(n->size);
  mp_limb_t *qp = NULL;
  mp_limb_t r;

  if (d == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (nn == 0) {
    if (q != NULL)
      q->size = 0;

    return 0;
  }

  if (nn == 1 && n->limbs[0] < d) {
    if (q != NULL)
      q->size = 0;

    return n->limbs[0];
  }

  if (q != NULL)
    qp = mpz_grow(q, nn);

  r = mpn_divmod_1(qp, n->limbs, nn, d);

  if (q != NULL) {
    nn -= (qp[nn - 1] == 0);
    q->size = n->size < 0 ? -nn : nn;
  }

  return r;
}

mp_limb_t
mpz_rem_ui(mpz_srcptr n, mp_limb_t d) {
  return mpz_quo_ui(NULL, n, d);
}

mp_long_t
mpz_quo_si(mpz_ptr q, mpz_srcptr n, mp_long_t d) {
  mp_size_t rs = n->size;
  mp_long_t r = mpz_quo_ui(q, n, mp_limb_cast(d));

  if (q != NULL && d < 0)
    mpz_neg(q, q);

  if (rs < 0)
    r = -r;

  return r;
}

mp_long_t
mpz_rem_si(mpz_srcptr n, mp_long_t d) {
  return mpz_quo_si(NULL, n, d);
}

/*
 * Euclidean Division
 */

void
mpz_divmod(mpz_ptr q, mpz_ptr r, mpz_srcptr n, mpz_srcptr d) {
  if (n->size < 0) {
    mpz_t tr, td;

    if (r == NULL) {
      mpz_init_vla(tr, MP_ABS(d->size) + 1);
      r = tr;
    }

    if (q == d || r == d) {
      mpz_init_vla(td, MP_ABS(d->size));
      mpz_set(td, d);
      d = td;
    }

    mpz_quorem(q, r, n, d);

    if (r->size < 0) {
      if (q != NULL) {
        if (d->size < 0)
          mpz_add_ui(q, q, 1);
        else
          mpz_sub_ui(q, q, 1);
      }

      if (d->size < 0)
        mpz_sub(r, r, d);
      else
        mpz_add(r, r, d);
    }

    if (r == tr)
      mpz_clear_vla(tr);

    if (d == td)
      mpz_clear_vla(td);
  } else {
    mpz_quorem(q, r, n, d);
  }
}

void
mpz_div(mpz_ptr q, mpz_srcptr n, mpz_srcptr d) {
  mpz_divmod(q, NULL, n, d);
}

void
mpz_mod(mpz_ptr r, mpz_srcptr n, mpz_srcptr d) {
  mpz_divmod(NULL, r, n, d);
}

mp_limb_t
mpz_div_ui(mpz_ptr q, mpz_srcptr n, mp_limb_t d) {
  mp_size_t rs = n->size;
  mp_limb_t r = mpz_quo_ui(q, n, d);

  if (rs < 0 && r != 0) {
    if (q != NULL)
      mpz_sub_ui(q, q, 1);

    r = d - r;
  }

  return r;
}

mp_limb_t
mpz_mod_ui(mpz_srcptr n, mp_limb_t d) {
  return mpz_div_ui(NULL, n, d);
}

mp_long_t
mpz_div_si(mpz_ptr q, mpz_srcptr n, mp_long_t d) {
  mp_long_t r = mpz_quo_si(q, n, d);

  if (r < 0) {
    if (q != NULL) {
      if (d < 0)
        mpz_add_ui(q, q, 1);
      else
        mpz_sub_ui(q, q, 1);
    }

    if (d < 0)
      r -= d;
    else
      r += d;
  }

  return r;
}

mp_long_t
mpz_mod_si(mpz_srcptr n, mp_long_t d) {
  return mpz_div_si(NULL, n, d);
}

/*
 * Precomputed Division
 */

static mp_limb_t
mpz_mod_2by1(mpz_srcptr x, mp_limb_t d, mp_limb_t m) {
  mp_size_t xn = MP_ABS(x->size);
  mp_limb_t r = 0;
  mp_limb_t q;
  mp_size_t j;

  ASSERT(d >= MP_LIMB_HI);

  if (xn == 0)
    return 0;

  if (xn == 1 && x->limbs[0] < d) {
    r = x->limbs[0];
  } else {
    for (j = xn - 1; j >= 0; j--)
      mp_div_2by1(&q, &r, r, x->limbs[j], d, m);
  }

  if (x->size < 0 && r != 0)
    r = d - r;

  return r;
}

/*
 * Exact Division
 */

void
mpz_divexact(mpz_ptr q, mpz_srcptr n, mpz_srcptr d) {
  mp_size_t nn = MP_ABS(n->size);
  mp_size_t dn = MP_ABS(d->size);
  mp_size_t qn = nn - dn + 1;
  mp_limb_t *qp;

  if (nn < dn) {
    q->size = 0;
    return;
  }

  qp = mpz_grow(q, qn);

  mpn_divexact(qp, n->limbs, nn, d->limbs, dn);

  qn -= (qp[qn - 1] == 0);

  q->size = (n->size ^ d->size) < 0 ? -qn : qn;
}

void
mpz_divexact_ui(mpz_ptr q, mpz_srcptr n, mp_limb_t d) {
  mp_size_t nn = MP_ABS(n->size);
  mp_size_t qn = nn;
  mp_limb_t *qp;

  if (d == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (nn == 0) {
    q->size = 0;
    return;
  }

  qp = mpz_grow(q, qn);

  mpn_divexact_1(qp, n->limbs, nn, d);

  qn -= (qp[qn - 1] == 0);

  q->size = n->size < 0 ? -qn : qn;
}

void
mpz_divexact_si(mpz_ptr q, mpz_srcptr n, mp_long_t d) {
  mpz_divexact_ui(q, n, mp_limb_cast(d));

  if (d < 0)
    mpz_neg(q, q);
}

/*
 * Round Division
 */

void
mpz_divround(mpz_ptr q, mpz_srcptr n, mpz_srcptr d) {
  /* Computes q = (n +- (d / 2)) / d. */
  mpz_t t;

  mpz_init_vla(t, mpz_add_size(n, d));

  mpz_quo_2exp(t, d, 1);

  if ((n->size ^ d->size) < 0)
    mpz_sub(t, n, t);
  else
    mpz_add(t, n, t);

  mpz_quo(q, t, d);

  mpz_clear_vla(t);
}

void
mpz_divround_ui(mpz_ptr q, mpz_srcptr n, mp_limb_t d) {
  mp_size_t s = n->size;
  mp_limb_t r = mpz_quo_ui(q, n, d);
  mp_limb_t h = d >> 1;

  if (r > h || (r == h && (d & 1) == 0)) {
    if (s < 0)
      mpz_sub_ui(q, q, 1);
    else
      mpz_add_ui(q, q, 1);
  }
}

void
mpz_divround_si(mpz_ptr q, mpz_srcptr n, mp_long_t d) {
  mpz_divround_ui(q, n, mp_limb_cast(d));

  if (d < 0)
    mpz_neg(q, q);
}

/*
 * Divisibility
 */

int
mpz_divisible_p(mpz_srcptr n, mpz_srcptr d) {
  mpz_t r;
  int ret;

  if (n->size == 0)
    return 1;

  if (d->size == 0)
    return 0;

  mpz_init_vla(r, MP_ABS(d->size));

  mpz_rem(r, n, d);

  ret = (r->size == 0);

  mpz_clear_vla(r);

  return ret;
}

int
mpz_divisible_ui_p(mpz_srcptr n, mp_limb_t d) {
  if (n->size == 0)
    return 1;

  if (d == 0)
    return 0;

  return mpz_rem_ui(n, d) == 0;
}

int
mpz_divisible_2exp_p(mpz_srcptr n, mp_bits_t bits) {
  mp_size_t s = bits / MP_LIMB_BITS;
  mp_bits_t r;

  if (n->size == 0)
    return 1;

  if (s >= MP_ABS(n->size))
    return 0;

  r = bits % MP_LIMB_BITS;

  if (n->limbs[s] & MP_MASK(r))
    return 0;

  while (s--) {
    if (n->limbs[s] != 0)
      return 0;
  }

  return 1;
}

/*
 * Congruence
 */

int
mpz_congruent_p(mpz_srcptr x, mpz_srcptr y, mpz_srcptr d) {
  mpz_t t;
  int ret;

  if (d->size == 0)
    return mpz_cmp(x, y) == 0;

  mpz_init_vla(t, mpz_add_size(x, y));

  mpz_sub(t, x, y);

  ret = mpz_divisible_p(t, d);

  mpz_clear_vla(t);

  return ret;
}

int
mpz_congruent_ui_p(mpz_srcptr x, mpz_srcptr y, mp_limb_t d) {
  mpz_t t;
  int ret;

  if (d == 0)
    return mpz_cmp(x, y) == 0;

  mpz_init_vla(t, mpz_add_size(x, y));

  mpz_sub(t, x, y);

  ret = mpz_divisible_ui_p(t, d);

  mpz_clear_vla(t);

  return ret;
}

int
mpz_congruent_2exp_p(mpz_srcptr x, mpz_srcptr y, mp_bits_t bits) {
  mpz_t t;
  int ret;

  if (bits == 0)
    return 1;

  mpz_init_vla(t, mpz_add_size(x, y));

  mpz_sub(t, x, y);

  ret = mpz_divisible_2exp_p(t, bits);

  mpz_clear_vla(t);

  return ret;
}

/*
 * Exponentiation
 */

void
mpz_pow_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  mpz_t t, u;

  if (y == 0) {
    mpz_set_ui(z, 1);
    return;
  }

  if (y == 1) {
    mpz_set(z, x);
    return;
  }

  if (x->size == 0) {
    z->size = 0;
    return;
  }

  mpz_init(t);

  if ((y & (y - 1)) == 0) {
    mpz_set(z, x);

    y -= 1;

    while (y > 0) {
      mpz_sqr(t, z);
      mpz_swap(z, t);

      y >>= 1;
    }
  } else {
    mpz_init(u);

    mpz_set(u, x);
    mpz_set_ui(z, 1);

    while (y > 0) {
      if (y & 1) {
        mpz_mul(t, z, u);
        mpz_swap(z, t);
      }

      mpz_sqr(t, u);
      mpz_swap(u, t);

      y >>= 1;
    }

    mpz_clear(u);
  }

  mpz_clear(t);
}

void
mpz_ui_pow_ui(mpz_ptr z, mp_limb_t x, mp_limb_t y) {
  mpz_set_ui(z, x);
  mpz_pow_ui(z, z, y);
}

/*
 * Roots
 */

void
mpz_rootrem(mpz_ptr z, mpz_ptr r, mpz_srcptr x, mp_limb_t k) {
  /* Integer k-th root.
   *
   * [ARITH] Algorithm 1.14, Page 27, Section 1.5.2
   */
  mpz_t u, s, t;

  if (k == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (x->size < 0 && (k & 1) == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (x->size == 0) {
    if (z != NULL)
      z->size = 0;

    if (r != NULL)
      r->size = 0;

    return;
  }

  mpz_init(u);
  mpz_init(s);
  mpz_init(t);

  /* Algorithm:
   *
   *   u <- x (or any value >= floor(x^(1/k)))
   *
   *   repeat
   *     s <- u
   *     t <- (k - 1) * s + floor(x / s^(k - 1))
   *     u <- floor(t / k)
   *   until u >= s
   *
   *   return s
   */
  mpz_setbit(u, (mpz_bitlen(x) + k - 1) / k + 1);

  if (x->size < 0)
    mpz_neg(u, u);

  do {
    mpz_swap(s, u);
    mpz_pow_ui(t, s, k - 1);
    mpz_quo(u, x, t);
    mpz_mul_ui(t, s, k - 1);
    mpz_add(t, t, u);
    mpz_quo_ui(u, t, k);
  } while (mpz_cmpabs(u, s) < 0);

  if (r != NULL) {
    mpz_pow_ui(t, s, k);
    mpz_sub(r, x, t);
  }

  if (z != NULL)
    mpz_swap(z, s);

  mpz_clear(u);
  mpz_clear(s);
  mpz_clear(t);
}

int
mpz_root(mpz_ptr z, mpz_srcptr x, mp_limb_t k) {
  mpz_t r;
  int ret;

  if (z == NULL) {
    if (k == 0)
      return 0;

    if (x->size < 0 && (k & 1) == 0)
      return 0;
  }

  mpz_init(r);
  mpz_rootrem(z, r, x, k);

  ret = (r->size == 0);

  mpz_clear(r);

  return ret;
}

int
mpz_perfect_power_p(mpz_srcptr x) {
  mp_limb_t n = mpz_bitlen(x);
  mp_limb_t *sp;
  mp_size_t sn;
  mp_limb_t p;
  int ret = 1;

  if (n <= 1)
    return 1;

  if (mpz_perfect_square_p(x))
    return 1;

  /* Test prime exponents in [3,ceil(log2(x+1))]. */
  sn = mpn_sieve_size(n);
  sp = mp_alloc_vla(sn);

  mpn_sieve(sp, n);

  for (p = 3; p <= n; p += 2) {
    if (mpn_tstbit(sp, p)) {
      if (mpz_root(NULL, x, p))
        goto done;
    }
  }

  ret = 0;
done:
  mp_free_vla(sp, sn);
  return ret;
}

void
mpz_sqrtrem(mpz_ptr z, mpz_ptr r, mpz_srcptr x) {
  /* Integer Square Root.
   *
   * [ARITH] Algorithm 1.13, Page 27, Section 1.5
   */
  mpz_t u, s, t;

  if (x->size < 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (x->size == 0) {
    if (z != NULL)
      z->size = 0;

    if (r != NULL)
      r->size = 0;

    return;
  }

  mpz_init(u);
  mpz_init(s);
  mpz_init(t);

  /* Algorithm:
   *
   *   u <- x (or any value >= floor(x^(1/2)))
   *
   *   repeat
   *     s <- u
   *     t <- s + floor(x / s)
   *     u <- floor(t / 2)
   *   until u >= s
   *
   *   return s
   */
  mpz_setbit(u, (mpz_bitlen(x) + 1) / 2 + 1);

  do {
    mpz_swap(s, u);
    mpz_quo(t, x, s);
    mpz_add(t, s, t);
    mpz_quo_2exp(u, t, 1);
  } while (mpz_cmpabs(u, s) < 0);

  if (r != NULL) {
    mpz_sqr(t, s);
    mpz_sub(r, x, t);
  }

  if (z != NULL)
    mpz_swap(z, s);

  mpz_clear(u);
  mpz_clear(s);
  mpz_clear(t);
}

void
mpz_sqrt(mpz_ptr z, mpz_srcptr x) {
  mpz_sqrtrem(z, NULL, x);
}

int
mpz_perfect_square_p(mpz_srcptr x) {
  mpz_t r;
  int ret;

  if (x->size < 0)
    return 0;

  mpz_init(r);
  mpz_sqrtrem(NULL, r, x);

  ret = (r->size == 0);

  mpz_clear(r);

  return ret;
}

/*
 * AND
 */

void
mpz_and(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t yn = MP_ABS(y->size);
  const mp_limb_t *xp, *yp;
  mp_limb_t cx, cy, cz;
  mp_limb_t fx, fy;
  mp_limb_t zx, zy;
  mp_size_t i, zn;
  mp_limb_t *zp;

  if (xn < yn)
    MPZ_CSWAP(x, xn, y, yn);

  zn = xn;
  zp = mpz_grow(z, zn + 1);

  xp = x->limbs;
  yp = y->limbs;

  if ((x->size & y->size) < 0) {
    /* (-x) & (-y) == ~(x-1) & ~(y-1)
     *             == ~((x-1) | (y-1))
     *             == -(((x-1) | (y-1)) + 1)
     */
    cx = 1;
    cy = 1;
    cz = 1;

    for (i = 0; i < yn; i++) {
      mp_sub(zx, cx, xp[i], cx);
      mp_sub(zy, cy, yp[i], cy);

      zx |= zy;

      mp_add(zp[i], cz, zx, cz);
    }

    for (i = yn; i < xn; i++) {
      mp_sub(zx, cx, xp[i], cx);
      mp_add(zp[i], cz, zx, cz);
    }

    zp[zn++] = cz;
  } else if ((x->size | y->size) < 0) {
    /* x & (-y) == x & ~(y-1)
     * (-x) & y == y & ~(x-1)
     */
    cx = (x->size < 0);
    cy = (y->size < 0);

    fx = -cx;
    fy = -cy;

    for (i = 0; i < yn; i++) {
      mp_sub(zx, cx, xp[i], cx);
      mp_sub(zy, cy, yp[i], cy);

      zp[i] = (zx ^ fx) & (zy ^ fy);
    }

    for (i = yn; i < xn; i++) {
      mp_sub(zx, cx, xp[i], cx);

      zp[i] = (zx ^ fx) & fy;
    }
  } else {
    /* x & y == x & y */
    mpn_and_n(zp, xp, yp, yn);
    zn = yn;
  }

  zn = mpn_strip(zp, zn);

  z->size = (x->size & y->size) < 0 ? -zn : zn;
}

mp_limb_t
mpz_and_ui(mpz_srcptr x, mp_limb_t y) {
  if (x->size < 0) {
    /* (-x) & y == y & ~(x-1) */
    return y & ~(x->limbs[0] - 1);
  }

  if (x->size == 0)
    return 0;

  return x->limbs[0] & y;
}

void
mpz_and_si(mpz_ptr z, mpz_srcptr x, mp_long_t y) {
  mpz_t t;

  if (y < 0) {
    mpz_roinit_si(t, y);
    mpz_and(z, x, t);
  } else {
    mpz_set_ui(z, mpz_and_ui(x, y));
  }
}

/*
 * OR
 */

void
mpz_ior(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t yn = MP_ABS(y->size);
  const mp_limb_t *xp, *yp;
  mp_limb_t cx, cy, cz;
  mp_limb_t fx, fy;
  mp_limb_t zx, zy;
  mp_size_t i, zn;
  mp_limb_t *zp;

  if (xn < yn)
    MPZ_CSWAP(x, xn, y, yn);

  zn = xn;
  zp = mpz_grow(z, zn + 1);

  xp = x->limbs;
  yp = y->limbs;

  if ((x->size | y->size) < 0) {
    /* (-x) | (-y) == ~(x-1) | ~(y-1)
     *             == ~((x-1) & (y-1))
     *             == -(((x-1) & (y-1)) + 1)
     *
     * x | (-y) == x | ~(y-1)
     *          == ~((y-1) & ~x)
     *          == -(((y-1) & ~x) + 1)
     *
     * (-x) | y == y | ~(x-1)
     *          == ~((x-1) & ~y)
     *          == -(((x-1) & ~y) + 1)
     */
    cx = (x->size < 0);
    cy = (y->size < 0);
    cz = 1;

    fx = -(cx ^ 1);
    fy = -(cy ^ 1);

    fx &= -(cx ^ cy);
    fy &= -(cx ^ cy);

    for (i = 0; i < yn; i++) {
      mp_sub(zx, cx, xp[i], cx);
      mp_sub(zy, cy, yp[i], cy);

      zx = (zx ^ fx) & (zy ^ fy);

      mp_add(zp[i], cz, zx, cz);
    }

    for (i = yn; i < xn; i++) {
      mp_sub(zx, cx, xp[i], cx);

      zx = (zx ^ fx) & fy;

      mp_add(zp[i], cz, zx, cz);
    }

    zp[zn++] = cz;
  } else {
    /* x | y == x | y */
    mpn_ior_n(zp, xp, yp, yn);
    mpn_copyi(zp + yn, xp + yn, xn - yn);
  }

  zn = mpn_strip(zp, zn);

  z->size = (x->size | y->size) < 0 ? -zn : zn;
}

void
mpz_ior_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  mpz_t t;

  if (x->size < 0) {
    mpz_roinit_ui(t, y);
    mpz_ior(z, x, t);
  } else if (x->size == 0) {
    mpz_set_ui(z, y);
  } else {
    mpz_set(z, x);

    z->limbs[0] |= y;
  }
}

void
mpz_ior_si(mpz_ptr z, mpz_srcptr x, mp_long_t y) {
  if (y < 0) {
    mp_limb_t v = mp_limb_cast(y);
    mp_limb_t r;

    if (x->size < 0) {
      /* (-x) | (-y) == ~(x-1) | ~(y-1)
       *             == ~((x-1) & (y-1))
       *             == -(((x-1) & (y-1)) + 1)
       */
      r = ((x->limbs[0] - 1) & (v - 1)) + 1;
    } else if (x->size > 0) {
      /* x | (-y) == x | ~(y-1)
       *          == ~((y-1) & ~x)
       *          == -(((y-1) & ~x) + 1)
       */
      r = ((v - 1) & ~x->limbs[0]) + 1;
    } else {
      /* 0 | (-y) == -(y) */
      r = v;
    }

    mpz_set_ui(z, r);
    mpz_neg(z, z);
  } else {
    mpz_ior_ui(z, x, y);
  }
}

/*
 * XOR
 */

void
mpz_xor(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t yn = MP_ABS(y->size);
  const mp_limb_t *xp, *yp;
  mp_limb_t cx, cy, cz;
  mp_limb_t zx, zy;
  mp_size_t i, zn;
  mp_limb_t *zp;

  if (xn < yn)
    MPZ_CSWAP(x, xn, y, yn);

  zn = xn;
  zp = mpz_grow(z, zn + 1);

  xp = x->limbs;
  yp = y->limbs;

  if ((x->size | y->size) < 0) {
    /* (-x) ^ (-y) == ~(x-1) ^ ~(y-1)
     *             == (x-1) ^ (y-1)
     *
     * x ^ (-y) == x ^ ~(y-1)
     *          == ~(x ^ (y-1))
     *          == -((x ^ (y-1)) + 1)
     *
     * (-x) ^ y == y ^ ~(x-1)
     *          == ~(y ^ (x-1))
     *          == -((y ^ (x-1)) + 1)
     */
    cx = (x->size < 0);
    cy = (y->size < 0);
    cz = cx ^ cy;

    for (i = 0; i < yn; i++) {
      mp_sub(zx, cx, xp[i], cx);
      mp_sub(zy, cy, yp[i], cy);

      zx ^= zy;

      mp_add(zp[i], cz, zx, cz);
    }

    for (i = yn; i < xn; i++) {
      mp_sub(zx, cx, xp[i], cx);
      mp_add(zp[i], cz, zx, cz);
    }

    zp[zn++] = cz;
  } else {
    /* x ^ y == x ^ y */
    mpn_xor_n(zp, xp, yp, yn);
    mpn_copyi(zp + yn, xp + yn, xn - yn);
  }

  zn = mpn_strip(zp, zn);

  z->size = (x->size ^ y->size) < 0 ? -zn : zn;
}

void
mpz_xor_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  mpz_t t;

  if (x->size < 0) {
    mpz_roinit_ui(t, y);
    mpz_xor(z, x, t);
  } else if (x->size == 0) {
    mpz_set_ui(z, y);
  } else {
    mpz_set(z, x);

    z->limbs[0] ^= y;
    z->size -= (z->limbs[z->size - 1] == 0);
  }
}

void
mpz_xor_si(mpz_ptr z, mpz_srcptr x, mp_long_t y) {
  mpz_t t;

  if (y < 0) {
    mpz_roinit_si(t, y);
    mpz_xor(z, x, t);
  } else {
    mpz_xor_ui(z, x, y);
  }
}

/*
 * NOT
 */

void
mpz_com(mpz_ptr z, mpz_srcptr x) {
  if (x->size < 0) {
    /* ~(-x) == ~(~(x-1)) == x-1 */
    mpz_neg(z, x);
    mpz_sub_ui(z, z, 1);
  } else {
    /* ~x == -x-1 == -(x+1) */
    mpz_add_ui(z, x, 1);
    mpz_neg(z, z);
  }
}

/*
 * Left Shift
 */

void
mpz_mul_2exp(mpz_ptr z, mpz_srcptr x, mp_bits_t bits) {
  const mp_limb_t *xp;
  mp_size_t xn, zn, s;
  mp_limb_t *zp;
  mp_bits_t r;

  if (x->size == 0) {
    z->size = 0;
    return;
  }

  if (bits == 0) {
    mpz_set(z, x);
    return;
  }

  s = bits / MP_LIMB_BITS;
  r = bits % MP_LIMB_BITS;

  xn = MP_ABS(x->size);
  zn = xn + s;

  zp = mpz_grow(z, zn + (r != 0));
  xp = x->limbs;

  if (r != 0) {
    zp[zn] = mpn_lshift(zp + s, xp, xn, r);
    zn += (zp[zn] != 0);
  } else if (s != 0 || zp != xp) {
    mpn_copyd(zp + s, xp, xn);
  }

  mpn_zero(zp, s);

  z->size = x->size < 0 ? -zn : zn;
}

/*
 * Unsigned Right Shift
 */

void
mpz_quo_2exp(mpz_ptr z, mpz_srcptr x, mp_bits_t bits) {
  const mp_limb_t *xp;
  mp_size_t xn, zn, s;
  mp_limb_t *zp;
  mp_bits_t r;

  if (x->size == 0) {
    z->size = 0;
    return;
  }

  if (bits == 0) {
    mpz_set(z, x);
    return;
  }

  s = bits / MP_LIMB_BITS;
  r = bits % MP_LIMB_BITS;

  xn = MP_ABS(x->size);
  zn = xn - s;

  if (zn <= 0) {
    z->size = 0;
    return;
  }

  zp = mpz_grow(z, zn);
  xp = x->limbs;

  if (r != 0) {
    mpn_rshift(zp, xp + s, zn, r);
    zn -= (zp[zn - 1] == 0);
  } else if (s != 0 || zp != xp) {
    mpn_copyi(zp, xp + s, zn);
  }

  z->size = x->size < 0 ? -zn : zn;
}

void
mpz_rem_2exp(mpz_ptr z, mpz_srcptr x, mp_bits_t bits) {
  const mp_limb_t *xp;
  mp_size_t xn, zn;
  mp_limb_t *zp;
  mp_bits_t lo;

  if (x->size == 0 || bits == 0) {
    z->size = 0;
    return;
  }

  xn = MP_ABS(x->size);
  zn = (bits + MP_LIMB_BITS - 1) / MP_LIMB_BITS;
  lo = bits % MP_LIMB_BITS;

  /* (-x) mod y == -(x & (y-1))
   *
   * x mod y == x & (y-1)
   */
  if (zn > xn) {
    zn = xn;
    lo = 0;
  }

  zp = mpz_grow(z, zn);
  xp = x->limbs;

  if (zp != xp)
    mpn_copyi(zp, xp, zn);

  if (lo != 0)
    zp[zn - 1] &= MP_MASK(lo);

  zn = mpn_strip(zp, zn);

  z->size = x->size < 0 ? -zn : zn;
}

/*
 * Right Shift
 */

void
mpz_div_2exp(mpz_ptr z, mpz_srcptr x, mp_bits_t bits) {
  if (x->size < 0) {
    /* (-x) >> y == ~(x-1) >> y
     *           == ~((x-1) >> y)
     *           == -(((x-1) >> y) + 1)
     */
    mpz_neg(z, x);
    mpz_sub_ui(z, z, 1);
    mpz_quo_2exp(z, z, bits);
    mpz_add_ui(z, z, 1);
    mpz_neg(z, z);
  } else {
    /* x >> y == x >> y */
    mpz_quo_2exp(z, x, bits);
  }
}

void
mpz_mod_2exp(mpz_ptr z, mpz_srcptr x, mp_bits_t bits) {
  const mp_limb_t *xp;
  mp_size_t xn, zn;
  mp_limb_t *zp;
  mp_bits_t lo;

  if (x->size == 0 || bits == 0) {
    z->size = 0;
    return;
  }

  xn = MP_ABS(x->size);
  zn = (bits + MP_LIMB_BITS - 1) / MP_LIMB_BITS;
  lo = bits % MP_LIMB_BITS;

  if (x->size < 0) {
    /* (-x) mod y == (-x) & (y-1)
     *            == (y-1) & ~(x-1)
     */
    zp = mpz_grow(z, zn);
    xp = x->limbs;

    if (zn > xn) {
      mpn_sub_1(zp, xp, xn, 1);
      mpn_zero(zp + xn, zn - xn);
    } else {
      mpn_sub_1(zp, xp, zn, 1);
    }

    mpn_com(zp, zp, zn);
  } else {
    /* x mod y == x & (y-1) */
    if (zn > xn) {
      zn = xn;
      lo = 0;
    }

    zp = mpz_grow(z, zn);
    xp = x->limbs;

    if (zp != xp)
      mpn_copyi(zp, xp, zn);
  }

  if (lo != 0)
    zp[zn - 1] &= MP_MASK(lo);

  z->size = mpn_strip(zp, zn);
}

/*
 * Bit Manipulation
 */

int
mpz_tstbit(mpz_srcptr x, mp_bits_t pos) {
  mp_size_t s = pos / MP_LIMB_BITS;
  int b;

  if (s >= MP_ABS(x->size))
    return x->size < 0;

  b = (x->limbs[s] >> (pos % MP_LIMB_BITS)) & 1;

  if (x->size < 0)
    b ^= !mpz_divisible_2exp_p(x, pos);

  return b;
}

static void
mpz_setbit_abs(mpz_ptr z, mp_bits_t pos) {
  mp_size_t s = pos / MP_LIMB_BITS;
  mp_bits_t r = pos % MP_LIMB_BITS;
  mp_size_t zn = MP_ABS(z->size);
  mp_limb_t *zp = z->limbs;

  if (zn < s + 1) {
    zp = mpz_grow(z, s + 1);

    while (zn < s + 1)
      zp[zn++] = 0;

    z->size = z->size < 0 ? -zn : zn;
  }

  zp[s] |= MP_LIMB_C(1) << r;
}

static void
mpz_clrbit_abs(mpz_ptr z, mp_bits_t pos) {
  mp_size_t s = pos / MP_LIMB_BITS;
  mp_bits_t r = pos % MP_LIMB_BITS;
  mp_size_t zn = MP_ABS(z->size);
  mp_limb_t *zp = z->limbs;

  if (s < zn) {
    zp[s] &= ~(MP_LIMB_C(1) << r);

    zn = mpn_strip(zp, zn);

    z->size = z->size < 0 ? -zn : zn;
  }
}

void
mpz_setbit(mpz_ptr z, mp_bits_t pos) {
  if (z->size < 0 && !mpz_divisible_2exp_p(z, pos))
    mpz_clrbit_abs(z, pos);
  else
    mpz_setbit_abs(z, pos);
}

void
mpz_clrbit(mpz_ptr z, mp_bits_t pos) {
  if (z->size < 0 && !mpz_divisible_2exp_p(z, pos))
    mpz_setbit_abs(z, pos);
  else
    mpz_clrbit_abs(z, pos);
}

void
mpz_combit(mpz_ptr z, mp_bits_t pos) {
  if (!mpz_tstbit(z, pos))
    mpz_setbit(z, pos);
  else
    mpz_clrbit(z, pos);
}

mp_bits_t
mpz_scan0(mpz_srcptr x, mp_bits_t pos) {
  const mp_limb_t *xp = x->limbs;
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t s = pos / MP_LIMB_BITS;

  if (s >= xn)
    return x->size < 0 ? MP_BITS_MAX : pos;

  if (x->size < 0)
    return mpn_scan(xp, xn, 1, mpn_zero_p(xp, s), pos);

  return mpn_scan(xp, xn, 0, 0, pos);
}

mp_bits_t
mpz_scan1(mpz_srcptr x, mp_bits_t pos) {
  const mp_limb_t *xp = x->limbs;
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t s = pos / MP_LIMB_BITS;

  if (s >= xn)
    return x->size < 0 ? pos : MP_BITS_MAX;

  if (x->size < 0)
    return mpn_scan(xp, xn, 0, mpn_zero_p(xp, s), pos);

  return mpn_scan(xp, xn, 1, 0, pos);
}

mp_bits_t
mpz_popcount(mpz_srcptr x) {
  if (x->size < 0)
    return MP_BITS_MAX;

  return mpn_popcount(x->limbs, x->size);
}

mp_bits_t
mpz_hamdist(mpz_srcptr x, mpz_srcptr y) {
  const mp_limb_t *xp, *yp;
  mp_size_t i, xn, yn;
  mp_bits_t cnt = 0;
  mp_limb_t cx, cy;
  mp_limb_t zx, zy;

  if ((x->size ^ y->size) < 0)
    return MP_BITS_MAX;

  xn = MP_ABS(x->size);
  yn = MP_ABS(y->size);

  if (xn < yn)
    MPZ_CSWAP(x, xn, y, yn);

  xp = x->limbs;
  yp = y->limbs;

  if (x->size < 0) {
    cx = 1;
    cy = 1;

    for (i = 0; i < yn; i++) {
      mp_sub(zx, cx, xp[i], cx);
      mp_sub(zy, cy, yp[i], cy);

      cnt += mp_popcount(zx ^ zy);
    }

    for (i = yn; i < xn; i++) {
      mp_sub(zx, cx, xp[i], cx);

      cnt += mp_popcount(zx);
    }
  } else {
    cnt += mpn_hamdist(xp, yp, yn);
    cnt += mpn_popcount(xp + yn, xn - yn);
  }

  return cnt;
}

/*
 * Negation
 */

void
mpz_abs(mpz_ptr z, mpz_srcptr x) {
  mpz_set(z, x);
  z->size = MP_ABS(z->size);
}

void
mpz_neg(mpz_ptr z, mpz_srcptr x) {
  mpz_set(z, x);
  z->size = -z->size;
}

/*
 * Number Theoretic Functions
 */

void
mpz_gcd(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mp_size_t xn, yn, zn, itch;
  mp_limb_t *zp, *scratch;

  if (x->size == 0) {
    mpz_abs(z, y);
    return;
  }

  if (y->size == 0) {
    mpz_abs(z, x);
    return;
  }

  xn = MP_ABS(x->size);
  yn = MP_ABS(y->size);

  if (xn < yn)
    MPZ_CSWAP(x, xn, y, yn);

  zn = yn;
  zp = mpz_grow(z, zn);

  itch = MPN_GCD_ITCH(xn, yn);
  scratch = mp_alloc_vla(itch);

  zn = mpn_gcd(zp, x->limbs, xn,
                   y->limbs, yn,
                   scratch);

  mp_free_vla(scratch, itch);

  z->size = zn;
}

mp_limb_t
mpz_gcd_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  mp_limb_t *scratch;
  mp_size_t xn, itch;
  mp_limb_t g;

  if (x->size == 0) {
    if (z != NULL)
      mpz_set_ui(z, y);

    return y;
  }

  xn = MP_ABS(x->size);

  if (y == 0) {
    if (z != NULL)
      mpz_abs(z, x);

    if (xn != 1)
      return 0;

    return x->limbs[0];
  }

  itch = MPN_GCD_1_ITCH(xn);
  scratch = mp_alloc_vla(itch);

  g = mpn_gcd_1(x->limbs, xn, y, scratch);

  mp_free_vla(scratch, itch);

  if (z != NULL)
    mpz_set_ui(z, g);

  return g;
}

void
mpz_lcm(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mpz_t t;

  if (x->size == 0 || y->size == 0) {
    z->size = 0;
    return;
  }

  mpz_init_vla(t, mpz_add_size(x, y));

  mpz_gcd(t, x, y);
  mpz_quo(t, x, t);
  mpz_mul(z, y, t);
  mpz_abs(z, z);

  mpz_clear_vla(t);
}

void
mpz_lcm_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y) {
  if (x->size == 0 || y == 0) {
    z->size = 0;
    return;
  }

  mpz_mul_ui(z, x, y / mpz_gcd_ui(NULL, x, y));
  mpz_abs(z, z);
}

void
mpz_gcdext(mpz_ptr g, mpz_ptr s, mpz_ptr t, mpz_srcptr x, mpz_srcptr y) {
  /* Euclid's algorithm for large numbers.
   *
   * [KNUTH] Algorithm L, Page 347, Section 4.5.2.
   */
  mpz_t u, v, A, B, C, D, up, vp;
  mp_bits_t uz, vz, shift;

  if (x->size == 0) {
    if (g != NULL)
      mpz_abs(g, y);

    if (s != NULL)
      s->size = 0;

    if (t != NULL)
      mpz_set_si(t, mpz_sgn(y));

    return;
  }

  if (y->size == 0) {
    if (g != NULL)
      mpz_abs(g, x);

    if (s != NULL)
      mpz_set_si(s, mpz_sgn(x));

    if (t != NULL)
      t->size = 0;

    return;
  }

  mpz_init(u);
  mpz_init(v);
  mpz_init(A);
  mpz_init(B);
  mpz_init(C);
  mpz_init(D);
  mpz_init(up);
  mpz_init(vp);

  mpz_abs(u, x);
  mpz_abs(v, y);

  /* A * u + B * v = u */
  mpz_set_ui(A, 1);
  mpz_set_ui(B, 0);

  /* C * u + D * v = v */
  mpz_set_ui(C, 0);
  mpz_set_ui(D, 1);

  uz = mpz_ctz(u);
  vz = mpz_ctz(v);

  shift = MP_MIN(uz, vz);

  mpz_quo_2exp(u, u, shift);
  mpz_quo_2exp(v, v, shift);

  mpz_set(up, u);
  mpz_set(vp, v);

  while (u->size != 0) {
    uz = mpz_ctz(u);
    vz = mpz_ctz(v);

    mpz_quo_2exp(u, u, uz);
    mpz_quo_2exp(v, v, vz);

    while (uz--) {
      if (mpz_odd_p(A) || mpz_odd_p(B)) {
        mpz_add(A, A, vp);
        mpz_sub(B, B, up);
      }

      mpz_quo_2exp(A, A, 1);
      mpz_quo_2exp(B, B, 1);
    }

    while (vz--) {
      if (mpz_odd_p(C) || mpz_odd_p(D)) {
        mpz_add(C, C, vp);
        mpz_sub(D, D, up);
      }

      mpz_quo_2exp(C, C, 1);
      mpz_quo_2exp(D, D, 1);
    }

    if (mpz_cmpabs(u, v) >= 0) {
      mpz_sub(u, u, v);
      mpz_sub(A, A, C);
      mpz_sub(B, B, D);
    } else {
      mpz_sub(v, v, u);
      mpz_sub(C, C, A);
      mpz_sub(D, D, B);
    }
  }

  if (x->size < 0)
    mpz_neg(C, C);

  if (y->size < 0)
    mpz_neg(D, D);

  if (g != NULL)
    mpz_mul_2exp(g, v, shift);

  if (s != NULL)
    mpz_swap(s, C);

  if (t != NULL)
    mpz_swap(t, D);

  mpz_clear(u);
  mpz_clear(v);
  mpz_clear(A);
  mpz_clear(B);
  mpz_clear(C);
  mpz_clear(D);
  mpz_clear(up);
  mpz_clear(vp);
}

static int
mpz_invert_inner(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t yn = MP_ABS(y->size);
  mp_limb_t *zp = mpz_grow(z, yn);
  mp_size_t itch = MPN_INVERT_ITCH(yn);
  mp_limb_t *scratch = mp_alloc_vla(itch);
  int ret;

  ret = mpn_invert(zp, x->limbs, xn,
                       y->limbs, yn,
                       scratch);

  z->size = mpn_strip(zp, yn);

  mp_free_vla(scratch, itch);

  return ret;
}

int
mpz_invert(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mpz_t t, g, s;
  int ret;

  if (x->size == 0 || y->size == 0) {
    z->size = 0;
    return 0;
  }

  if (mpz_cmpabs_ui(y, 1) == 0) {
    z->size = 0;
    return 0;
  }

  if (mpz_odd_p(y)) {
    if (x->size < 0 || mpz_cmpabs(x, y) >= 0) {
      mpz_init_vla(t, MP_ABS(y->size) + 1);
      mpz_mod(t, x, y);

      ret = mpz_invert_inner(z, t, y);

      mpz_clear_vla(t);
    } else {
      ret = mpz_invert_inner(z, x, y);
    }
  } else {
    mpz_init(g);
    mpz_init(s);

    mpz_gcdext(g, s, NULL, x, y);

    ret = (mpz_cmp_ui(g, 1) == 0);

    if (ret) {
      mpz_mod(s, s, y);
      mpz_swap(z, s);
    } else {
      z->size = 0;
    }

    mpz_clear(g);
    mpz_clear(s);
  }

  return ret;
}

int
mpz_legendre(mpz_srcptr x, mpz_srcptr p) {
  if (p->size < 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  return mpz_jacobi(x, p);
}

static int
mpz_jacobi_inner(mpz_srcptr x, mpz_srcptr y) {
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t yn = MP_ABS(y->size);
  mp_size_t itch = MPN_JACOBI_ITCH(yn);
  mp_limb_t *scratch = mp_alloc_vla(itch);
  int j;

  j = mpn_jacobi(x->limbs, xn, y->limbs, yn, scratch);

  mp_free_vla(scratch, itch);

  return j;
}

int
mpz_jacobi(mpz_srcptr x, mpz_srcptr y) {
  mpz_t t;
  int j;

  if (y->size == 0 || (y->limbs[0] & 1) == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (x->size < 0 || mpz_cmpabs(x, y) >= 0) {
    mpz_init_vla(t, MP_ABS(y->size) + 1);
    mpz_mod(t, x, y);

    j = mpz_jacobi_inner(t, y);

    mpz_clear_vla(t);
  } else {
    j = mpz_jacobi_inner(x, y);
  }

  if ((x->size & y->size) < 0)
    j = -j;

  return j;
}

int
mpz_kronecker(mpz_srcptr x, mpz_srcptr y) {
  static const int table[8] = {0, 1, 0, -1, 0, -1, 0, 1};
  mp_bits_t bits;
  mpz_t t;
  int k;

  if (x->size == 0)
    return mpz_cmpabs_ui(y, 1) == 0;

  if (y->size == 0)
    return mpz_cmpabs_ui(x, 1) == 0;

  if (((x->limbs[0] | y->limbs[0]) & 1) == 0)
    return 0;

  bits = mpz_ctz(y);

  if (bits > 0) {
    mpz_init_vla(t, MP_ABS(y->size));
    mpz_quo_2exp(t, y, bits);

    k = mpz_jacobi(x, t);

    if (bits & 1)
      k *= table[x->limbs[0] & 7];

    mpz_clear_vla(t);
  } else {
    k = mpz_jacobi(x, y);
  }

  return k;
}

int
mpz_kronecker_ui(mpz_srcptr x, mp_limb_t y) {
  mpz_t t;

  mpz_roinit_ui(t, y);

  return mpz_kronecker(x, t);
}

int
mpz_kronecker_si(mpz_srcptr x, mp_long_t y) {
  mpz_t t;

  mpz_roinit_si(t, y);

  return mpz_kronecker(x, t);
}

int
mpz_ui_kronecker(mp_limb_t x, mpz_srcptr y) {
  mpz_t t;

  mpz_roinit_ui(t, x);

  return mpz_kronecker(t, y);
}

int
mpz_si_kronecker(mp_long_t x, mpz_srcptr y) {
  mpz_t t;

  mpz_roinit_si(t, x);

  return mpz_kronecker(t, y);
}

static void
mpz_powm_inner(mpz_ptr z, mpz_srcptr x, mpz_srcptr y, mpz_srcptr m) {
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t yn = MP_ABS(y->size);
  mp_size_t mn = MP_ABS(m->size);
  mp_limb_t *zp = mpz_grow(z, mn);
  mp_size_t itch = MPN_POWM_ITCH(yn, mn);
  mp_limb_t *scratch = mp_alloc_limbs(itch);

  mpn_powm(zp, x->limbs, xn,
               y->limbs, yn,
               m->limbs, mn,
               scratch);

  z->size = mpn_strip(zp, mn);

  mp_free_limbs(scratch);
}

void
mpz_powm(mpz_ptr z, mpz_srcptr x, mpz_srcptr y, mpz_srcptr m) {
  mpz_t t;

  if (m->size == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (y->size < 0) {
    mpz_init(t);

    if (!mpz_invert(t, x, m))
      btc_abort(); /* LCOV_EXCL_LINE */

    mpz_powm_inner(z, t, y, m);
    mpz_clear(t);
  } else if (x->size < 0 || mpz_cmpabs(x, m) >= 0) {
    mpz_init_vla(t, MP_ABS(m->size) + 1);
    mpz_mod(t, x, m);
    mpz_powm_inner(z, t, y, m);
    mpz_clear_vla(t);
  } else {
    mpz_powm_inner(z, x, y, m);
  }
}

void
mpz_powm_ui(mpz_ptr z, mpz_srcptr x, mp_limb_t y, mpz_srcptr m) {
  mpz_t t;

  mpz_roinit_ui(t, y);
  mpz_powm(z, x, t, m);
}

static void
mpz_powm_sec_inner(mpz_ptr z, mpz_srcptr x, mpz_srcptr y, mpz_srcptr m) {
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t yn = MP_ABS(y->size);
  mp_size_t mn = MP_ABS(m->size);
  mp_limb_t *zp = mpz_grow(z, mn);
  mp_size_t itch = MPN_SEC_POWM_ITCH(mn);
  mp_limb_t *scratch = mp_alloc_limbs(itch);

  mpn_sec_powm(zp, x->limbs, xn,
                   y->limbs, yn,
                   m->limbs, mn,
                   scratch);

  z->size = mpn_strip(zp, mn);

  mp_free_limbs(scratch);
}

void
mpz_powm_sec(mpz_ptr z, mpz_srcptr x, mpz_srcptr y, mpz_srcptr m) {
  mpz_t t;

  if (y->size < 0 || m->size == 0 || (m->limbs[0] & 1) == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (x->size < 0 || mpz_cmpabs(x, m) >= 0) {
    mpz_init_vla(t, MP_ABS(m->size) + 1);
    mpz_mod(t, x, m);
    mpz_powm_sec_inner(z, t, y, m);
    mpz_clear_vla(t);
  } else {
    mpz_powm_sec_inner(z, x, y, m);
  }
}

static int
mpz_sqrtm_3mod4(mpz_ptr z, mpz_srcptr x, mpz_srcptr p) {
  /* Square Root (p = 3 mod 4). */
  mpz_t e, b, c;
  int ret = 0;

  mpz_init(e);
  mpz_init(b);
  mpz_init(c);

  /* b = x^((p + 1) / 4) mod p */
  mpz_add_ui(e, p, 1);
  mpz_quo_2exp(e, e, 2);
  mpz_powm(b, x, e, p);

  /* c = b^2 mod p */
  mpz_sqr(c, b);
  mpz_mod(c, c, p);

  /* c != x */
  if (mpz_cmp(c, x) != 0)
    goto fail;

  /* z = b */
  mpz_swap(z, b);

  ret = 1;
fail:
  mpz_clear(e);
  mpz_clear(b);
  mpz_clear(c);
  return ret;
}

static int
mpz_sqrtm_5mod8(mpz_ptr z, mpz_srcptr x, mpz_srcptr p) {
  /* Atkin's Algorithm (p = 5 mod 8). */
  mpz_t t, e, a, b, c;
  int ret = 0;

  mpz_init(t);
  mpz_init(e);
  mpz_init(a);
  mpz_init(b);
  mpz_init(c);

  /* t = x * 2 mod p */
  mpz_mul_2exp(t, x, 1);
  mpz_mod(t, t, p);

  /* a = t^((p - 5) / 8) mod p */
  mpz_quo_2exp(e, p, 3);
  mpz_powm(a, t, e, p);

  /* b = (a^2 * t - 1) * x * a mod p */
  mpz_sqr(c, a);
  mpz_mod(b, c, p);
  mpz_mul(c, b, t);
  mpz_mod(b, c, p);
  mpz_sub_ui(b, b, 1);
  mpz_mul(c, b, x);
  mpz_mod(b, c, p);
  mpz_mul(c, b, a);
  mpz_mod(b, c, p);

  /* c = b^2 mod p */
  mpz_sqr(c, b);
  mpz_mod(c, c, p);

  /* c != x */
  if (mpz_cmp(c, x) != 0)
    goto fail;

  /* z = b */
  mpz_swap(z, b);

  ret = 1;
fail:
  mpz_clear(t);
  mpz_clear(e);
  mpz_clear(a);
  mpz_clear(b);
  mpz_clear(c);
  return ret;
}

static int
mpz_sqrtm_0(mpz_ptr z, mpz_srcptr x, mpz_srcptr p) {
  /* Tonelli-Shanks (p = 1 mod 16). */
  mpz_t t, s, n, y, b, g;
  mp_bits_t i, e, k, m;
  int ret = 0;

  mpz_init(t);
  mpz_init(s);
  mpz_init(n);
  mpz_init(y);
  mpz_init(b);
  mpz_init(g);

  /* if p == 1 */
  if (mpz_cmp_ui(p, 1) == 0)
    goto fail;

  switch (mpz_jacobi(x, p)) {
    case -1:
      goto fail;
    case 0:
      z->size = 0;
      goto succeed;
    case 1:
      break;
  }

  /* s = p - 1 */
  mpz_sub_ui(s, p, 1);

  /* e = s factors of 2 */
  e = mpz_ctz(s);

  /* s = s >> e */
  mpz_quo_2exp(s, s, e);

  /* n = 2 */
  mpz_set_ui(n, 2);

  /* while n^((p - 1) / 2) != -1 mod p */
  while (mpz_jacobi(n, p) != -1) {
    /* n = n + 1 */
    mpz_add_ui(n, n, 1);
  }

  /* y = x^((s + 1) / 2) mod p */
  mpz_add_ui(t, s, 1);
  mpz_quo_2exp(t, t, 1);
  mpz_powm(y, x, t, p);

  /* b = x^s mod p */
  mpz_powm(b, x, s, p);

  /* g = n^s mod p */
  mpz_powm(g, n, s, p);

  /* k = e */
  k = e;

  for (;;) {
    /* t = b */
    mpz_set(t, b);

    /* m = 0 */
    m = 0;

    /* while t != 1 */
    while (mpz_cmp_ui(t, 1) != 0) {
      /* t = t^2 mod p */
      mpz_sqr(s, t);
      mpz_mod(t, s, p);
      m += 1;
    }

    /* if m == 0 */
    if (m == 0)
      break;

    /* if m >= k */
    if (m >= k)
      goto fail;

    /* t = g^(2^(k - m - 1)) mod p */
    mpz_swap(t, g);

    for (i = 0; i < k - m - 1; i++) {
      mpz_sqr(s, t);
      mpz_mod(t, s, p);
    }

    /* g = t^2 mod p */
    mpz_sqr(s, t);
    mpz_mod(g, s, p);

    /* y = y * t mod p */
    mpz_mul(s, y, t);
    mpz_mod(y, s, p);

    /* b = b * g mod p */
    mpz_mul(s, b, g);
    mpz_mod(b, s, p);

    /* k = m */
    k = m;
  }

  /* z = y */
  mpz_swap(z, y);
succeed:
  ret = 1;
fail:
  mpz_clear(t);
  mpz_clear(s);
  mpz_clear(n);
  mpz_clear(y);
  mpz_clear(b);
  mpz_clear(g);
  return ret;
}

int
mpz_sqrtm(mpz_ptr z, mpz_srcptr x, mpz_srcptr p) {
  /* Compute x^(1 / 2) in F(p). */
  mpz_t t;
  int ret;

  if (p->size <= 0 || mpz_even_p(p)) {
    z->size = 0;
    return 0;
  }

  mpz_init(t);
  mpz_mod(t, x, p);

  if ((p->limbs[0] & 3) == 3)
    ret = mpz_sqrtm_3mod4(z, t, p);
  else if ((p->limbs[0] & 7) == 5)
    ret = mpz_sqrtm_5mod8(z, t, p);
  else
    ret = mpz_sqrtm_0(z, t, p);

  mpz_clear(t);

  if (!ret)
    z->size = 0;

  return ret;
}

int
mpz_sqrtpq(mpz_ptr z, mpz_srcptr x, mpz_srcptr p, mpz_srcptr q) {
  /* Compute x^(1 / 2) in F(p * q). */
  mpz_t sp, sq, mp, mq, n, t, u, v;
  int ret = 0;

  mpz_init(sp);
  mpz_init(sq);
  mpz_init(mp);
  mpz_init(mq);
  mpz_init(n);
  mpz_init(t);
  mpz_init(u);
  mpz_init(v);

  /* sp = x^(1 / 2) in F(p) */
  if (!mpz_sqrtm(sp, x, p))
    goto fail;

  /* sq = x^(1 / 2) in F(q) */
  if (!mpz_sqrtm(sq, x, q))
    goto fail;

  /* (mp, mq) = bezout coefficients for egcd(p, q) */
  mpz_gcdext(NULL, mp, mq, p, q);

  /* n = p * q */
  mpz_mul(n, p, q);

  /* u = sq * mp * p mod n */
  mpz_mul(t, sq, mp);
  mpz_mod(u, t, n);
  mpz_mul(t, u, p);
  mpz_mod(u, t, n);

  /* v = sp * mq * q mod n */
  mpz_mul(t, sp, mq);
  mpz_mod(v, t, n);
  mpz_mul(t, v, q);
  mpz_mod(v, t, n);

  /* z = u + v mod n */
  mpz_add(t, u, v);
  mpz_mod(z, t, n);

  ret = 1;
fail:
  mpz_clear(sp);
  mpz_clear(sq);
  mpz_clear(mp);
  mpz_clear(mq);
  mpz_clear(n);
  mpz_clear(t);
  mpz_clear(u);
  mpz_clear(v);
  return ret;
}

mp_bits_t
mpz_remove(mpz_ptr z, mpz_srcptr x, mpz_srcptr y) {
  mp_size_t xn = MP_ABS(x->size);
  mp_size_t yn = MP_ABS(y->size);
  mp_bits_t c = 0;
  mp_bits_t b, s;
  mpz_t n, q, r;
  mp_limb_t w;

  if (yn == 0)
    btc_abort(); /* LCOV_EXCL_LINE */

  w = y->limbs[0];

  if (xn == 0 || (yn == 1 && w == 1)) {
    if (z != NULL)
      mpz_set(z, x);

    return 0;
  }

  if (yn == 1 && (w & (w - 1)) == 0) {
    b = mp_bitlen(w - 1);
    c = mpz_ctz(x) / b;

    if (z != NULL) {
      s = (y->size < 0);

      mpz_quo_2exp(z, x, c * b);

      if (c & s)
        mpz_neg(z, z);
    }

    return c;
  }

  mpz_init_vla(n, xn);
  mpz_init_vla(q, xn);
  mpz_init_vla(r, yn);

  mpz_set(n, x);

  while (n->size != 0) {
    mpz_quorem(q, r, n, y);

    if (r->size != 0)
      break;

    mpz_swap(n, q);

    c += 1;
  }

  if (z != NULL)
    mpz_set(z, n);

  mpz_clear_vla(n);
  mpz_clear_vla(q);
  mpz_clear_vla(r);

  return c;
}

void
mpz_fac_ui(mpz_ptr z, mp_limb_t n) {
  mpz_mfac_uiui(z, n, 1);
}

void
mpz_2fac_ui(mpz_ptr z, mp_limb_t n) {
  mpz_mfac_uiui(z, n, 2);
}

void
mpz_mfac_uiui(mpz_ptr z, mp_limb_t n, mp_limb_t m) {
  if (n == 0) {
    mpz_set_ui(z, 1);
    return;
  }

  mpz_set_ui(z, n);

  if (m == 0)
    return;

  while (n > m) {
    n -= m;
    mpz_mul_ui(z, z, n);
  }
}

void
mpz_primorial_ui(mpz_ptr z, mp_limb_t n) {
  mp_limb_t *sp;
  mp_size_t sn;
  mp_limb_t p;

  if (n < 2) {
    mpz_set_ui(z, 1);
    return;
  }

  sn = mpn_sieve_size(n);
  sp = mp_alloc_vla(sn);

  mpn_sieve(sp, n);

  mpz_set_ui(z, 2);

  for (p = 3; p <= n; p += 2) {
    if (mpn_tstbit(sp, p))
      mpz_mul_ui(z, z, p);
  }

  mp_free_vla(sp, sn);
}

void
mpz_bin_ui(mpz_ptr z, mpz_srcptr n, mp_limb_t k) {
  /* bin(n, k) = n! / (k! * (n - k)!) */
  int neg = 0;
  mp_limb_t i;
  mpz_t m, t;

  mpz_init(m);
  mpz_init(t);

  /* bin(-n, k) = (-1)^k * bin(n + k - 1, k) */
  if (n->size < 0) {
    mpz_neg(m, n);
    mpz_add_ui(m, m, k);
    mpz_sub_ui(m, m, 1);
    neg = (k & 1);
  } else {
    mpz_set(m, n);
  }

  if (mpz_cmp_ui(m, k) < 0) {
    z->size = 0;
    goto done;
  }

  /* bin(n, k) = bin(n, n - k) */
  mpz_sub_ui(t, m, k);

  if (mpz_cmp_ui(t, k) < 0)
    k = mpz_get_ui(t);

  mpz_set_ui(z, 1);

  for (i = 0; i < k; i++) {
    mpz_mul(t, z, m);
    mpz_quo_ui(z, t, i + 1);
    mpz_sub_ui(m, m, 1);
  }

  if (neg)
    mpz_neg(z, z);

done:
  mpz_clear(m);
  mpz_clear(t);
}

void
mpz_bin_uiui(mpz_ptr z, mp_limb_t n, mp_limb_t k) {
  /* bin(n, k) = n! / (k! * (n - k)!) */
  mp_limb_t i;

  if (n < k) {
    z->size = 0;
    return;
  }

  /* bin(n, k) = bin(n, n - k) */
  if (k > n - k)
    k = n - k;

  mpz_set_ui(z, 1);

  for (i = 0; i < k; i++) {
    mpz_mul_ui(z, z, n - i);
    mpz_quo_ui(z, z, i + 1);
  }
}

void
mpz_bin_siui(mpz_ptr z, mp_long_t n, mp_limb_t k) {
  if (n < 0) {
    /* bin(-n, k) = (-1)^k * bin(n + k - 1, k) */
    mp_limb_t m = mp_limb_cast(n);
    mpz_t t;

    if (UNLIKELY(m + k < k)) {
      mpz_roset_n(t, &m, -1);
      mpz_bin_ui(z, t, k);
    } else {
      mpz_bin_uiui(z, m + k - 1, k);

      if (k & 1)
        mpz_neg(z, z);
    }
  } else {
    mpz_bin_uiui(z, n, k);
  }
}

void
mpz_fib_ui(mpz_ptr fn, mp_limb_t n) {
  mpz_fib2_ui(fn, NULL, n);
}

void
mpz_fib2_ui(mpz_ptr fn, mpz_ptr fn1, mp_limb_t n) {
  mpz_t a, b, c, d, t;
  mp_bits_t i;

  if (n == 0) {
    if (fn1 != NULL)
      fn1->size = 0;

    fn->size = 0;

    return;
  }

  mpz_init(a);
  mpz_init(b);
  mpz_init(c);
  mpz_init(d);
  mpz_init(t);

  mpz_set_ui(a, 0);
  mpz_set_ui(b, 1);

  n -= 1;

  for (i = mp_bitlen(n) - 1; i >= 0; i--) {
    mpz_add(t, b, b);
    mpz_sub(t, t, a);
    mpz_mul(c, a, t);

    mpz_sqr(d, a);
    mpz_sqr(t, b);
    mpz_add(d, d, t);

    if ((n >> i) & 1) {
      mpz_add(b, c, d);
      mpz_swap(a, d);
    } else {
      mpz_swap(a, c);
      mpz_swap(b, d);
    }
  }

  if (fn1 != NULL)
    mpz_swap(fn1, a);

  mpz_swap(fn, b);

  mpz_clear(a);
  mpz_clear(b);
  mpz_clear(c);
  mpz_clear(d);
  mpz_clear(t);
}

void
mpz_lucnum_ui(mpz_ptr ln, mp_limb_t n) {
  mpz_lucnum2_ui(ln, NULL, n);
}

void
mpz_lucnum2_ui(mpz_ptr ln, mpz_ptr ln1, mp_limb_t n) {
  mpz_t fn, fn1;

  if (n == 0) {
    if (ln1 != NULL)
      ln1->size = 0;

    mpz_set_ui(ln, 2);

    return;
  }

  mpz_init(fn);
  mpz_init(fn1);

  mpz_fib2_ui(fn, fn1, n);

  if (ln1 != NULL) {
    /* L[n-1] = 2*F[n] - F[n-1] */
    mpz_add(ln1, fn, fn);
    mpz_sub(ln1, ln1, fn1);
  }

  /* L[n] = F[n] + 2*F[n-1] */
  mpz_add(ln, fn1, fn1);
  mpz_add(ln, ln, fn);

  mpz_clear(fn);
  mpz_clear(fn1);
}

/*
 * Primality Testing (logic from golang)
 */

int
mpz_mr_prime_p(mpz_srcptr n, int reps, int force2, mp_rng_f *rng, void *arg) {
  /* Miller-Rabin Primality Test.
   *
   * [HANDBOOK] Algorithm 4.24, Page 139, Section 4.2.3.
   */
  mpz_t nm1, nm3, q, x, y, t;
  mp_bits_t j, k;
  int ret = 0;
  int i;

  /* if n < 2 */
  if (mpz_cmp_ui(n, 2) < 0)
    return 0;

  /* if n mod 2 == 0 */
  if (mpz_even_p(n)) {
    /* n == 2 */
    return mpz_cmp_ui(n, 2) == 0;
  }

  /* if n <= 7 */
  if (mpz_cmp_ui(n, 7) <= 0) {
    /* n == 3 or n == 5 or n == 7 */
    return 1;
  }

  mpz_init(nm1);
  mpz_init(nm3);
  mpz_init(q);
  mpz_init(x);
  mpz_init(y);
  mpz_init(t);

  /* nm1 = n - 1 */
  mpz_sub_ui(nm1, n, 1);

  /* nm3 = nm1 - 2 */
  mpz_sub_ui(nm3, nm1, 2);

  /* k = nm1 factors of 2 */
  k = mpz_ctz(nm1);

  /* q = nm1 >> k */
  mpz_quo_2exp(q, nm1, k);

  for (i = 0; i < reps; i++) {
    if (i == reps - 1 && force2) {
      /* x = 2 */
      mpz_set_ui(x, 2);
    } else {
      /* x = random integer in [2,n-2] */
      mpz_urandomm(x, nm3, rng, arg);
      mpz_add_ui(x, x, 2);
    }

    /* y = x^q mod n */
    mpz_powm(y, x, q, n);

    /* if y == 1 or y == -1 mod n */
    if (mpz_cmp_ui(y, 1) == 0 || mpz_cmp(y, nm1) == 0)
      continue;

    for (j = 1; j < k; j++) {
      /* y = y^2 mod n */
      mpz_sqr(t, y);
      mpz_mod(y, t, n);

      /* if y == -1 mod n */
      if (mpz_cmp(y, nm1) == 0)
        goto next;

      /* if y == 1 mod n */
      if (mpz_cmp_ui(y, 1) == 0)
        goto fail;
    }

    goto fail;
next:
    ;
  }

  ret = 1;
fail:
  mpz_clear(nm1);
  mpz_clear(nm3);
  mpz_clear(q);
  mpz_clear(x);
  mpz_clear(y);
  mpz_clear(t);
  return ret;
}

int
mpz_lucas_prime_p(mpz_srcptr n, mp_limb_t limit) {
  /* Lucas Primality Test.
   *
   * [LUCAS] Page 1401, Section 5.
   */
  mpz_t d, s, nm2, vk, vk1, t1, t2;
  mp_bits_t i, r, t;
  mp_limb_t p;
  int ret = 0;
  int j;

  /* if n < 2 */
  if (mpz_cmp_ui(n, 2) < 0)
    return 0;

  /* if n mod 2 == 0 */
  if (mpz_even_p(n)) {
    /* n == 2 */
    return mpz_cmp_ui(n, 2) == 0;
  }

  /* if n <= 7 */
  if (mpz_cmp_ui(n, 7) <= 0) {
    /* n == 3 or n == 5 or n == 7 */
    return 1;
  }

  mpz_init(d);
  mpz_init(s);
  mpz_init(nm2);
  mpz_init(vk);
  mpz_init(vk1);
  mpz_init(t1);
  mpz_init(t2);

  /* p = 3 */
  p = 3;

  for (;;) {
    if (p > 10000) {
      /* Thought to be impossible. */
      goto fail;
    }

    if (limit != 0 && p > limit) {
      /* Enforce a limit to prevent DoS'ing. */
      goto fail;
    }

    /* d = p^2 - 4 */
    mpz_set_ui(d, p * p - 4);

    /* j = jacobi(d) in F(n) */
    j = mpz_jacobi(d, n);

    /* if d is non-square in F(n) */
    if (j == -1)
      break;

    /* if d is zero in F(n) */
    if (j == 0) {
      /* if n == p + 2 */
      if (mpz_cmp_ui(n, p + 2) == 0)
        goto succeed;
      goto fail;
    }

    if (p == 40) {
      /* if floor(n^(1 / 2))^2 == n */
      if (mpz_perfect_square_p(n))
        goto fail;
    }

    p += 1;
  }

  /* s = n + 1 */
  mpz_add_ui(s, n, 1);

  /* r = s factors of 2 */
  r = mpz_ctz(s);

  /* s >>= r */
  mpz_quo_2exp(s, s, r);

  /* nm2 = n - 2 */
  mpz_sub_ui(nm2, n, 2);

  /* vk = 2 */
  mpz_set_ui(vk, 2);

  /* vk1 = p */
  mpz_set_ui(vk1, p);

  for (i = mpz_bitlen(s); i >= 0; i--) {
    /* if floor(s / 2^i) mod 2 == 1 */
    if (mpz_tstbit(s, i)) {
      /* vk = vk * vk1 - p mod n */
      /* vk1 = vk1^2 - 2 mod n */
      mpz_mul(t1, vk, vk1);
      mpz_sub_ui(t1, t1, p);
      mpz_mod(vk, t1, n);
      mpz_sqr(t1, vk1);
      mpz_sub_ui(t1, t1, 2);
      mpz_mod(vk1, t1, n);
    } else {
      /* vk1 = vk1 * vk - p mod n */
      /* vk = vk^2 - 2 mod n */
      mpz_mul(t1, vk1, vk);
      mpz_sub_ui(t1, t1, p);
      mpz_mod(vk1, t1, n);
      mpz_sqr(t1, vk);
      mpz_sub_ui(t1, t1, 2);
      mpz_mod(vk, t1, n);
    }
  }

  /* if vk == +-2 mod n */
  if (mpz_cmp_ui(vk, 2) == 0 || mpz_cmp(vk, nm2) == 0) {
    /* if vk * p == vk1 * 2 mod n */
    mpz_mul_ui(t1, vk, p);
    mpz_mul_2exp(t2, vk1, 1);

    mpz_sub(t1, t1, t2);
    mpz_mod(t1, t1, n);

    if (t1->size == 0)
      goto succeed;
  }

  for (t = 0; t < r - 1; t++) {
    /* if vk == 0 */
    if (vk->size == 0)
      goto succeed;

    /* if vk == 2 */
    if (mpz_cmp_ui(vk, 2) == 0)
      goto fail;

    /* vk = vk^2 - 2 mod n */
    mpz_sqr(t1, vk);
    mpz_sub_ui(t1, t1, 2);
    mpz_mod(vk, t1, n);
  }

  goto fail;
succeed:
  ret = 1;
fail:
  mpz_clear(d);
  mpz_clear(s);
  mpz_clear(nm2);
  mpz_clear(vk);
  mpz_clear(vk1);
  mpz_clear(t1);
  mpz_clear(t2);
  return ret;
}

static void
mpz_mod_primorial(mp_limb_t *ra, mp_limb_t *rb, mpz_srcptr x) {
  /* Compute x mod (p(16)# / 2).
   *
   * Where p(16)# = 32589158477190044730.
   *
   * See: https://oeis.org/A002110
   *      https://oeis.org/A070826
   *
   * This function avoids division. Our 2-by-1
   * inverses are precomputed and the modulo's
   * below are strength-reduced by GCC.
   */
#if MP_LIMB_BITS == 64
  /* d = p(16)# / 2 */
  static const mp_limb_t d = MP_LIMB_C(16294579238595022365);
  static const mp_limb_t m = MP_LIMB_C(2436419703539282795);
  mp_limb_t r = mpz_mod_2by1(x, d, m);

  *ra = r % MP_LIMB_C(4127218095);
  *rb = r % MP_LIMB_C(3948078067);
#else
  /* d1 = 3 * 5 * 7 * 11 * 13 * 17 * 19 * 23 * 37 */
  /* d2 = 29 * 31 * 41 * 43 * 47 * 53 */
  /* d1 * d2 = p(16)# / 2 */
  static const mp_limb_t d1 = MP_LIMB_C(4127218095);
  static const mp_limb_t m1 = MP_LIMB_C(174567303);
  static const mp_limb_t d2 = MP_LIMB_C(3948078067);
  static const mp_limb_t m2 = MP_LIMB_C(377367891);

  *ra = mpz_mod_2by1(x, d1, m1);
  *rb = mpz_mod_2by1(x, d2, m2);
#endif
}

int
mpz_probab_prime_p(mpz_srcptr x, int rounds, mp_rng_f *rng, void *arg) {
  /* Baillie-PSW Primality Test.
   *
   * [BPSW] "Bibliography".
   */
  mp_limb_t ra, rb;

  /* No negatives. */
  if (x->size <= 0)
    return 0;

  /* Check small primes list. */
  if (x->size == 1 && x->limbs[0] < 1024)
    return mpn_tstbit(mp_primes, x->limbs[0]);

  /* No even numbers. */
  if ((x->limbs[0] & 1) == 0)
    return 0;

  /* Trial division. */
  mpz_mod_primorial(&ra, &rb, x);

  if (ra % 3 == 0
      || ra % 5 == 0
      || ra % 7 == 0
      || ra % 11 == 0
      || ra % 13 == 0
      || ra % 17 == 0
      || ra % 19 == 0
      || ra % 23 == 0
      || ra % 37 == 0
      || rb % 29 == 0
      || rb % 31 == 0
      || rb % 41 == 0
      || rb % 43 == 0
      || rb % 47 == 0
      || rb % 53 == 0) {
    return 0;
  }

  /* Miller-Rabin primality test. */
  if (!mpz_mr_prime_p(x, rounds + 1, 1, rng, arg))
    return 0;

  /* Lucas primality test. */
  if (!mpz_lucas_prime_p(x, 0))
    return 0;

  return 1;
}

void
mpz_randprime(mpz_ptr z, mp_bits_t bits, mp_rng_f *rng, void *arg) {
  static const mp_limb_t max = MP_LIMB_C(1) << 20;
  mp_limb_t r, ra, rb, delta;

  CHECK(bits > 1);

  for (;;) {
    mpz_urandomb(z, bits, rng, arg);

    mpz_setbit(z, bits - 1);
    mpz_setbit(z, bits - 2);
    mpz_setbit(z, 0);

    if (bits > 6) {
      mpz_mod_primorial(&ra, &rb, z);

      for (delta = 0; delta < max; delta += 2) {
        r = ra + delta;

        if (r % 3 == 0
            || r % 5 == 0
            || r % 7 == 0
            || r % 11 == 0
            || r % 13 == 0
            || r % 17 == 0
            || r % 19 == 0
            || r % 23 == 0
            || r % 37 == 0) {
          continue;
        }

        r = rb + delta;

        if (r % 29 == 0
            || r % 31 == 0
            || r % 41 == 0
            || r % 43 == 0
            || r % 47 == 0
            || r % 53 == 0) {
          continue;
        }

        mpz_add_ui(z, z, delta);

        break;
      }

      if (mpz_bitlen(z) != bits)
        continue;
    }

    if (!mpz_probab_prime_p(z, 20, rng, arg))
      continue;

    break;
  }
}

void
mpz_nextprime(mpz_ptr z, mpz_srcptr x, mp_rng_f *rng, void *arg) {
  if (mpz_cmp_ui(x, 2) < 0) {
    mpz_set_ui(z, 2);
    return;
  }

  mpz_add_ui(z, x, 1 + mpz_odd_p(x));

  while (!mpz_probab_prime_p(z, 20, rng, arg))
    mpz_add_ui(z, z, 2);
}

int
mpz_findprime(mpz_ptr z, mpz_srcptr x, mp_limb_t m, mp_rng_f *rng, void *arg) {
  mp_limb_t i;

  mpz_set(z, x);

  if (mpz_even_p(z)) {
    mpz_add_ui(z, z, 1);

    if (m == 0)
      return 0;

    m -= 1;
  }

  m = (m / 2) + 1;

  for (i = 0; i < m; i++) {
    if (mpz_probab_prime_p(z, 20, rng, arg))
      return 1;

    mpz_add_ui(z, z, 2);
  }

  return 0;
}

/*
 * Helpers
 */

int
mpz_fits_ui_p(mpz_srcptr x) {
  return MP_ABS(x->size) <= 1;
}

int
mpz_fits_si_p(mpz_srcptr x) {
  if (MP_ABS(x->size) > 1)
    return 0;

  if (x->size == 0)
    return 1;

  if (x->size < 0)
    return x->limbs[0] <= MP_LIMB_HI;

  return x->limbs[0] < MP_LIMB_HI;
}

int
mpz_odd_p(mpz_srcptr x) {
  if (x->size == 0)
    return 0;

  return x->limbs[0] & 1;
}

int
mpz_even_p(mpz_srcptr x) {
  return !mpz_odd_p(x);
}

mp_bits_t
mpz_ctz(mpz_srcptr x) {
  return mpn_ctz(x->limbs, MP_ABS(x->size));
}

mp_bits_t
mpz_bitlen(mpz_srcptr x) {
  mp_size_t xn = MP_ABS(x->size);

  if (xn == 0)
    return 0;

  return xn * MP_LIMB_BITS - mp_clz(x->limbs[xn - 1]);
}

size_t
mpz_bytelen(mpz_srcptr x) {
  return (mpz_bitlen(x) + 7) / 8;
}

size_t
mpz_sizeinbase(mpz_srcptr x, int base) {
  return mpn_sizeinbase(x->limbs, MP_ABS(x->size), base);
}

void
mpz_swap(mpz_ptr x, mpz_ptr y) {
  mp_limb_t *limbs = x->limbs;
  mp_size_t alloc = x->alloc;
  mp_size_t size = x->size;

  x->limbs = y->limbs;
  x->alloc = y->alloc;
  x->size = y->size;

  y->limbs = limbs;
  y->alloc = alloc;
  y->size = size;
}

void *
_mpz_realloc(mpz_ptr z, mp_size_t n) {
  n = MP_MAX(1, n);

  ASSERT(z->alloc > 0);

  if (n < z->alloc) {
    z->limbs = mp_realloc_limbs(z->limbs, n);
    z->alloc = n;

    if (n < MP_ABS(z->size)) {
      z->limbs[0] = 0;
      z->size = 0;
    }
  } else {
    mpz_grow(z, n);
  }

  return z->limbs;
}

void
mpz_realloc2(mpz_ptr z, mp_bits_t bits) {
  _mpz_realloc(z, (bits + MP_LIMB_BITS - 1) / MP_LIMB_BITS);
}

/*
 * Limb Helpers
 */

mp_limb_t
mpz_getlimbn(mpz_srcptr x, mp_size_t n) {
  if (n >= MP_ABS(x->size))
    return 0;

  return x->limbs[n];
}

size_t
mpz_size(mpz_srcptr x) {
  return MP_ABS(x->size);
}

const mp_limb_t *
mpz_limbs_read(mpz_srcptr x) {
  return x->limbs;
}

mp_limb_t *
mpz_limbs_write(mpz_ptr z, mp_size_t n) {
  if (n > z->alloc) {
    mpz_clear(z);
    mpz_init_n(z, n);
  }

  return z->limbs;
}

mp_limb_t *
mpz_limbs_modify(mpz_ptr z, mp_size_t n) {
  return mpz_grow(z, n);
}

void
mpz_limbs_finish(mpz_ptr z, mp_size_t n) {
  mp_size_t zn = mpn_strip(z->limbs, MP_ABS(n));

  z->size = n < 0 ? -zn : zn;
}

/*
 * Import
 */

void
mpz_import(mpz_ptr z, const unsigned char *raw, size_t size, int endian) {
  mp_size_t zn = mp_size_cast((size + MP_LIMB_BYTES - 1) / MP_LIMB_BYTES);
  mp_limb_t *zp;

  if (zn == 0) {
    z->size = 0;
    return;
  }

  zp = mpz_grow(z, zn);

  mpn_import(zp, zn, raw, size, endian);

  z->size = mpn_strip(zp, zn);
}

/*
 * Export
 */

void
mpz_export(unsigned char *raw, mpz_srcptr x, size_t size, int endian) {
  CHECK(size >= mpz_bytelen(x));
  mpn_export(raw, size, x->limbs, MP_ABS(x->size), endian);
}

/*
 * String Import
 */

int
mpz_set_str(mpz_ptr z, const char *str, int base) {
  mp_limb_t *zp;
  mp_size_t zn;
  int neg = 0;

  if (str == NULL) {
    z->size = 0;
    return 0;
  }

  while (mp_isspace(*str))
    str++;

  if (*str == '+' || *str == '-') {
    neg = (*str == '-');
    str++;
  }

  if (base == 0) {
    if (str[0] == '0') {
      switch (str[1]) {
        case 'b':
        case 'B':
          base = 2;
          str += 2;
          break;
        case 'o':
        case 'O':
          base = 8;
          str += 2;
          break;
        case 'x':
        case 'X':
          base = 16;
          str += 2;
          break;
        default:
          base = 8;
          break;
      }
    } else {
      base = 10;
    }
  }

  zn = mp_str_limbs(str, base);
  zp = mpz_grow(z, zn);

  if (!mpn_set_str(zp, zn, str, base)) {
    z->size = 0;
    return 0;
  }

  zn = mpn_strip(zp, zn);

  z->size = neg ? -zn : zn;

  return 1;
}

/*
 * String Export
 */

char *
mpz_get_str(mpz_srcptr x, int base) {
  size_t len = mpz_sizeinbase(x, base);
  size_t neg = (x->size < 0);
  char *str = mp_alloc_str(neg + len + 1);

  mpn_get_str(str + neg, x->limbs, MP_ABS(x->size), base);

  if (neg)
    str[0] = '-';

  return str;
}

/*
 * STDIO
 */

void
mpz_print(mpz_srcptr x, int base, mp_puts_f *mp_puts) {
  char *str = mpz_get_str(x, base);

  mp_puts(str);
  mp_free_str(str);
}

/*
 * RNG
 */

void
mpz_urandomb(mpz_ptr z, mp_bits_t bits, mp_rng_f *rng, void *arg) {
  mp_size_t zn = (bits + MP_LIMB_BITS - 1) / MP_LIMB_BITS;
  mp_bits_t lo = bits % MP_LIMB_BITS;
  mp_limb_t *zp = mpz_grow(z, zn);

  mpn_random(zp, zn, rng, arg);

  if (lo != 0)
    zp[zn - 1] &= MP_MASK(lo);

  z->size = mpn_strip(zp, zn);
}

void
mpz_urandomm(mpz_ptr z, mpz_srcptr x, mp_rng_f *rng, void *arg) {
  mp_size_t xn, zn;
  mp_limb_t *zp;

  if (z == x)
    btc_abort(); /* LCOV_EXCL_LINE */

  if (x->size == 0) {
    z->size = 0;
    return;
  }

  xn = MP_ABS(x->size);
  zn = xn;
  zp = mpz_grow(z, zn);

  mpn_randomm(zp, x->limbs, xn, rng, arg);

  zn = mpn_strip(zp, zn);

  z->size = x->size < 0 ? -zn : zn;
}
