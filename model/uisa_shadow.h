/* Shadow functional model of the 68040-on-uisa scheme.
 *
 * The host emulator (Previous) calls uisa_shadow_pre() before each 68k
 * instruction and uisa_shadow_post() after it, and logs every memory access
 * the instruction makes with uisa_shadow_log() while uisa_shadow_logging is
 * set. The model runs the instruction's PLA micro-op flow on the pre-state,
 * taking extension words and load values from the log, and compares the
 * resulting registers, CCR, next PC and stores with what the host did.
 */
#ifndef UISA_SHADOW_H
#define UISA_SHADOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct uisa_m68k_state {
	uint32_t r[16];     /* D0-D7, A0-A7 (A7 = active stack pointer) */
	uint32_t pc;
	uint16_t sr;
};

enum { UISA_LOG_READ = 0, UISA_LOG_WRITE = 1, UISA_LOG_IFETCH = 2 };

extern int uisa_shadow_logging;

void uisa_shadow_init(const char *report_path);
void uisa_shadow_pre(uint16_t opword, const struct uisa_m68k_state *s);
void uisa_shadow_log(int kind, uint32_t addr, int size, uint32_t value);
void uisa_shadow_post(const struct uisa_m68k_state *s);
void uisa_shadow_dump(void);

/* For tests: how many times `opword` produced `result` (0 = match, 1 = reg
 * mismatch, ... in the order of the report columns). */
uint64_t uisa_shadow_count(uint16_t opword, int result);

#ifdef __cplusplus
}
#endif

#endif
