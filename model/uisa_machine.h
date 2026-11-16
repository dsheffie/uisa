/* The uisa execution core: run one 68k instruction (or exception entry) as
 * uisa code — a PLA micro-op flow, the MOVEM sequencer, or a millicode
 * routine — on a copy of the guest state.
 *
 * Shared by the shadow checker (uisa_shadow.cc), whose memory is the host's
 * access log, and the execution engine (uisa_engine.cc), whose memory is the
 * host's MMU and bus. Memory is reached only through `Mem`.
 */
#ifndef UISA_MACHINE_H
#define UISA_MACHINE_H

#include <stdint.h>

#include <string>
#include <vector>

#include "uisa_shadow.h"   // uisa_m68k_state, UISA_CTL_*

namespace uisa_model {

const uint16_t UISA_PSEUDO_IDX = 0xffff;

struct uisa_fuop {
  uint16_t op;
  uint8_t rd, rs1, rs2;
  uint8_t ik;      // immediate kind (IK_*), bit 7 negates, bit 6 adds the bitfield byte offset
  int8_t ioff;     // extension-word offset from the instruction address
  int32_t imm;
};

enum { K_ILLEGAL, K_PLA, K_SEQUENCER, K_MILLICODE, K_UNMAPPED, K_NUM };
enum { G_XREG = 60,      // flow register: the register named by the extension word (bitfield Dx, MOVEC/MOVES Rn, MULx.L Dl, DIVx.L Dq)
       G_XREG2 = 61 };    // MULx.L Dh, DIVx.L Dr (extension bits 2:0)
enum { G_D = 32, G_A = 40, G_PC = 48, G_SR = 49 };   // guest registers in the uisa namespace

// Outcomes of a modelled instruction (the shadow report's columns).
enum {
  R_MATCH,
  R_MISMATCH_REG,
  R_MISMATCH_CCR,
  R_MISMATCH_PC,
  R_MISMATCH_SR,
  R_MISMATCH_CTL,
  R_MISMATCH_STORE,
  R_LOAD_NOT_IN_LOG,    // a load the memory could not supply (shadow: the host never read it; engine: access fault)
  R_EXT_NOT_IN_LOG,     // an extension word the memory could not supply
  R_FULL_EXT,           // full-format extension word: not modelled yet
  R_UNSUPPORTED_UOP,    // a uop or addressing form the model cannot run
  R_MC_UNHANDLED,       // millicode reached brk: a form its routine does not handle yet
  R_EXCEPTION,          // unused (exceptions are now modelled and compared)
  R_NATIVE,             // millicode operation with no routine yet (not compared)
  R_ABORTED,            // pre without post: the host took a fault mid-instruction
  R_NUM
};
extern const char *const R_NAMES[R_NUM];

// Memory as one guest instruction sees it. Function codes are the 68040's
// (1 user data, 2 user program, 5 supervisor data, 6 supervisor program).
struct Mem {
  virtual ~Mem() {}
  // instruction-stream bytes (extension words); false = not available
  virtual bool ifetch(uint32_t addr, int n, uint32_t &v) = 0;
  // a guest-space load; false = not available
  virtual bool load(int fc, uint32_t addr, int n, uint32_t &v) = 0;
  virtual void store(int fc, uint32_t addr, int n, uint32_t v) = 0;
  // probe.r / probe.w: the table walker's status (68040 MMUSR) for addr
  virtual uint32_t probe(int fc, uint32_t addr, bool write) = 0;
  // tlbflush (PFLUSH): ATC flush; cpush/cinv have no functional effect
  virtual void mmu_flush() {}
};

struct Machine {
  Mem *mem = nullptr;
  uint32_t R[64];
  uint32_t csr[256];
  bool N, Z, V, C, X;
  uint16_t cur_op;
  uint32_t gpc_insn;
  uint32_t gnpc;
  bool gnpc_set;
  int fail = R_MATCH;
  std::string why;
  // millicode
  uint32_t cur_pc = 0, next_pc = 0;
  std::vector<uint32_t> cstack;
  bool done = false, raised = false;
  unsigned trap_vec = 0;   // a trap a guest uop raised, taken once the instruction retires
  uint32_t vector = 0;
  uint32_t mc_insns = 0;
  // full-format extension words seen by the length decoder
  int full_n = 0;
  int full_off[2];
  int full_bytes[2];
  int full_extra = 0;

  void failf(int r, const char *fmt = nullptr, uint32_t v = 0);
  int fc_space(int space, bool write) const;   // function code for a uisa address space
  uint32_t ext16(int off);
  uint32_t ext32(int off);
  uint32_t ext16_or_zero(int off);
  uint32_t imm(const uisa_fuop &u);
  uint32_t imm_at(const uisa_fuop &u);
  uint32_t load(int fc, uint32_t addr, int n);
  void store(int fc, uint32_t addr, int n, uint32_t v) { mem->store(fc, addr, n, v); }
  uint32_t index_reg(uint32_t w);
  uint32_t index(int off, uint32_t *d8);
  static int full_disp_bytes(uint32_t w);
  void scan_full(int off);
  int adj(int off) const;
  bool is_full(int off) const;
  uint32_t full_ea(int off, uint32_t base);
  void step(const uisa_fuop &u);
  void base(const uisa_fuop &u, uint32_t a, uint32_t b, uint32_t IMM);
  void cstack_push(uint32_t v);
  uint32_t cstack_pop();
  void raise(uint32_t vec);
  void run_millicode(uint32_t entry, int ilen);
  void run_from(uint32_t mpc);
  uint32_t ea_control(int mode, int reg, int off);
  void run_movem();
};

// The flow the front end picks for an opword (second-level XSEL decode
// included, which may look at the first extension word).
struct Flow {
  int kind;
  const uisa_fuop *uops;
  int len;
  uint32_t xreg, xreg2;   // G_XREG / G_XREG2 substitutions
  int ilen;               // instruction length without full-format extension words
  int32_t mc_entry;       // millicode routine (K_MILLICODE), or < 0 if none
};
Flow select_flow(uint16_t op, uint16_t sr, uint32_t pc, Mem &mem);

// Load the machine from a pre-instruction state.
void setup(Machine &m, Mem &mem, uint16_t op, const uisa_m68k_state &pre);
// Run a PLA, sequencer or millicode flow; returns the model's next PC.
uint32_t exec_flow(Machine &m, const Flow &f);
// Exception processing outside an instruction: an access fault (vector 2,
// frame fields from `f`) or an interrupt at `level` with `vector`.
void exec_event(Machine &m, int vector, int level, const uisa_fault *f);
// Write the machine's guest state back.
void writeback(const Machine &m, uint32_t model_pc, uisa_m68k_state &s);
// Compare the machine with a post-instruction state (stores excluded).
int compare_regs(const Machine &m, const uisa_m68k_state &post, uint32_t model_pc);
int active_stack(uint16_t sr);

// Counters across all instructions run.
extern uint64_t pla_uops_total, seq_uops_total, mc_insns_total;
extern uint64_t *const mc_prof;     // executions per millicode word
extern const size_t mc_words;
extern const int32_t *const mc_entry_table;   // MC_ENTRY

}  // namespace uisa_model

#endif
