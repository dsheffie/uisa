/* csmith.h for ucc program mode: a stand-in for csmith's runtime headers.
 *
 * csmith programs `#include "csmith.h"`. compiler/prog/test_prog.py runs
 * them through the host C preprocessor with -I compiler/prog/csmith twice:
 * with -D__UCC__ for `ucc --program`, and without it for the host compiler.
 * Both builds get these definitions, so the checksums compare. They are not
 * the checksums csmith itself prints (that runtime uses printf and strings,
 * which program mode does not have).
 *
 * Generate programs with (csmith 2.3; test_prog.py --csmith N does):
 *   csmith --no-unions --no-bitfields --no-volatiles --no-packed-struct
 *          --no-float
 *
 * Integer model: as on the LP64 host, `long` and the L suffix are 64 bits
 * in program mode, so csmith's long constants mean the same on both sides.
 */
#ifndef UCC_CSMITH_SHIM_H
#define UCC_CSMITH_SHIM_H

#ifdef __UCC__
typedef s8 int8_t;
typedef u8 uint8_t;
typedef s16 int16_t;
typedef u16 uint16_t;
typedef s32 int32_t;
typedef u32 uint32_t;
typedef s64 int64_t;
typedef u64 uint64_t;
#define INT8_MIN (-127 - 1)
#define INT8_MAX 127
#define INT16_MIN (-32767 - 1)
#define INT16_MAX 32767
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define INT64_MIN (-9223372036854775807LL - 1)
#define INT64_MAX 9223372036854775807LL
#define UINT8_MAX 255
#define UINT16_MAX 65535
#define UINT32_MAX 4294967295U
#define UINT64_MAX 18446744073709551615ULL
#else
#include <stdint.h>
#endif

/* keywords and library calls program mode does not have */
#define static
#define const
#define volatile
#define strcmp(a, b) 1
#define printf(...) 0

/* signed: the result if it is defined, else the left operand (csmith's rules) */
#define UCC_SAFE_S(T, MIN, MAX, BITS)                                                                           \
  T safe_unary_minus_func_##T##_s(T si) { return si == MIN ? si : -si; }                                      \
  T safe_add_func_##T##_s_s(T a, T b) {                                                                       \
    return ((a > 0 && b > 0 && a > MAX - b) || (a < 0 && b < 0 && a < MIN - b)) ? a : (T)(a + b);             \
  }                                                                                                          \
  T safe_sub_func_##T##_s_s(T a, T b) {                                                                       \
    return ((a ^ b) & (((a ^ ((a ^ b) & ~MAX)) - b) ^ b)) < 0 ? a : (T)(a - b);                               \
  }                                                                                                          \
  T safe_mul_func_##T##_s_s(T a, T b) {                                                                       \
    return ((a > 0 && b > 0 && a > MAX / b) || (a > 0 && b <= 0 && b < MIN / a) ||                            \
            (a <= 0 && b > 0 && a < MIN / b) || (a <= 0 && b <= 0 && a != 0 && b < MAX / a))                 \
               ? a                                                                                           \
               : (T)(a * b);                                                                                 \
  }                                                                                                          \
  T safe_mod_func_##T##_s_s(T a, T b) { return (b == 0 || (a == MIN && b == -1)) ? a : (T)(a % b); }         \
  T safe_div_func_##T##_s_s(T a, T b) { return (b == 0 || (a == MIN && b == -1)) ? a : (T)(a / b); }         \
  T safe_lshift_func_##T##_s_s(T a, int32_t b) {                                                             \
    return (a < 0 || b < 0 || b >= BITS || a > (MAX >> b)) ? a : (T)(a << b);                                 \
  }                                                                                                          \
  T safe_lshift_func_##T##_s_u(T a, uint32_t b) { return (a < 0 || b >= BITS || a > (MAX >> b)) ? a : (T)(a << b); } \
  T safe_rshift_func_##T##_s_s(T a, int32_t b) { return (a < 0 || b < 0 || b >= BITS) ? a : (T)(a >> b); }   \
  T safe_rshift_func_##T##_s_u(T a, uint32_t b) { return (a < 0 || b >= BITS) ? a : (T)(a >> b); }

/* unsigned: wrapping arithmetic, guarded division and shifts */
#define UCC_SAFE_U(T, MAX, BITS)                                                                                \
  T safe_unary_minus_func_##T##_u(T ui) { return (T)-ui; }                                                    \
  T safe_add_func_##T##_u_u(T a, T b) { return (T)(a + b); }                                                  \
  T safe_sub_func_##T##_u_u(T a, T b) { return (T)(a - b); }                                                  \
  T safe_mul_func_##T##_u_u(T a, T b) { return (T)((uint64_t)a * b); }                                        \
  T safe_mod_func_##T##_u_u(T a, T b) { return b == 0 ? a : (T)(a % b); }                                     \
  T safe_div_func_##T##_u_u(T a, T b) { return b == 0 ? a : (T)(a / b); }                                     \
  T safe_lshift_func_##T##_u_s(T a, int32_t b) { return (b < 0 || b >= BITS || a > (MAX >> b)) ? a : (T)(a << b); } \
  T safe_lshift_func_##T##_u_u(T a, uint32_t b) { return (b >= BITS || a > (MAX >> b)) ? a : (T)(a << b); }   \
  T safe_rshift_func_##T##_u_s(T a, int32_t b) { return (b < 0 || b >= BITS) ? a : (T)(a >> b); }            \
  T safe_rshift_func_##T##_u_u(T a, uint32_t b) { return b >= BITS ? a : (T)(a >> b); }

UCC_SAFE_S(int8_t, INT8_MIN, INT8_MAX, 32)
UCC_SAFE_S(int16_t, INT16_MIN, INT16_MAX, 32)
UCC_SAFE_S(int32_t, INT32_MIN, INT32_MAX, 32)
UCC_SAFE_S(int64_t, INT64_MIN, INT64_MAX, 64)
UCC_SAFE_U(uint8_t, UINT8_MAX, 32)
UCC_SAFE_U(uint16_t, UINT16_MAX, 32)
UCC_SAFE_U(uint32_t, UINT32_MAX, 32)
UCC_SAFE_U(uint64_t, UINT64_MAX, 64)

/* the checksum: CRC-32 over every global, 8 bytes at a time */
uint32_t crc32_tab[256];
uint32_t crc32_context = 0xFFFFFFFFU;
void crc32_gentab(void) {
  for (int32_t i = 0; i < 256; i++) {
    uint32_t crc = (uint32_t)i;
    for (int32_t j = 8; j > 0; j--) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
    crc32_tab[i] = crc;
  }
}
void crc32_byte(uint8_t b) { crc32_context = ((crc32_context >> 8) & 0x00FFFFFF) ^ crc32_tab[(crc32_context ^ b) & 0xFF]; }
void crc32_8bytes(uint64_t val) {
  for (int32_t k = 0; k < 8; k++) crc32_byte((uint8_t)(val >> (8 * k)));
}
#define transparent_crc(val, name, flag) crc32_8bytes((uint64_t)(val))
#define platform_main_begin()
#define platform_main_end(crc, flag) return (int32_t)(crc)

#endif
