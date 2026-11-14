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

// ---- randomized checks against C references of Previous's 68040 semantics ----
static uint32_t rng_state = 12345;
static uint32_t rnd() { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }
static uint32_t rnd_val() {   // bias towards small, large and boundary values
  switch (rnd() % 6) {
  case 0: return rnd() % 16;
  case 1: return 0u - (rnd() % 16);
  case 2: return 0x80000000u + (rnd() % 4) - 2;
  case 3: return rnd() >> (rnd() % 32);
  default: return rnd();
  }
}

static int ref_fail(const char *what, uint16_t op, uint16_t ext, int r) {
  printf("FAIL: %s opword %04x ext %04x result %d\n", what, op, ext, r);
  return 1;
}

// DIVU.L/DIVS.L D<s>,... (Previous m68k_divl, 68040 flags). Returns false on
// a zero divisor (not checked here).
static bool ref_divl(uisa_m68k_state &st, uint32_t src, uint16_t extra) {
  int dq = (extra >> 12) & 7, dr = extra & 7;
  uint16_t ccr = st.sr & 0x1f;
  if (src == 0) return false;
  bool ovf = false;
  uint32_t q32 = 0, r32 = 0;
  if (extra & 0x800) {
    int64_t a = (int32_t)st.r[dq];
    if (extra & 0x400) { a &= 0xffffffffll; a |= (int64_t)((uint64_t)st.r[dr] << 32); }
    if ((uint64_t)a == 0x8000000000000000ull && src == ~0u) ovf = true;
    else {
      int64_t rem = a % (int64_t)(int32_t)src, quot = a / (int64_t)(int32_t)src;
      if ((quot & (int64_t)0xffffffff80000000ll) != 0 && (quot & (int64_t)0xffffffff80000000ll) != (int64_t)0xffffffff80000000ll) ovf = true;
      else { q32 = (uint32_t)quot; r32 = (uint32_t)rem; }
    }
  } else {
    uint64_t a = st.r[dq];
    if (extra & 0x400) a |= (uint64_t)st.r[dr] << 32;
    uint64_t rem = a % src, quot = a / src;
    if (quot > 0xffffffffu) ovf = true;
    else { q32 = (uint32_t)quot; r32 = (uint32_t)rem; }
  }
  if (ovf) ccr = (ccr & ~1) | 2;
  else {
    ccr &= 0x10;
    if (q32 == 0) ccr |= 4;
    if ((int32_t)q32 < 0) ccr |= 8;
    st.r[dr] = r32;
    st.r[dq] = q32;
  }
  st.sr = (st.sr & 0xff00) | ccr;
  return true;
}

// 40-bit big-endian view of up to 5 bytes for memory bit fields
static uint32_t ref_bf_insert_flags(uint32_t field, int width, uint16_t &ccr) {
  ccr &= 0x10;
  if (field == 0) ccr |= 4;
  if (field & (1u << (width - 1))) ccr |= 8;
  return field;
}
static uint32_t ref_bf_new(int kind, uint32_t old, uint32_t ins, int width) {
  uint32_t m = 0xffffffffu >> (32 - width);
  switch (kind) {
  case 2: return old ^ m;
  case 4: return 0;
  case 6: return m;
  default: return ins & m;
  }
}

static int random_divl(int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    uint16_t ext = (uint16_t)(((rnd() % 8) << 12) | (rnd() % 2 ? 0x800 : 0) | (rnd() % 2 ? 0x400 : 0));
    ext |= rnd() % 8;
    if (!(ext & 0x400)) ext = (ext & ~7) | ((ext >> 12) & 7);   // 32-bit form: Dr = Dq
    int sreg = rnd() % 8;
    uint16_t op = 0x4c40 | sreg;
    uisa_m68k_state pre = st(0x2000, 0x2700 | (rnd() & 0x1f));
    for (int r = 0; r < 8; r++) pre.r[r] = rnd_val();
    pre.r[15] = 0x8000;
    if (pre.r[sreg] == 0) pre.r[sreg] = 7;
    uisa_m68k_state post = pre;
    ref_divl(post, pre.r[sreg], ext);
    post.pc = 0x2004;
    uint64_t before = uisa_shadow_count(op, 0);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_log(UISA_LOG_IFETCH, 0x2002, 2, ext);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random divl", op, ext, 0);
  }
  return bad;
}

