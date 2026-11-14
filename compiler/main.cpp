/* main.cpp: ucc, the uisa millicode compiler driver.
 *
 *   ucc [-o out.s] [-v] [--dump-tac] [-O0|-O2] [--rlimit n] file.c
 *
 * Pipeline: parse -> TAC -> CFG cleanup -> SSA -> SSA cleanups -> call-graph
 * checks (no recursion, return-stack depth) -> legalization -> liveness and
 * interference -> pressure check -> z3 allocation -> uisa assembly.
 */
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>

#include "ast.h"
#include "backend.h"
#include "codegen.h"
#include "errors.h"

namespace spec {
#include "uisa_intrinsics.inc"
}

const char *g_srcName = "<stdin>";
int g_errors = 0;

void error(int line, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (line > 0) fprintf(stderr, "%s:%d: error: ", g_srcName, line);
  else fprintf(stderr, "%s: error: ", g_srcName);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  g_errors++;
}

void fatal(int line, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (line > 0) fprintf(stderr, "%s:%d: error: ", g_srcName, line);
  else fprintf(stderr, "%s: error: ", g_srcName);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

extern FILE *yyin;
extern int yyparse();
extern TransUnit *parse_result;

static void stopOnErrors() {
  if (g_errors) {
    fprintf(stderr, "%s: %d error%s\n", g_srcName, g_errors, g_errors == 1 ? "" : "s");
    exit(1);
  }
}

// No recursion; every chain of nested calls fits the hardware return stack.
static void checkCallGraph(Program *p) {
  std::map<Function *, int> state, depth;
  std::vector<Function *> path;
  std::function<int(Function *)> visit = [&](Function *f) -> int {
    if (state[f] == 2) return depth[f];
    if (state[f] == 1) {
      std::string cyc;
      bool on = false;
      for (Function *g : path) {
        if (g == f) on = true;
        if (on) cyc += g->name + " -> ";
      }
      error(f->line, "recursion is not allowed in millicode: %s%s", cyc.c_str(), f->name.c_str());
      return 0;
    }
    state[f] = 1;
    path.push_back(f);
    int d = 0;
    for (Function *g : f->callees) d = std::max(d, 1 + visit(g));
    path.pop_back();
    state[f] = 2;
    return depth[f] = d;
  };
  for (Function *f : p->funcs) {
    int d = visit(f);
    int frames = f->isEntry ? d : d + 1;   // a helper is itself entered by a call
    if (frames > spec::CALL_STACK_DEPTH)
      error(f->line, "'%s' nests %d calls but the millicode return stack holds %d", f->name.c_str(), frames,
            spec::CALL_STACK_DEPTH);
  }
}

int main(int argc, char **argv) {
  const char *outName = nullptr, *inName = nullptr;
  bool verbose = false, dumpTac = false;
  unsigned rlimit = 300000000;  // z3 resource budget (-O2): deterministic, unlike a timeout
  bool optimize = false;        // -O0: greedy allocation (debug builds); -O2: z3 (deploy builds)
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc) outName = argv[++i];
    else if (!strcmp(argv[i], "-v")) verbose = true;
    else if (!strcmp(argv[i], "--dump-tac")) dumpTac = true;
    else if (!strcmp(argv[i], "-O0")) optimize = false;
    else if (!strcmp(argv[i], "-O2") || !strcmp(argv[i], "-O")) optimize = true;
    else if (!strcmp(argv[i], "--rlimit") && i + 1 < argc) rlimit = (unsigned)strtoul(argv[++i], nullptr, 0);
    else if (argv[i][0] == '-') {
      fprintf(stderr, "usage: ucc [-o out.s] [-v] [--dump-tac] [-O0|-O2] [--rlimit n] file.c\n");
      return 2;
    } else inName = argv[i];
  }
  if (!inName) {
    fprintf(stderr, "usage: ucc [-o out.s] [-v] [--dump-tac] [-O0|-O2] [--rlimit n] file.c\n");
    return 2;
  }
  g_srcName = inName;
  yyin = fopen(inName, "r");
  if (!yyin) {
    perror(inName);
    return 1;
  }
  yyparse();
  stopOnErrors();
  Program *prog = generateTAC(parse_result);
  stopOnErrors();
  for (Function *f : prog->funcs) cleanCFG(f);
  stopOnErrors();
  for (Function *f : prog->funcs) buildSSA(f);
  stopOnErrors();
  for (Function *f : prog->funcs) optimizeSSA(f);
  checkCallGraph(prog);
  stopOnErrors();
  for (Function *f : prog->funcs) {
    legalize(f);
    f->rebuildEdges();
    computeLoopDepth(f);
  }
  if (dumpTac)
    for (Function *f : prog->funcs) f->print(stderr);
  AllocProblem ap;
  buildAllocProblem(prog, ap);
  stopOnErrors();
  checkPressure(prog, ap);
  stopOnErrors();
  allocateRegisters(prog, ap, rlimit, optimize, verbose);
  stopOnErrors();
  verifyAllocation(prog, ap);
  FILE *out = outName ? fopen(outName, "w") : stdout;
  if (!out) {
    perror(outName);
    return 1;
  }
  emitProgram(prog, out, inName);
  if (outName) fclose(out);
  return 0;
}
