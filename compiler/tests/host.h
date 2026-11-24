// Host C++ implementations of the millicode C subset's types and of the
// intrinsics the tests use, so a test program can be compiled natively and
// compared against the same program compiled by ucc and run on the uisa ISS.
// Compile with -fwrapv: millicode arithmetic wraps.
#ifndef UCC_TEST_HOST_H
#define UCC_TEST_HOST_H

#include <cstdint>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
#define entry

namespace host {
extern uint32_t G[18];        // d0-d7, a0-a7, gpc, gsr
extern uint32_t E[5];         // entry-ABI x1..x4
extern uint32_t CCR;
struct Exit { int vector; };  // gexit (-1), gfault n, brk (1000 + n)
}

enum { D0, D1, D2, D3, D4, D5, D6, D7, A0, A1, A2, A3, A4, A5, A6, A7, GPC, GSR };
enum { CCR_C = 1, CCR_V = 2, CCR_Z = 4, CCR_N = 8, CCR_X = 16 };

// guest memory is a fixed function of the address in both worlds
static inline uint8_t membyte(uint32_t a) { return (uint8_t)(((a * 2654435761u) >> 24) ^ a); }

static inline u32 __rdg(u32 i) { return host::G[i & 31]; }
static inline void __wrg(u32 i, u32 v) { host::G[i & 31] = v; }
static inline u32 __opword() { return host::E[1]; }
static inline u32 __ext01() { return host::E[2]; }
static inline u32 __ext23() { return host::E[3]; }
static inline u32 __next_pc() { return host::E[4]; }
[[noreturn]] static inline void __gexit() { throw host::Exit{-1}; }
[[noreturn]] static inline void __gfault(u32 n) { throw host::Exit{(int)n}; }
[[noreturn]] static inline void __brk(u32 n) { throw host::Exit{1000 + (int)n}; }
static inline u32 __rdccr() { return host::CCR; }
static inline void __wrccr(u32 v) { host::CCR = v & 31; }
static inline u32 __clz(u32 a) { return a ? __builtin_clz(a) : 32; }
static inline u32 __ctz(u32 a) { return a ? __builtin_ctz(a) : 32; }
static inline u32 __cpop(u32 a) { return __builtin_popcount(a); }
static inline u32 __minu(u32 a, u32 b) { return a < b ? a : b; }
static inline u32 __maxu(u32 a, u32 b) { return a > b ? a : b; }
static inline u32 __min(u32 a, u32 b) { return (s32)a < (s32)b ? a : b; }
static inline u32 __max(u32 a, u32 b) { return (s32)a > (s32)b ? a : b; }
static inline u32 __rol(u32 a, u32 b) { b &= 31; return b ? (a << b) | (a >> (32 - b)) : a; }
static inline u32 __ror(u32 a, u32 b) { b &= 31; return b ? (a >> b) | (a << (32 - b)) : a; }
static inline u32 __mulhu(u32 a, u32 b) { return (u32)(((uint64_t)a * b) >> 32); }
static inline u32 __mulh(u32 a, u32 b) { return (u32)(((int64_t)(s32)a * (s32)b) >> 32); }
static inline u32 __rev8(u32 a) { return __builtin_bswap32(a); }
static inline u32 __mrg8(u32 a, u32 b) { return (a & ~0xffu) | (b & 0xff); }
static inline u32 __mrg16(u32 a, u32 b) { return (a & ~0xffffu) | (b & 0xffff); }
static inline u32 __bfextui(u32 a, u32 off, u32 wid) {   // 68k field: offset from the MSB, wraps
  wid = wid & 31 ? wid & 31 : 32;
  u32 r = __rol(a, off & 31);
  return wid == 32 ? r : r >> (32 - wid);
}
static inline u32 __ld_8u_gd(u32 a) { return membyte(a); }
static inline u32 __ld_16u_gd(u32 a) { return (membyte(a) << 8) | membyte(a + 1); }
static inline u32 __ld_32_gd(u32 a) {
  return ((u32)membyte(a) << 24) | (membyte(a + 1) << 16) | (membyte(a + 2) << 8) | membyte(a + 3);
}
static inline u32 __ld_8s_gd(u32 a) { return (u32)(s8)membyte(a); }

#endif