static int random_divw(int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    bool sgn = rnd() % 2;
    int sreg = rnd() % 8, dreg = rnd() % 8;
    uint16_t op = (uint16_t)(0x80c0 | (sgn ? 0x100 : 0) | (dreg << 9) | sreg);
    uisa_m68k_state pre = st(0x2200, 0x2700 | (rnd() & 0x1f));
    for (int r = 0; r < 8; r++) pre.r[r] = rnd_val();
    pre.r[15] = 0x8000;
    if ((pre.r[sreg] & 0xffff) == 0) pre.r[sreg] |= 3;
    uisa_m68k_state post = pre;
    uint32_t dst = pre.r[dreg];
    uint16_t src = pre.r[sreg] & 0xffff, ccr = pre.sr & 0x1f;
    bool ovf;
    uint32_t newv = 0;
    if (!sgn) {
      uint32_t q = dst / src, rm = dst % src;
      ovf = q > 0xffff;
      newv = (q & 0xffff) | (rm << 16);
    } else if (dst == 0x80000000u && (int16_t)src == -1) {
      ovf = true;
    } else {
      int32_t q = (int32_t)dst / (int16_t)src;
      uint16_t rm = (uint16_t)((int32_t)dst % (int16_t)src);
      ovf = (q & 0xffff8000) != 0 && (q & 0xffff8000) != (int32_t)0xffff8000;
      newv = ((uint32_t)q & 0xffff) | ((uint32_t)rm << 16);
    }
    if (ovf) ccr = (ccr & ~1) | 2;
    else {
      ccr &= 0x10;
      if ((newv & 0xffff) == 0) ccr |= 4;
      if (newv & 0x8000) ccr |= 8;
      post.r[dreg] = newv;
    }
    post.sr = (post.sr & 0xff00) | ccr;
    post.pc = 0x2202;
    uint64_t before = uisa_shadow_count(op, 0);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random divw", op, 0, 0);
  }
  return bad;
}

