// Differential test of ucc: every test program runs natively (host.h) and as
// compiled millicode on a small uisa ISS built from the generated decoder and
// semantics (build/uisa.h, build/uisa_sem.inc), on many random guest states.
// Guest registers, flags and the exit reason must agree.
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "host.h"
#include "iss.h"

namespace host {
uint32_t G[18], E[5], CCR;
}

struct UccTest {
  const char *name;
  const uint32_t *image;
  int words;
  int start;            // word index of the entry function
  void (*native)();
};

#include "ucc_tests.inc"

static uint32_t pick(std::mt19937 &rng) {
  static const uint32_t special[] = {0, 1, 2, 3, 7, 8, 31, 32, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff,
                                     0x10000, 0x7fffffff, 0x80000000u, 0xfffffffeu, 0xffffffffu};
  switch (rng() % 4) {
    case 0: return special[rng() % (sizeof special / sizeof special[0])];
    case 1: return rng() % 64;
    default: return rng();
  }
}

int main(int argc, char **argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 3000;
  int fails = 0;
  for (const UccTest &t : TESTS) {
    std::mt19937 rng(12345);
    int tfail = 0;
    unsigned maxSteps = 0, maxDepth = 0;
    for (int it = 0; it < iters && tfail < 3; it++) {
      uint32_t G0[18], E0[5], ccr0 = rng() & 31;
      for (auto &g : G0) g = pick(rng);
      E0[0] = 0;
      E0[1] = rng() & 0xffff;
      for (int k = 2; k < 5; k++) E0[k] = pick(rng);
      // native
      memcpy(host::G, G0, sizeof G0);
      memcpy(host::E, E0, sizeof E0);
      host::CCR = ccr0;
      int hexit = 0;
      try {
        t.native();
        hexit = -2;   // returned: not possible for an entry function
      } catch (host::Exit &e) {
        hexit = e.vector;
      }
      // compiled
      Machine m;
      for (int k = 0; k < 18; k++) m.R[32 + k] = G0[k];
      for (int k = 1; k < 5; k++) m.R[k] = E0[k];
      for (int k = 5; k < 32; k++) m.R[k] = rng();   // garbage in the scratch registers
      CCR_UNPACK(ccr0, m.X, m.N, m.Z, m.V, m.C);
      m.run(t.image, t.words, t.start);
      if (m.steps > maxSteps) maxSteps = m.steps;
      if (m.maxDepth > maxDepth) maxDepth = m.maxDepth;
      std::string why = m.err;
      if (why.empty() && m.exitv != hexit) why = "exit " + std::to_string(m.exitv) + " vs native " + std::to_string(hexit);
      for (int k = 0; k < 18 && why.empty() && hexit == -1; k++)
        if (m.R[32 + k] != host::G[k]) {
          char b[96];
          snprintf(b, sizeof b, "guest reg %d: ucc %08x native %08x", k, m.R[32 + k], host::G[k]);
          why = b;
        }
      uint32_t mccr = CCR_PACK(m.X, m.N, m.Z, m.V, m.C);
      if (why.empty() && hexit == -1 && mccr != host::CCR) why = "ccr";
      if (!why.empty()) {
        printf("FAIL %s iteration %d: %s\n  in:", t.name, it, why.c_str());
        for (int k = 0; k < 18; k++) printf(" %08x", G0[k]);
        printf("\n  entry: %08x %08x %08x %08x\n", E0[1], E0[2], E0[3], E0[4]);
        tfail++;
      }
    }
    printf("ucc test %-12s %s (%d words, up to %u steps, call depth %u)\n", t.name, tfail ? "FAILED" : "ok",
           t.words, maxSteps, maxDepth);
    fails += tfail;
  }
  printf("test_ucc: %d failures\n", fails);
  return fails ? 1 : 0;
}
