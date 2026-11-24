/* uisa execution engine: run 68k instructions as uisa code against a host's
 * real memory (as opposed to the shadow, which replays the host's access log).
 *
 * The host fetches the opword and calls engine_step() with the guest state.
 * The engine picks the instruction's flow (PLA, MOVEM sequencer or
 * millicode), runs it on a copy of the state with loads going to the host,
 * and buffers its stores. Nothing is committed by the engine:
 *
 *   E_DONE      the new state is in `s`; the host performs `stores` in order
 *               (checking that none faults first) and retires the instruction
 *   E_FAULT     a load or extension-word fetch faulted; `s` is unchanged and
 *               the host takes the access fault (or re-runs the instruction
 *               on its own interpreter, which takes it)
 *   E_FALLBACK  the engine cannot run this instruction (no flow, a millicode
 *               operation with no routine yet, a form the routine rejects);
 *               `s` is unchanged and the host runs it itself
 *
 * Loads are not undone on a fault or fallback, so a host whose loads have
 * side effects (device registers) sees them again when it re-runs the
 * instruction. Instructions the engine cannot run are known from the opword
 * before any load (`kind`, `mc_entry`), so this is rare.
 */
#ifndef UISA_ENGINE_H
#define UISA_ENGINE_H

#include <stdint.h>

#include <string>
#include <vector>

#include "uisa_shadow.h"

namespace uisa_model {

struct EngineHost {
  virtual ~EngineHost() {}
  // a guest access of n bytes (1, 2 or 4; any alignment) with function code
  // fc, through the MMU; false on an access fault
  virtual bool read(int fc, uint32_t va, int n, uint32_t &v) = 0;
  // probe.r / probe.w: the table walk's MMUSR, without touching memory
  virtual uint32_t probe(int fc, uint32_t va, bool write) = 0;
  // PFLUSH
  virtual void mmu_flush() = 0;
};

struct EngineStore { uint8_t fc, size; uint32_t addr, value; };

enum { E_DONE, E_FAULT, E_FALLBACK };

struct EngineResult {
  int status = E_DONE;
  int kind = 0;                     // K_PLA, K_SEQUENCER, K_MILLICODE, ...
  int fail = 0;                     // R_* when not E_DONE
  std::string why;
  std::vector<EngineStore> stores;  // program order, not yet performed
  bool raised = false;              // the instruction took an exception
  uint32_t vector = 0;
  uint32_t uops = 0;                // PLA/sequencer uops or millicode words run
};

// Run the instruction at s.pc whose opword is `op`.
int engine_step(uint16_t op, uisa_m68k_state &s, EngineHost &host, EngineResult &r);

// The flow kind the engine would use for `op` (no memory access beyond the
// first extension word for second-level decode).
const char *engine_kind_name(int kind);

}  // namespace uisa_model

#endif
