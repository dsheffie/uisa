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
  struct { int kind, size; uint32_t addr, value; } log[12];
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
    printf("FAIL: %s (opword %04x) expected result %d; got:", t.name, t.op, t.expect);
    for (int r = 0; r < 16; r++)
      if (uisa_shadow_count(t.op, r)) printf(" [%d]=%llu", r, (unsigned long long)uisa_shadow_count(t.op, r));
    printf("\n");
    fails++;
  }
}

int main() {
  uisa_shadow_init("build/test_shadow_report.txt");
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
  {  // MOVEM.L D0-D1/A6,-(A7): stores A6, D1, D0 downwards
    Insn t{"movem.l d0-d1/a6,-(a7)", 0x48e7, st(0x700, 0x2700), st(0x704, 0x2700),
           {{UISA_LOG_IFETCH, 2, 0x702, 0xc002}, {UISA_LOG_WRITE, 4, 0x7ffc, 0x66}, {UISA_LOG_WRITE, 4, 0x7ff8, 0x11},
            {UISA_LOG_WRITE, 4, 0x7ff4, 0x10}}, 0};
    t.pre.r[0] = 0x10; t.pre.r[1] = 0x11; t.pre.r[14] = 0x66; t.pre.r[15] = 0x8000;
    t.post = t.pre; t.post.pc = 0x704; t.post.r[15] = 0x7ff4;
    run(t);
  }
  {  // MOVEM.W (A7)+,D0/A1: word loads sign-extend into whole registers
    Insn t{"movem.w (a7)+,d0/a1", 0x4c9f, st(0x800, 0x2700), st(0x804, 0x2700),
           {{UISA_LOG_IFETCH, 2, 0x802, 0x0201}, {UISA_LOG_READ, 2, 0x7000, 0x8001}, {UISA_LOG_READ, 2, 0x7002, 0x0002}}, 0};
    t.pre.r[15] = 0x7000; t.pre.r[0] = 0x12345678;
    t.post = t.pre; t.post.pc = 0x804; t.post.r[0] = 0xffff8001; t.post.r[9] = 2; t.post.r[15] = 0x7004;
    run(t);
  }
  {  // MOVE.W SR,D0 (supervisor): millicode
    Insn t{"move.w sr,d0", 0x40c0, st(0x900, 0x2715), st(0x902, 0x2715), {}, 0};
    t.pre.r[0] = 0xaaaa0000;
    t.post.r[0] = 0xaaaa2715;
    run(t);
  }
  {  // MOVE.W SR,D1 in user mode: privilege violation, format 0 frame on the ISP
    Insn t{"move.w sr,d1 (user)", 0x40c1, st(0x980, 0x0000), st(0x5000, 0x2000),
           {{UISA_LOG_READ, 4, 0x1020, 0x5000}, {UISA_LOG_WRITE, 2, 0x7ff8, 0x0000}, {UISA_LOG_WRITE, 4, 0x7ffa, 0x980},
            {UISA_LOG_WRITE, 2, 0x7ffe, 0x0020}}, 0};
    t.pre.r[15] = 0x4000; t.pre.ctl[UISA_CTL_ISP] = 0x8000; t.pre.ctl[UISA_CTL_VBR] = 0x1000;
    t.post.r[15] = 0x7ff8; t.post.ctl[UISA_CTL_USP] = 0x4000; t.post.ctl[UISA_CTL_VBR] = 0x1000;
    run(t);
  }
  {  // TRAP #3 from user mode: stacks the next PC, vector 35
    Insn t{"trap #3", 0x4e43, st(0x1200, 0x001f), st(0x6000, 0x201f),
           {{UISA_LOG_READ, 4, 0x108c, 0x6000}, {UISA_LOG_WRITE, 2, 0x7ff8, 0x001f}, {UISA_LOG_WRITE, 4, 0x7ffa, 0x1202},
            {UISA_LOG_WRITE, 2, 0x7ffe, 0x008c}}, 0};
    t.pre.r[15] = 0x4000; t.pre.ctl[UISA_CTL_ISP] = 0x8000; t.pre.ctl[UISA_CTL_VBR] = 0x1000;
    t.post.r[15] = 0x7ff8; t.post.ctl[UISA_CTL_USP] = 0x4000; t.post.ctl[UISA_CTL_VBR] = 0x1000;
    run(t);
  }
  {  // TRAPV with V set (supervisor): format 2 frame with the instruction address
    Insn t{"trapv", 0x4e76, st(0x1300, 0x2702), st(0x6100, 0x2702),
           {{UISA_LOG_READ, 4, 0x101c, 0x6100}, {UISA_LOG_WRITE, 2, 0x7ff4, 0x2702}, {UISA_LOG_WRITE, 4, 0x7ff6, 0x1302},
            {UISA_LOG_WRITE, 2, 0x7ffa, 0x201c}, {UISA_LOG_WRITE, 4, 0x7ffc, 0x1300}}, 0};
    t.pre.r[15] = 0x8000; t.pre.ctl[UISA_CTL_VBR] = 0x1000;
    t.post.r[15] = 0x7ff4; t.post.ctl[UISA_CTL_VBR] = 0x1000;
    run(t);
  }
  {  // TRAPEQ.W #imm with Z clear: no trap
    Insn t{"trapeq.w", 0x57fa, st(0x1380, 0x2700), st(0x1384, 0x2700), {{UISA_LOG_IFETCH, 2, 0x1382, 0x1234}}, 0};
    run(t);
  }
  {  // RTE format 0 to user mode: pops 8 bytes, switches to the USP
    Insn t{"rte (format 0)", 0x4e73, st(0x1400, 0x2700), st(0x2222, 0x0004),
           {{UISA_LOG_READ, 2, 0x7ff8, 0x0004}, {UISA_LOG_READ, 4, 0x7ffa, 0x2222}, {UISA_LOG_READ, 2, 0x7ffe, 0x0080}}, 0};
    t.pre.r[15] = 0x7ff8; t.pre.ctl[UISA_CTL_USP] = 0x4000;
    t.post.r[15] = 0x4000; t.post.ctl[UISA_CTL_USP] = 0x4000; t.post.ctl[UISA_CTL_ISP] = 0x8000;
    run(t);
  }
  {  // RTE format 7 (access error): pops 60 bytes and restarts the instruction
    Insn t{"rte (format 7)", 0x4e73, st(0x1500, 0x2700), st(0x3330, 0x2000),
           {{UISA_LOG_READ, 2, 0x7000, 0x2000}, {UISA_LOG_READ, 4, 0x7002, 0x3330}, {UISA_LOG_READ, 2, 0x7006, 0x7008},
            {UISA_LOG_READ, 2, 0x700c, 0x0125}}, 0};
    t.pre.r[15] = 0x7000; t.post.r[15] = 0x703c;
    // the same opword, so count a second match
    uisa_shadow_pre(t.op, &t.pre);
    for (auto &l : t.log)
      if (l.size) uisa_shadow_log(l.kind, l.addr, l.size, l.value);
    uisa_shadow_post(&t.post);
    if (uisa_shadow_count(t.op, 0) != 2) { printf("FAIL: %s\n", t.name); fails++; }
  }
  {  // MOVE.W #$0000,SR: supervisor -> user switches A7 to the USP
    Insn t{"move.w #0,sr", 0x46fc, st(0xa00, 0x2704), st(0xa04, 0x0000), {{UISA_LOG_IFETCH, 2, 0xa02, 0x0000}}, 0};
    t.pre.r[15] = 0x8000; t.pre.ctl[UISA_CTL_USP] = 0x4000;
    t.post.r[15] = 0x4000; t.post.ctl[UISA_CTL_ISP] = 0x8000; t.post.ctl[UISA_CTL_USP] = 0x4000;
    run(t);
  }
  {  // MOVEC VBR,D2
    Insn t{"movec vbr,d2", 0x4e7a, st(0xb00, 0x2700), st(0xb04, 0x2700), {{UISA_LOG_IFETCH, 2, 0xb02, 0x2801}}, 0};
    t.pre.ctl[UISA_CTL_VBR] = 0x1234; t.post.ctl[UISA_CTL_VBR] = 0x1234; t.post.r[2] = 0x1234;
    run(t);
  }
  {  // MOVEC A1,USP-via-MOVEC (code 0x800)
    Insn t{"movec a1,usp", 0x4e7b, st(0xc00, 0x2700), st(0xc04, 0x2700), {{UISA_LOG_IFETCH, 2, 0xc02, 0x9800}}, 0};
    t.pre.r[9] = 0x5555; t.post.r[9] = 0x5555; t.post.ctl[UISA_CTL_USP] = 0x5555;
    run(t);
  }
  {  // BFEXTU D1{4:8},D2: field of D1 starting 4 bits below the MSB
    Insn t{"bfextu d1{4:8},d2", 0xe9c1, st(0xd00, 0x2710), st(0xd04, 0x2718), {{UISA_LOG_IFETCH, 2, 0xd02, 0x2108}}, 0};
    t.pre.r[1] = 0x0ab00000; t.post.r[1] = 0x0ab00000; t.post.r[2] = 0xab;
    run(t);
  }
  {  // BFEXTU (A0){12:12},D3: spans three bytes in memory
    Insn t{"bfextu (a0){12:12},d3", 0xe9d0, st(0xe00, 0x2700), st(0xe04, 0x2700),
           {{UISA_LOG_IFETCH, 2, 0xe02, 0x330c}, {UISA_LOG_READ, 1, 0x2001, 0x0f}, {UISA_LOG_READ, 1, 0x2002, 0xff}}, 0};
    t.pre.r[8] = 0x2000; t.post.r[8] = 0x2000; t.post.r[3] = 0xfff; t.post.sr = 0x2708;
    run(t);
  }
  {  // DBEQ D0 with Z clear: decrement and branch back
    Insn t{"dbeq d0", 0x57c8, st(0xf00, 0x2700), st(0xf00 + 2 - 18, 0x2700), {{UISA_LOG_IFETCH, 2, 0xf02, 0xffee}}, 0};
    t.pre.r[0] = 0x00010005; t.post.r[0] = 0x00010004;
    run(t);
  }
  {  // MULU.L D1,D3:D2 (64-bit result)
    Insn t{"mulu.l d1,d3:d2", 0x4c01, st(0x1000, 0x2700), st(0x1004, 0x2700), {{UISA_LOG_IFETCH, 2, 0x1002, 0x2403}}, 0};
    t.pre.r[1] = 0x10000; t.pre.r[2] = 0x30000;
    t.post.r[1] = 0x10000; t.post.r[2] = 0; t.post.r[3] = 3;
    run(t);
  }
  {  // CMPM.B (A0)+,(A1)+: equal bytes set Z
    Insn t{"cmpm.b (a0)+,(a1)+", 0xb308, st(0x1100, 0x2700), st(0x1102, 0x2704),
           {{UISA_LOG_READ, 1, 0x3000, 0x41}, {UISA_LOG_READ, 1, 0x4000, 0x41}}, 0};
    t.pre.r[8] = 0x3000; t.pre.r[9] = 0x4000; t.post.r[8] = 0x3001; t.post.r[9] = 0x4001;
    run(t);
  }
  {  // level 6 interrupt from user mode: autovector 30, format 0 frame on the ISP, mask = 6
    uisa_m68k_state pre = st(0x1600, 0x0004), post = st(0x7000, 0x2600);
    pre.r[15] = 0x4000; pre.ctl[UISA_CTL_ISP] = 0x8000; pre.ctl[UISA_CTL_VBR] = 0x1000;
    post.r[15] = 0x7ff8; post.ctl[UISA_CTL_USP] = 0x4000; post.ctl[UISA_CTL_VBR] = 0x1000;
    post.sr = 0x2604;
    uisa_shadow_exc_pre(30, 6, &pre, nullptr);
    uisa_shadow_log(UISA_LOG_READ, 0x1078, 4, 0x7000);
    uisa_shadow_log(UISA_LOG_WRITE, 0x7ff8, 2, 0x0004);
    uisa_shadow_log(UISA_LOG_WRITE, 0x7ffa, 4, 0x1600);
    uisa_shadow_log(UISA_LOG_WRITE, 0x7ffe, 2, 0x0078);
    uisa_shadow_exc_post(&post);
    if (uisa_shadow_exc_count(30, 0) != 1) { printf("FAIL: level 6 interrupt\n"); fails++; }
  }
  {  // access fault (supervisor): format 7 frame from the fault fields
    uisa_m68k_state pre = st(0x1700, 0x2708), post = st(0x7100, 0x2708);
    pre.r[15] = 0x8000; pre.ctl[UISA_CTL_VBR] = 0x1000;
    post.r[15] = 0x8000 - 60; post.ctl[UISA_CTL_VBR] = 0x1000;
    uisa_fault f = {0x00c0ffee, 0x0105, 0x00c0ffec, 0, 0, 0x00a5, 0x12345678, {0, 0, 0, 0}};
    uint32_t a = 0x8000 - 60;
    uisa_shadow_exc_pre(2, 0, &pre, &f);
    uisa_shadow_log(UISA_LOG_READ, 0x1008, 4, 0x7100);
    uisa_shadow_log(UISA_LOG_WRITE, a + 0, 2, 0x2708);
    uisa_shadow_log(UISA_LOG_WRITE, a + 2, 4, 0x1700);
    uisa_shadow_log(UISA_LOG_WRITE, a + 6, 2, 0x7008);
    uisa_shadow_log(UISA_LOG_WRITE, a + 8, 4, f.ea);
    uisa_shadow_log(UISA_LOG_WRITE, a + 12, 2, f.ssw);
    uisa_shadow_log(UISA_LOG_WRITE, a + 14, 2, f.wb3_status);
    uisa_shadow_log(UISA_LOG_WRITE, a + 16, 2, 0);
    uisa_shadow_log(UISA_LOG_WRITE, a + 18, 2, 0);
    uisa_shadow_log(UISA_LOG_WRITE, a + 20, 4, f.addr);
    uisa_shadow_log(UISA_LOG_WRITE, a + 24, 4, f.addr);
    uisa_shadow_log(UISA_LOG_WRITE, a + 28, 4, f.wb3_data);
    for (int o = 32; o < 60; o += 4) uisa_shadow_log(UISA_LOG_WRITE, a + o, 4, 0);
    uisa_shadow_exc_post(&post);
    if (uisa_shadow_exc_count(2, 0) != 1) {
      printf("FAIL: access fault:");
      for (int r = 0; r < 16; r++)
        if (uisa_shadow_exc_count(2, r)) printf(" [%d]=%llu", r, (unsigned long long)uisa_shadow_exc_count(2, r));
      printf("\n");
      fails++;
    }
  }
  {  // deliberate mismatch: host says D1 is different
    Insn t{"move.l d0,d1 (bad host)", 0x2200, st(0x600, 0x2700), st(0x602, 0x2700), {}, 1};
    t.pre.r[0] = 5;
    t.post.r[0] = 5; t.post.r[1] = 6;
    run(t);
  }
  uisa_shadow_dump();
  printf("test_shadow: %d failures\n", fails);
  return fails ? 1 : 0;
}
