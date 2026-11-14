// Drives the shadow model with hand-built instructions (pre-state, access
// log, post-state), as the host emulator would.
#include <cstdio>
#include <cstring>
#include "uisa_shadow.h"

static int fails = 0;

struct Insn {
  const char *name;
  uint16_t op;
  uisa_m68k_state pre, post;
  struct { int kind, size; uint32_t addr, value; } log[8];
  int expect;   // 0 = match, otherwise the report column expected to count it
};

static uisa_m68k_state st(uint32_t pc, uint16_t sr) {
  uisa_m68k_state s;
  memset(&s, 0, sizeof s);
  s.pc = pc;
  s.sr = sr;
  return s;
}

static void run(Insn &t) {
  uisa_shadow_pre(t.op, &t.pre);
  for (auto &l : t.log)
    if (l.size) uisa_shadow_log(l.kind, l.addr, l.size, l.value);
  uisa_shadow_post(&t.post);
  if (uisa_shadow_count(t.op, t.expect) != 1) {
    printf("FAIL: %s (opword %04x) expected result %d\n", t.name, t.op, t.expect);
    fails++;
  }
}

int main() {
  uisa_shadow_init(nullptr);
  {  // MOVE.L (A0)+,D1: load, post-increment, N set
    Insn t{"move.l (a0)+,d1", 0x2218, st(0x100, 0x2700), st(0x102, 0x2708), {{UISA_LOG_READ, 4, 0x1000, 0x80000001}}, 0};
    t.pre.r[8] = 0x1000;
    t.post.r[8] = 0x1004; t.post.r[1] = 0x80000001;
    run(t);
  }
  {  // BSR.W +0x20: push return address, branch
    Insn t{"bsr.w", 0x6100, st(0x200, 0x2700), st(0x222, 0x2700),
           {{UISA_LOG_IFETCH, 2, 0x202, 0x0020}, {UISA_LOG_WRITE, 4, 0x7ffc, 0x204}}, 0};
    t.pre.r[15] = 0x8000;
    t.post.r[15] = 0x7ffc;
    run(t);
  }
  {  // LINK.W A6,#-8
    Insn t{"link.w a6,#-8", 0x4e56, st(0x300, 0x2704), st(0x304, 0x2704),
           {{UISA_LOG_IFETCH, 2, 0x302, 0xfff8}, {UISA_LOG_WRITE, 4, 0x7ffc, 0x5000}}, 0};
    t.pre.r[15] = 0x8000; t.pre.r[14] = 0x5000;
    t.post.r[15] = 0x7ff4; t.post.r[14] = 0x7ffc;
    run(t);
  }
  {  // ADD.B D1,D0 merges into the low byte; carry out
    Insn t{"add.b d1,d0", 0xd001, st(0x400, 0x2700), st(0x402, 0x2715), {}, 0};
    t.pre.r[0] = 0x123456ff; t.pre.r[1] = 0x00000001;
    t.post.r[0] = 0x12345600; t.post.r[1] = 1;
    run(t);
  }
  {  // MOVE.B (d8,A0,D1.W*2),D2: brief-format index
    Insn t{"move.b (d8,a0,d1.w*2),d2", 0x1430, st(0x500, 0x2700), st(0x504, 0x2700),
           {{UISA_LOG_IFETCH, 2, 0x502, 0x1204}, {UISA_LOG_READ, 1, 0x1000 + 2 * 3 + 4, 0x7f}}, 0};
    t.pre.r[8] = 0x1000; t.pre.r[1] = 0xffff0003;
    t.post.r[8] = 0x1000; t.post.r[1] = 0xffff0003; t.post.r[2] = 0x7f;
    run(t);
  }
  {  // deliberate mismatch: host says D1 is different
    Insn t{"move.l d0,d1 (bad host)", 0x2200, st(0x600, 0x2700), st(0x602, 0x2700), {}, 1};
    t.pre.r[0] = 5;
    t.post.r[0] = 5; t.post.r[1] = 6;
    run(t);
  }
  printf("test_shadow: %d failures\n", fails);
  return fails ? 1 : 0;
}