static int random_bfwrite(int n) {
  int bad = 0;
  const int kinds[4] = {2, 4, 6, 7};   // chg clr set ins
  for (int i = 0; i < n; i++) {
    int kind = kinds[rnd() % 4];
    bool mem = rnd() % 2;
    int dreg = rnd() % 8, sreg = rnd() % 8, oreg = rnd() % 8, wreg = rnd() % 8;
    uint16_t op = (uint16_t)(0xe8c0 | (kind << 8) | (mem ? 0x10 | 0 : dreg));   // Dn or (A0)
    uint16_t ext = (uint16_t)((kind == 7 ? sreg << 12 : 0) | (rnd() % 2 ? 0x800 | (oreg << 6) : (rnd() % 32) << 6) |
                              (rnd() % 2 ? 0x20 | wreg : rnd() % 32));
    uisa_m68k_state pre = st(0x2100, 0x2700 | (rnd() & 0x1f));
    for (int r = 0; r < 8; r++) pre.r[r] = rnd_val();
    if (mem && (ext & 0x800)) pre.r[oreg] = (uint32_t)((int32_t)(rnd() % 200) - 100);   // small signed offsets
    pre.r[8] = 0x10000;
    pre.r[15] = 0x8000;
    uisa_m68k_state post = pre;
    int32_t offset = (ext & 0x800) ? (int32_t)pre.r[(ext >> 6) & 7] : (ext >> 6) & 31;
    int width = ((((ext & 0x20) ? pre.r[ext & 7] : ext) - 1) & 31) + 1;
    uint16_t ccr = pre.sr & 0x1f;
    uint32_t ins = pre.r[(ext >> 12) & 7];
    uint8_t bytes[5] = {0};
    uint32_t addr = 0;
    int nb = 0;
    if (!mem) {
      uint32_t v = pre.r[dreg];
      int o = offset & 31;
      uint32_t t = o ? (v << o) | (v >> (32 - o)) : v;
      uint32_t old = t >> (32 - width);
      ref_bf_insert_flags(kind == 7 ? ins & (0xffffffffu >> (32 - width)) : old, width, ccr);
      uint32_t nf = ref_bf_new(kind, old, ins, width);
      uint32_t keep = width == 32 ? 0 : t & ((1u << (32 - width)) - 1);
      t = keep | (width == 32 ? nf : nf << (32 - width));
      post.r[dreg] = o ? (t >> o) | (t << (32 - o)) : t;
    } else {
      addr = pre.r[8] + (offset >> 3);
      int bo = offset & 7;
      nb = (bo + width + 7) >> 3;
      uint64_t v = 0;
      for (int b = 0; b < 5; b++) { bytes[b] = (uint8_t)rnd(); v = (v << 8) | bytes[b]; }
      int sh = 40 - bo - width;
      uint64_t m = ((1ull << width) - 1) << sh;
      uint32_t old = (uint32_t)((v & m) >> sh);
      ref_bf_insert_flags(kind == 7 ? ins & (0xffffffffu >> (32 - width)) : old, width, ccr);
      uint64_t nv = (v & ~m) | ((uint64_t)ref_bf_new(kind, old, ins, width) << sh);
      uint64_t before = uisa_shadow_count(op, 0);
      post.sr = (post.sr & 0xff00) | ccr;
      post.pc = 0x2104;
      uisa_shadow_pre(op, &pre);
      uisa_shadow_log(UISA_LOG_IFETCH, 0x2102, 2, ext);
      for (int b = 0; b < nb; b++) uisa_shadow_log(UISA_LOG_READ, addr + b, 1, bytes[b]);
      for (int b = 0; b < nb; b++) uisa_shadow_log(UISA_LOG_WRITE, addr + b, 1, (uint32_t)(nv >> (32 - 8 * b)) & 0xff);
      uisa_shadow_post(&post);
      if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random bf (mem)", op, ext, 0);
      continue;
    }
    post.sr = (post.sr & 0xff00) | ccr;
    post.pc = 0x2104;
    uint64_t before = uisa_shadow_count(op, 0);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_log(UISA_LOG_IFETCH, 0x2102, 2, ext);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random bf (reg)", op, ext, 0);
  }
  return bad;
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
  // --- bitfield millicode (compiled from millicode/bitfield.c) ---
  {  // BFEXTS D1{4:8},D2: sign-extends, N set
    Insn t{"bfexts d1{4:8},d2", 0xebc1, st(0xd10, 0x2700), st(0xd14, 0x2708), {{UISA_LOG_IFETCH, 2, 0xd12, 0x2108}}, 0};
    t.pre.r[1] = 0x0ab00000; t.post.r[1] = 0x0ab00000; t.post.r[2] = 0xffffffab;
    run(t);
  }
  {  // BFEXTS D7{28:8},D2: register fields wrap around bit 0
    Insn t{"bfexts d7{28:8},d2", 0xebc7, st(0xd20, 0x2711), st(0xd24, 0x2718), {{UISA_LOG_IFETCH, 2, 0xd22, 0x2708}}, 0};
    t.pre.r[7] = 0x8000000f; t.post.r[7] = 0x8000000f; t.post.r[2] = 0xfffffff8;
    run(t);
  }
  {  // BFFFO D3{8:16},D4: first one bit at offset 19
    Insn t{"bfffo d3{8:16},d4", 0xedc3, st(0xd30, 0x2704), st(0xd34, 0x2700), {{UISA_LOG_IFETCH, 2, 0xd32, 0x4210}}, 0};
    t.pre.r[3] = 0x00001000; t.post.r[3] = 0x00001000; t.post.r[4] = 19;
    run(t);
  }
  {  // BFFFO D5{8:16},D4 on an empty field: offset + width, Z set
    Insn t{"bfffo d5{8:16},d4 (zero)", 0xedc5, st(0xd40, 0x2700), st(0xd44, 0x2704), {{UISA_LOG_IFETCH, 2, 0xd42, 0x4210}}, 0};
    t.post.r[4] = 24;
    run(t);
  }
  {  // BFTST (A0){D5:8} with D5 = -3: a negative offset reaches the byte below A0
    Insn t{"bftst (a0){d5:8}", 0xe8d0, st(0xd50, 0x2700), st(0xd54, 0x2708),
           {{UISA_LOG_IFETCH, 2, 0xd52, 0x0948}, {UISA_LOG_READ, 1, 0x2fff, 0x07}, {UISA_LOG_READ, 1, 0x3000, 0xc0}}, 0};
    t.pre.r[8] = 0x3000; t.pre.r[5] = 0xfffffffd;
    t.post.r[8] = 0x3000; t.post.r[5] = 0xfffffffd;
    run(t);
  }
  {  // BFEXTU (d16,A1){7:32},D6: a 32-bit field spanning five bytes
    Insn t{"bfextu (d16,a1){7:32},d6", 0xe9e9, st(0xd60, 0x2700), st(0xd66, 0x2708),
           {{UISA_LOG_IFETCH, 2, 0xd62, 0x61c0}, {UISA_LOG_IFETCH, 2, 0xd64, 0x0010},
            {UISA_LOG_READ, 1, 0x4010, 0x01}, {UISA_LOG_READ, 1, 0x4011, 0x23}, {UISA_LOG_READ, 1, 0x4012, 0x45},
            {UISA_LOG_READ, 1, 0x4013, 0x67}, {UISA_LOG_READ, 1, 0x4014, 0x89}}, 0};
    t.pre.r[9] = 0x4000; t.post.r[9] = 0x4000; t.post.r[6] = 0x91a2b3c4;
    run(t);
  }
  {  // BFEXTU (d8,A0,D1.W*4){0:16},D3: brief-format index
    Insn t{"bfextu (d8,a0,d1.w*4){0:16},d3", 0xe9f0, st(0xd70, 0x2700), st(0xd76, 0x2708),
           {{UISA_LOG_IFETCH, 2, 0xd72, 0x3010}, {UISA_LOG_IFETCH, 2, 0xd74, 0x1408},
            {UISA_LOG_READ, 1, 0x1010, 0xbe}, {UISA_LOG_READ, 1, 0x1011, 0xef}}, 0};
    t.pre.r[8] = 0x1000; t.pre.r[1] = 0xffff0002;
    t.post.r[8] = 0x1000; t.post.r[1] = 0xffff0002; t.post.r[3] = 0xbeef;
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
  {  // DIVU.W D1,D0 by zero: C cleared, vector 5 with a format 2 frame
    Insn t{"divu.w d1,d0 (zero)", 0x80c1, st(0x1800, 0x2701), st(0x6200, 0x2700),
           {{UISA_LOG_READ, 4, 0x1014, 0x6200}, {UISA_LOG_WRITE, 2, 0x7ff4, 0x2700}, {UISA_LOG_WRITE, 4, 0x7ff6, 0x1802},
            {UISA_LOG_WRITE, 2, 0x7ffa, 0x2014}, {UISA_LOG_WRITE, 4, 0x7ffc, 0x1800}}, 0};
    t.pre.r[15] = 0x8000; t.pre.ctl[UISA_CTL_VBR] = 0x1000; t.pre.r[0] = 100;
    t.post.r[15] = 0x7ff4; t.post.ctl[UISA_CTL_VBR] = 0x1000; t.post.r[0] = 100;
    run(t);
  }
  {  // DIVS.W #-3,D2: 100 / -3 = -33 remainder 1
    Insn t{"divs.w #-3,d2", 0x85fc, st(0x1900, 0x2700), st(0x1904, 0x2708), {{UISA_LOG_IFETCH, 2, 0x1902, 0xfffd}}, 0};
    t.pre.r[2] = 100; t.post.r[2] = 0x0001ffdf;
    run(t);
  }
  {  // DIVU.W D1,D0 overflow: D0, N and Z unchanged, V set
    Insn t{"divu.w d2,d0 (overflow)", 0x80c2, st(0x1a00, 0x270d), st(0x1a02, 0x270e), {}, 0};
    t.pre.r[0] = 0x00100000; t.pre.r[2] = 2; t.post.r[0] = 0x00100000; t.post.r[2] = 2;
    run(t);
  }
  {  // BFINS D3,D1{4:8}
    Insn t{"bfins d3,d1{4:8}", 0xefc1, st(0x1b00, 0x2700), st(0x1b04, 0x2708), {{UISA_LOG_IFETCH, 2, 0x1b02, 0x3108}}, 0};
    t.pre.r[1] = 0xffffffff; t.pre.r[3] = 0x1ab;
    t.post.r[1] = 0xfabfffff; t.post.r[3] = 0x1ab;
    run(t);
  }
  fails += random_divl(20000);
  fails += random_divw(20000);
  fails += random_bfwrite(20000);
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
