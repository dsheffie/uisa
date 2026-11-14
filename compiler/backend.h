// -*-c++-*-
/* backend.h: instruction selection, register allocation, uisa assembly. */
#ifndef UCC_BACKEND_H
#define UCC_BACKEND_H

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "tac.h"

// Make every operand directly encodable: constants that do not fit an
// immediate field become registers, zero becomes x0, address arithmetic
// folds into memory displacements, guest-register reads feed their uses.
void legalize(Function *f);

// The whole-program register allocation problem.
struct AllocProblem {
  struct Pref { Reg *a, *b; int weight; };
  struct CallSite { Function *caller, *callee; std::vector<Reg *> liveAcross; int line; };
  std::vector<Reg *> vals;                          // every value of every function
  std::map<Function *, std::vector<Reg *>> fvals;   // per function
  std::set<std::pair<Reg *, Reg *>> edges;          // interference (a < b by id order of vals)
  std::vector<Pref> prefs;
  std::vector<CallSite> calls;
  std::map<Function *, int> maxLive;
};

// Liveness, interference, copy preferences and live-across-call sets.
void buildAllocProblem(Program *p, AllocProblem &ap);
// Check register pressure against the register file; hard errors, no spilling.
void checkPressure(Program *p, AllocProblem &ap);
// Assign Reg::color for every value (regalloc.cpp, z3).
void allocateRegisters(Program *p, AllocProblem &ap, int timeoutMs, bool verbose);
// Check that coloring satisfies every constraint (independent of the solver).
void verifyAllocation(Program *p, AllocProblem &ap);
// Emit uisa assembly text.
void emitProgram(Program *p, FILE *out, const char *srcName);

int numColors();   // allocatable x registers (x1..x31)

#endif
