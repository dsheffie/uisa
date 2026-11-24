// -*-c++-*-
/* codegen.h: CFG cleanup, SSA construction and SSA-level cleanups.
 *
 * The Lengauer-Tarjan dominator tree, dominance frontiers, liveness-pruned
 * phi insertion and dominator-tree renaming, and the SSA optimizations.
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
// After legalization: merge repeated values and materialized constants
// (across the dominator tree when `global`, else within each block).
void valueNumberLegal(Function *f, bool global);
// After legalization: hoist loop-invariant values (constants included) to
// loop preheaders. undoHoists puts back every value hoisted so far
// (millicode, when the hoists push a function over the register budget).
void hoistLoopInvariants(Function *f);
bool undoHoists(Function *f);
// After legalization: a value (or constant) that every successor of a branch
// computes is computed once before the branch.
void hoistCommonLegal(Function *f);
// Remove instructions whose results are unused and that have no effect.
bool eliminateDeadCode(Function *f);
// Loop nesting depth of every block (for spill-free move weighting).
void computeLoopDepth(Function *f);
// Rebuild every Reg::def pointer.
void recomputeDefs(Function *f);

#endif
