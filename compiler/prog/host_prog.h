// Host side of program-mode tests: ucc's integer type names for a C
// compiler, so the same source runs natively. Compile with -fwrapv and
// -Dmain=prog_main; compiler/prog/host_main.c prints the result.
#ifndef UCC_HOST_PROG_H
#define UCC_HOST_PROG_H
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#endif
