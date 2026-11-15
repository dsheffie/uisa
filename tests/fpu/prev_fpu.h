/* prev_fpu.h: C interface to Previous's FPU instruction code (prev_fpu.c). */
#ifndef PREV_FPU_H
#define PREV_FPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REF_MAX_STORES 8

struct ref_state {
	uint32_t r[16];          /* D0-D7, A0-A7 */
	uint32_t pc;             /* in: the instruction's address; out: the PC after it (no exception) */
	uint32_t fp[8][3];       /* FP0-FP7 in FMOVEM extended format */
	uint32_t fpcr, fpsr, fpiar;
	int vector;              /* out: exception taken (0 = none) */
	uint32_t fp_exp_pend;    /* out: arithmetic exception left pending */
	uint32_t fpu_exp_state;  /* out: FSAVE frame state (0 = idle; set by the FPSP traps) */
	int nstores;
	uint32_t st_addr[REF_MAX_STORES], st_val[REF_MAX_STORES];
	int st_size[REF_MAX_STORES];
};

uint8_t ref_membyte(uint32_t addr);   /* supplied by the driver */
void ref_init(void);
void ref_run(uint16_t opword, struct ref_state *s);

#ifdef __cplusplus
}
#endif

#endif
