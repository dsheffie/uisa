/* prev_fpu.c: Previous's own 68040 FPU instruction code (cpu/fpp.c with the
 * SoftFloat back end) as the reference for millicode/fpu.c.
 *
 * Built against Previous's headers and linked with fpp.c, fpp_softfloat.c and
 * its softfloat; this file supplies the few CPU and memory hooks fpp.c needs.
 * Memory reads come from ref_membyte() (the test driver's memory); stores and
 * exceptions are recorded in the ref_state. Configured as Previous runs a
 * NeXT 68040: cpu_model = fpu_model = 68040, fpu_no_unimplemented = true.
 */
#include "sysconfig.h"
#include "sysdeps.h"
#include "options_cpu.h"
#include "memory.h"
#include "newcpu.h"
#include "fpp.h"
#include "main.h"

#include "prev_fpu.h"

struct regstruct regs;
struct uae_prefs currprefs, changed_prefs;
struct mmufixup mmufixup[2];
uae_u16 mmu030_state[3];
uae_u32 mmu030_data_buffer_out;
uae_u32 mmu030_fmovem_store[2];

static struct ref_state *cur;

static uae_u32 rd(uaecptr a, int n)
{
	uae_u32 v = 0;
	for (int i = 0; i < n; i++)
		v = (v << 8) | ref_membyte(a + i);
	return v;
}
static void wr(uaecptr a, int n, uae_u32 v)
{
	if (cur->nstores < REF_MAX_STORES) {
		cur->st_addr[cur->nstores] = a;
		cur->st_size[cur->nstores] = n;
		cur->st_val[cur->nstores] = v;
	}
	cur->nstores++;
}

static uae_u32 g_byte(uaecptr a) { return rd(a, 1); }
static uae_u32 g_word(uaecptr a) { return rd(a, 2); }
static uae_u32 g_long(uaecptr a) { return rd(a, 4); }
static void p_byte(uaecptr a, uae_u32 v) { wr(a, 1, v & 0xff); }
static void p_word(uaecptr a, uae_u32 v) { wr(a, 2, v & 0xffff); }
static void p_long(uaecptr a, uae_u32 v) { wr(a, 4, v); }
static uae_u32 n_iword(void) { uae_u32 v = rd(regs.pc, 2); regs.pc += 2; return v; }
static uae_u32 n_ilong(void) { uae_u32 v = rd(regs.pc, 4); regs.pc += 4; return v; }

/* Brief-format index only (the tests generate no full-format words). */
static uae_u32 REGPARAM2 disp_ea_020(uae_u32 base, int idx)
{
	uae_u16 w = n_iword();
	uae_s32 x = regs.regs[(w >> 12) & 15];
	if (!(w & 0x800))
		x = (uae_s16)x;
	return base + (uae_s8)w + (x << ((w >> 9) & 3));
}

uae_u32 (*x_get_long)(uaecptr) = g_long;
void (*x_put_long)(uaecptr, uae_u32) = p_long;
uae_u32 (*x_cp_get_byte)(uaecptr) = g_byte;
uae_u32 (*x_cp_get_word)(uaecptr) = g_word;
uae_u32 (*x_cp_get_long)(uaecptr) = g_long;
void (*x_cp_put_byte)(uaecptr, uae_u32) = p_byte;
void (*x_cp_put_word)(uaecptr, uae_u32) = p_word;
void (*x_cp_put_long)(uaecptr, uae_u32) = p_long;
uae_u32 (*x_cp_next_iword)(void) = n_iword;
uae_u32 (*x_cp_next_ilong)(void) = n_ilong;
uae_u32 (REGPARAM3 *x_cp_get_disp_ea_020)(uae_u32, int) REGPARAM = disp_ea_020;

void REGPARAM2 Exception(int nr)
{
	if (!cur->vector)
		cur->vector = nr;
}
void REGPARAM2 Exception_cpu_oldpc(int nr, uaecptr oldpc) { Exception(nr); }
uae_u32 REGPARAM2 op_illg(uae_u32 opcode) { Exception((opcode & 0xf000) == 0xf000 ? 11 : 4); return 0; }
void check_t0_trace(void) {}
void set_cpu_caches(bool flush) {}
void fpux_restore(int *v) {}

void ref_init(void)
{
	currprefs.cpu_model = currprefs.fpu_model = 68040;
	currprefs.fpu_no_unimplemented = true;
	changed_prefs = currprefs;
	fpu_reset();
}

void ref_run(uint16_t opword, struct ref_state *s)
{
	cur = s;
	s->vector = 0;
	s->nstores = 0;
	for (int i = 0; i < 16; i++)
		regs.regs[i] = s->r[i];
	for (int i = 0; i < 8; i++)
		fpp_to_exten_fmovem(&regs.fp[i], s->fp[i][0], s->fp[i][1], s->fp[i][2]);
	fpp_set_fpcr(s->fpcr);
	fpp_set_fpsr(s->fpsr);
	fpp_set_fpiar(s->fpiar);
	regs.fp_exp_pend = regs.fp_unimp_pend = 0;
	regs.fp_unimp_ins = false;
	regs.fpu_exp_state = 0;
	regs.pcr = 0;
	regs.s = 1;
	uae_u16 extra = rd(s->pc + 2, 2);
	regs.pc_p = regs.pc_oldp = NULL;
	regs.instruction_pc = s->pc;
	regs.pc = s->pc + 4;
	fpuop_arithmetic(opword, extra);
	for (int i = 0; i < 16; i++)
		s->r[i] = regs.regs[i];
	for (int i = 0; i < 8; i++)
		fpp_from_exten_fmovem(&regs.fp[i], &s->fp[i][0], &s->fp[i][1], &s->fp[i][2]);
	s->fpcr = fpp_get_fpcr();
	s->fpsr = fpp_get_fpsr();
	s->fpiar = fpp_get_fpiar();
	s->pc = regs.pc;
	s->fp_exp_pend = regs.fp_exp_pend;
	s->fpu_exp_state = regs.fpu_exp_state;
}
