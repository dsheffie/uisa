// -*-c++-*-
/* codegen.h: CFG cleanup, SSA construction and SSA-level cleanups.
 *
 * Forked from Decaf's codegen.cpp: the Lengauer-Tarjan dominator tree,
 * the dominance frontiers, the liveness-pruned phi insertion and the
 * dominator-tree renaming are Decaf's algorithms; the LLVM emission is gone.
 */
#ifndef UCC_CODEGEN_H
#define UCC_CODEGEN_H

#include "tac.h"

// Remove unreachable blocks, thread trivial jumps, merge straight-line
// blocks, split critical edges. Before SSA.
void cleanCFG(Function *f);
// Build pruned SSA form. Reports uses of unassigned variables.
void buildSSA(Function *f);
// Copy propagation, constant folding, trivial phis, dead code. On SSA.
void optimizeSSA(Function *f);
// Remove instructions whose results are unused and that have no effect.
bool eliminateDeadCode(Function *f);
// Loop nesting depth of every block (for spill-free move weighting).
void computeLoopDepth(Function *f);
// Rebuild every Reg::def pointer.
void recomputeDefs(Function *f);

#endif
