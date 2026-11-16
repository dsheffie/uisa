// Drives the shadow model with hand-built instructions (pre-state, access
// log, post-state), as the host emulator would.
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef BF_FULL_OK
#define BF_FULL_OK true   // millicode/bitfield.c handles full-format index words
#endif
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

// BFTST / BFEXTU / BFEXTS / BFFFO on a register or memory field, through
// several control addressing modes (including full-format index words).
// Returns failures; `full_ok` = whether full-format words must work.
static int random_bfread(int n, bool full_ok) {
  int bad = 0;
  const int kinds[4] = {0, 1, 3, 5};   // tst extu exts ffo
  for (int i = 0; i < n; i++) {
    int kind = kinds[rnd() % 4];
    int form = rnd() % 8;              // 0 Dn, 1 (A0), 2 (d16,A1), 3 (d8,A2,Xn), 4 abs.L, 5 (d16,PC), 6 (d8,PC,Xn), 7 full index
    int dreg = rnd() % 8, oreg = rnd() % 8, wreg = rnd() % 8, dn = rnd() % 8;
    static const uint16_t ea[8] = {0, 0x10, 0x29, 0x32, 0x39, 0x3a, 0x3b, 0x33};
    uint16_t op = (uint16_t)(0xe8c0 | (kind << 8) | (form ? ea[form] : dreg));
    uint16_t ext = (uint16_t)((dn << 12) | (rnd() % 2 ? 0x800 | (oreg << 6) : (rnd() % 32) << 6) |
                              (rnd() % 2 ? 0x20 | wreg : rnd() % 32));
    uint32_t pc = 0x2100;
    uisa_m68k_state pre = st(pc, 0x2700 | (rnd() & 0x1f));
    for (int r = 0; r < 8; r++) pre.r[r] = rnd_val();
    if (form && (ext & 0x800)) pre.r[oreg] = (uint32_t)((int32_t)(rnd() % 200) - 100);
    for (int r = 8; r < 15; r++) pre.r[r] = 0x10000 + 0x1000 * (r - 8);
    pre.r[15] = 0x8000;
    int32_t offset = (ext & 0x800) ? (int32_t)pre.r[(ext >> 6) & 7] : (ext >> 6) & 31;
    int width = ((((ext & 0x20) ? pre.r[ext & 7] : ext) - 1) & 31) + 1;
    std::vector<std::pair<uint32_t, uint32_t>> words;   // extension words after the bitfield word
    uint32_t addr = 0;
    int xr = rnd() % 8;                                // index register: D0-D7
    uint16_t xw;
    switch (form) {
    case 1: addr = pre.r[8]; break;
    case 2: { int16_t d = (int16_t)rnd(); words.push_back({0, (uint16_t)d}); addr = pre.r[9] + d; break; }
    case 3: case 6: {
      xw = (uint16_t)((xr << 12) | (rnd() & 0x0eff));
      words.push_back({0, xw});
      uint32_t x = pre.r[xr];
      if (!(xw & 0x800)) x = (uint32_t)(int32_t)(int16_t)x;
      addr = (form == 3 ? pre.r[10] : pc + 4) + (int8_t)xw + (x << ((xw >> 9) & 3));
      break;
    }
    case 4: addr = 0x30000 + (rnd() & 0xffff); words.push_back({0, addr >> 16}); words.push_back({0, addr & 0xffff}); break;
    case 5: { int16_t d = (int16_t)rnd(); words.push_back({0, (uint16_t)d}); addr = pc + 4 + d; break; }
    case 7: {   // full format: base A3 + bd.W + Xn*scale, no memory indirection
      xw = (uint16_t)((xr << 12) | (rnd() & 0x0e00) | 0x100 | 0x20);
      int16_t bd = (int16_t)rnd();
      words.push_back({0, xw}); words.push_back({0, (uint16_t)bd});
      uint32_t x = pre.r[xr];
      if (!(xw & 0x800)) x = (uint32_t)(int32_t)(int16_t)x;
      addr = pre.r[11] + bd + (x << ((xw >> 9) & 3));
      break;
    }
    }
    uint32_t field;                  // left-justified
    uint8_t bytes[5];
    int nb = 0;
    if (!form) {
      int o = offset & 31;
      uint32_t v = pre.r[dreg];
      field = o ? (v << o) | (v >> (32 - o)) : v;
    } else {
      addr += offset >> 3;
      int bo = offset & 7;
      nb = (bo + width + 7) >> 3;
      uint64_t v = 0;
      for (int b = 0; b < 5; b++) { bytes[b] = (uint8_t)rnd(); v = (v << 8) | bytes[b]; }
      field = (uint32_t)(v >> (8 - bo));
    }
    uint32_t val = width == 32 ? field : field >> (32 - width);
    uisa_m68k_state post = pre;
    uint16_t ccr = pre.sr & 0x10;
    if (val == 0) ccr |= 4;
    if (field & 0x80000000u) ccr |= 8;
    post.sr = (uint16_t)((pre.sr & 0xff00) | ccr);
    post.pc = pc + 4 + 2 * (uint32_t)words.size();
    if (kind == 1) post.r[dn] = val;
    if (kind == 3) post.r[dn] = width == 32 ? field : (uint32_t)((int32_t)field >> (32 - width));
    if (kind == 5) {
      int pos = 0;
      while (pos < width && !(field & (0x80000000u >> pos))) pos++;
      post.r[dn] = (uint32_t)offset + pos;
    }
    uint64_t before = uisa_shadow_count(op, 0);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_log(UISA_LOG_IFETCH, pc + 2, 2, ext);
    for (size_t w = 0; w < words.size(); w++) uisa_shadow_log(UISA_LOG_IFETCH, pc + 4 + 2 * (uint32_t)w, 2, words[w].second);
    for (int b = 0; b < nb; b++) uisa_shadow_log(UISA_LOG_READ, addr + b, 1, bytes[b]);
    uisa_shadow_post(&post);
    if (form == 7 && !full_ok) continue;
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random bf read", op, ext, form);
  }
  return bad;
}

// BFTST/BFEXTU/BFEXTS/BFFFO on Dn, (A0) and (d16,A1). Also checks that the
// front end's second-level decode sends each case where it should: immediate
// offset and width (and, for memory, a field within 4 bytes; never BFFFO on
// memory) to a PLA flow, everything else to millicode.
// MOVEC Rc,Rn / Rn,Rc for every control register, D/A general registers and
// both M states. In supervisor state with M = 0 the front end issues a PLA
// flow; with M = 1 it enters millicode.
static int random_movec(int n) {
  static const int ctl[16] = {UISA_CTL_SFC, UISA_CTL_DFC, UISA_CTL_CACR, UISA_CTL_TC, UISA_CTL_ITT0, UISA_CTL_ITT1,
                              UISA_CTL_DTT0, UISA_CTL_DTT1, UISA_CTL_USP, UISA_CTL_VBR, -1, UISA_CTL_MSP,
                              UISA_CTL_ISP, UISA_CTL_MMUSR, UISA_CTL_URP, UISA_CTL_SRP};
  int bad = 0;
  for (int i = 0; i < n; i++) {
    int v = rnd() % 16;
    if (v == 10) continue;
    bool write = rnd() & 1, m = rnd() % 8 == 0;
    int rn = rnd() % 16;
    uint16_t op = write ? 0x4e7b : 0x4e7a, code = (uint16_t)((v & 8) << 8 | (v & 7));
    uint16_t ext = (uint16_t)(rn << 12 | code);
    uisa_m68k_state pre = st(0x2600, (uint16_t)(0x2700 | (m ? 0x1000 : 0) | (rnd() & 0x1f)));
    for (int r = 0; r < 16; r++) pre.r[r] = rnd_val();
    for (int c = 0; c < UISA_CTL_NUM; c++) pre.ctl[c] = rnd_val();
    uisa_m68k_state post = pre;
    post.pc = 0x2604;
    int c = ctl[v];
    bool active = (c == UISA_CTL_ISP && !m) || (c == UISA_CTL_MSP && m);   // the active A7
    if (write) {
      // the 68040 keeps only a control register's implemented bits
      uint32_t wmask = c == UISA_CTL_SFC || c == UISA_CTL_DFC ? 0x7u
                     : c == UISA_CTL_CACR ? 0x80008000u
                     : c == UISA_CTL_TC ? 0xc000u
                     : c >= UISA_CTL_ITT0 && c <= UISA_CTL_DTT1 ? 0xffffe364u
                     : c == UISA_CTL_URP || c == UISA_CTL_SRP ? 0xfffffe00u : 0xffffffffu;
      if (active) post.r[15] = pre.r[rn]; else post.ctl[c] = pre.r[rn] & wmask;
    } else {
      post.r[rn] = active ? pre.r[15] : pre.ctl[c];
    }
    uint64_t before = uisa_shadow_count(op, 0), pla = uisa_shadow_kind_total(1);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_log(UISA_LOG_IFETCH, 0x2602, 2, ext);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random movec", op, ext, 0);
    if ((uisa_shadow_kind_total(1) != pla + 1) != m && bad++ < 5)
      ref_fail(m ? "random movec: expected millicode" : "random movec: expected a PLA flow", op, ext, 0);
  }
  return bad;
}

// MOVE SR,Dn / MOVE Dn,SR / MOVE #imm,SR in supervisor state with M = 0,
// including moves to user state (A7 becomes the USP, the ISP takes the old
// A7). These all take PLA flows. (The new SR keeps M = 0 and T = 0.)
static int random_sr(int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    int form = rnd() % 3, r = rnd() % 8;
    uint16_t op = (uint16_t)(form == 0 ? 0x40c0 | r : form == 1 ? 0x46c0 | r : 0x46fc);
    uisa_m68k_state pre = st(0x2800, (uint16_t)(0x2000 | (rnd() & 0x71f)));
    for (int k = 0; k < 16; k++) pre.r[k] = rnd_val();
    for (int c = 0; c < UISA_CTL_NUM; c++) pre.ctl[c] = rnd_val();
    uint16_t nv = (uint16_t)((rnd() & 0x271f) | (rnd() % 4 ? 0x2000 : 0));
    if (form == 1) pre.r[r] = (pre.r[r] & 0xffff0000) | nv;
    uisa_m68k_state post = pre;
    post.pc = form == 2 ? 0x2804 : 0x2802;
    if (form == 0) {
      post.r[r] = (pre.r[r] & 0xffff0000) | pre.sr;
    } else {
      post.sr = nv;
      if (!(nv & 0x2000)) { post.ctl[UISA_CTL_ISP] = pre.r[15]; post.r[15] = pre.ctl[UISA_CTL_USP]; }
    }
    uint64_t before = uisa_shadow_count(op, 0), pla = uisa_shadow_kind_total(1);
    uisa_shadow_pre(op, &pre);
    if (form == 2) uisa_shadow_log(UISA_LOG_IFETCH, 0x2802, 2, nv);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random sr", op, nv, 0);
    if (uisa_shadow_kind_total(1) != pla + 1 && bad++ < 5) ref_fail("random sr: expected a PLA flow", op, nv, 0);
  }
  return bad;
}

// MULU.L / MULS.L, 32- and 64-bit results, over Dn, (A0), (A1)+, (d16,A1),
// abs.L, #imm and (d16,PC), including Dh = Dl (the high half wins). All take
// PLA flows.
static int random_mull(int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    int form = rnd() % 7, dn = rnd() % 8;
    static const uint16_t eas[7] = {0, 0x10, 0x19, 0x29, 0x39, 0x3c, 0x3a};
    uint16_t op = (uint16_t)(0x4c00 | eas[form] | (form == 0 ? dn : 0));
    bool sgn = rnd() & 1, wide = rnd() & 1;
    int dl = rnd() % 8, dh = rnd() % 4 == 0 ? dl : rnd() % 8;
    uint16_t ext = (uint16_t)(dl << 12 | (sgn ? 0x800 : 0) | (wide ? 0x400 : 0) | dh);
    uisa_m68k_state pre = st(0x2a00, (uint16_t)(0x2700 | (rnd() & 0x1f)));
    for (int k = 0; k < 16; k++) pre.r[k] = rnd_val();
    pre.r[8] = 0x10000;
    pre.r[9] = 0x20000;
    int16_t d16 = (int16_t)((int)(rnd() % 64) - 32) & ~3;
    uint32_t mem = rnd_val(), addr = 0;
    int ilen = 4;
    switch (form) {
      case 1: addr = pre.r[8]; break;
      case 2: addr = pre.r[9]; break;
      case 3: addr = pre.r[9] + d16; ilen = 6; break;
      case 4: addr = 0x30000 + (rnd() % 64) * 4; ilen = 8; break;
      case 5: ilen = 8; break;
      case 6: addr = 0x2a04 + d16; ilen = 6; break;
    }
    uint32_t a = pre.r[dl], b = form == 0 ? pre.r[dn] : mem;
    uisa_m68k_state post = pre;
    if (form == 2) post.r[9] += 4;
    uint64_t p = sgn ? (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b) : (uint64_t)a * b;
    uint32_t lo = (uint32_t)p, hi = (uint32_t)(p >> 32);
    uint16_t ccr = pre.sr & 0x10;
    if (wide) {
      post.r[dl] = lo;
      post.r[dh] = hi;
      ccr |= (hi >> 31 ? 8 : 0) | (p == 0 ? 4 : 0);
    } else {
      post.r[dl] = lo;
      bool v = sgn ? (int64_t)p != (int64_t)(int32_t)lo : hi != 0;
      ccr |= (lo >> 31 ? 8 : 0) | (lo == 0 ? 4 : 0) | (v ? 2 : 0);
    }
    post.sr = (pre.sr & 0xff00) | ccr;
    post.pc = 0x2a00 + ilen;
    uint64_t before = uisa_shadow_count(op, 0), pla = uisa_shadow_kind_total(1);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_log(UISA_LOG_IFETCH, 0x2a02, 2, ext);
    if (form == 3 || form == 6) uisa_shadow_log(UISA_LOG_IFETCH, 0x2a04, 2, (uint16_t)d16);
    if (form == 4) { uisa_shadow_log(UISA_LOG_IFETCH, 0x2a04, 2, addr >> 16); uisa_shadow_log(UISA_LOG_IFETCH, 0x2a06, 2, addr & 0xffff); }
    if (form == 5) { uisa_shadow_log(UISA_LOG_IFETCH, 0x2a04, 2, mem >> 16); uisa_shadow_log(UISA_LOG_IFETCH, 0x2a06, 2, mem & 0xffff); }
    if (form != 0 && form != 5) uisa_shadow_log(UISA_LOG_READ, addr, 4, mem);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random mull", op, ext, 0);
    if (uisa_shadow_kind_total(1) != pla + 1 && bad++ < 5) ref_fail("random mull: expected a PLA flow", op, ext, 0);
  }
  return bad;
}

// DIVU.L / DIVS.L with a 32-bit dividend on the PLA path: Dr = Dq or not,
// over Dn, (A1)+, (d16,A1) and #imm, with signed overflow and zero divisors.
// A zero divisor traps once the instruction retires: the (A1)+ update stands,
// C is cleared, and vector 5 stacks a format 2 frame with the next PC.
static int random_divl_pla(int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    int form = rnd() % 4, sreg = rnd() % 8;
    static const uint16_t eas[4] = {0, 0x19, 0x29, 0x3c};
    uint16_t op = (uint16_t)(0x4c40 | eas[form] | (form == 0 ? sreg : 0));
    bool sgn = rnd() & 1;
    int dq = rnd() % 8, dr = rnd() % 4 == 0 ? dq : rnd() % 8;
    uint16_t ext = (uint16_t)(dq << 12 | (sgn ? 0x800 : 0) | dr);
    uisa_m68k_state pre = st(0x2c00, (uint16_t)(0x2700 | (rnd() & 0x1f)));
    for (int k = 0; k < 8; k++) pre.r[k] = rnd_val();
    pre.r[9] = 0x20000;
    pre.r[15] = 0x8000;
    pre.ctl[UISA_CTL_VBR] = 0x1000;
    int16_t d16 = (int16_t)(((int)(rnd() % 64) - 32) & ~3);
    uint32_t src = rnd_val(), addr = 0;
    int ilen = 4;
    if (form == 1) addr = pre.r[9];
    if (form == 2) { addr = pre.r[9] + d16; ilen = 6; }
    if (form == 3) ilen = 8;
    int pick = rnd() % 8;
    if (pick == 0) src = 0;
    if (pick == 1 && sgn) { src = 0xffffffff; pre.r[dq] = 0x80000000; }
    if (form == 0) { if (sreg == dq) src = pre.r[dq]; else pre.r[sreg] = src; }
    uisa_m68k_state post = pre;
    if (form == 1) post.r[9] += 4;
    std::vector<std::pair<int, std::pair<uint32_t, uint32_t>>> w;   // stack writes: size, addr, value
    uint32_t vec_read = 0;
    if (src != 0) {
      ref_divl(post, src, ext);
      post.pc = 0x2c00 + ilen;
    } else {
      post.sr &= ~1;
      w.push_back({2, {0x7ff4, post.sr}});
      w.push_back({4, {0x7ff6, 0x2c00u + ilen}});
      w.push_back({2, {0x7ffa, 0x2014}});
      w.push_back({4, {0x7ffc, 0x2c00}});
      post.r[15] = 0x7ff4;
      post.pc = 0x6300;
      vec_read = 0x6300;
    }
    uint64_t before = uisa_shadow_count(op, 0), pla = uisa_shadow_kind_total(1);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_log(UISA_LOG_IFETCH, 0x2c02, 2, ext);
    if (form == 2) uisa_shadow_log(UISA_LOG_IFETCH, 0x2c04, 2, (uint16_t)d16);
    if (form == 3) { uisa_shadow_log(UISA_LOG_IFETCH, 0x2c04, 2, src >> 16); uisa_shadow_log(UISA_LOG_IFETCH, 0x2c06, 2, src & 0xffff); }
    if (form == 1 || form == 2) uisa_shadow_log(UISA_LOG_READ, addr, 4, src);
    if (vec_read) uisa_shadow_log(UISA_LOG_READ, 0x1014, 4, vec_read);
    for (auto &x : w) uisa_shadow_log(UISA_LOG_WRITE, x.second.first, x.first, x.second.second);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail(src ? "random divl pla" : "random divl pla (zero)", op, ext, 0);
    if (uisa_shadow_kind_total(1) != pla + 1 && bad++ < 5) ref_fail("random divl pla: expected a PLA flow", op, ext, 0);
  }
  return bad;
}

// MOVES <ea>,Rn / Rn,<ea> in supervisor state, B/W/L, over (A0), (A1)+,
// -(A1), (d16,A1) and abs.W, with Rn sometimes the EA's address register:
// a load writes Rn after the An update, a store stores Rn's old value. A
// byte or word load merges into Dn and sign-extends into An. All PLA flows.
static int random_moves(int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    int form = rnd() % 5, sz = rnd() % 3, nb = 1 << sz;
    static const uint16_t eas[5] = {0x10, 0x19, 0x21, 0x29, 0x38};
    uint16_t op = (uint16_t)(0x0e00 | sz << 6 | eas[form]);
    bool store = rnd() & 1;
    int rn = rnd() % 4 == 0 ? (form == 0 ? 8 : 9) : rnd() % 16;
    uint16_t ext = (uint16_t)(rn << 12 | (store ? 0x800 : 0));
    uisa_m68k_state pre = st(0x2e00, (uint16_t)(0x2700 | (rnd() & 0x1f)));
    for (int k = 0; k < 16; k++) pre.r[k] = rnd_val();
    pre.r[8] = 0x10000;
    pre.r[9] = 0x20000 + (rnd() % 16) * 4;
    pre.r[15] = 0x8000;
    pre.ctl[UISA_CTL_SFC] = 1;
    pre.ctl[UISA_CTL_DFC] = 1;
    int16_t d16 = (int16_t)(((int)(rnd() % 64) - 32) & ~3);
    uint32_t addr = 0;
    int ilen = 4;
    uisa_m68k_state post = pre;
    switch (form) {
      case 0: addr = pre.r[8]; break;
      case 1: addr = pre.r[9]; post.r[9] += nb; break;
      case 2: addr = pre.r[9] - nb; post.r[9] = addr; break;
      case 3: addr = pre.r[9] + d16; ilen = 6; break;
      case 4: addr = 0x3000 + (rnd() % 64) * 4; ilen = 6; break;
    }
    uint32_t m = nb == 4 ? 0xffffffffu : (1u << (8 * nb)) - 1, v = rnd_val() & m;
    if (store) {
      v = pre.r[rn] & m;
    } else if (rn >= 8) {
      post.r[rn] = nb == 1 ? (uint32_t)(int8_t)v : nb == 2 ? (uint32_t)(int16_t)v : v;
    } else {
      post.r[rn] = (pre.r[rn] & ~m) | v;
    }
    post.pc = 0x2e00 + ilen;
    uint64_t before = uisa_shadow_count(op, 0), pla = uisa_shadow_kind_total(1);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_log(UISA_LOG_IFETCH, 0x2e02, 2, ext);
    if (form == 3) uisa_shadow_log(UISA_LOG_IFETCH, 0x2e04, 2, (uint16_t)d16);
    if (form == 4) uisa_shadow_log(UISA_LOG_IFETCH, 0x2e04, 2, addr);
    uisa_shadow_log(store ? UISA_LOG_WRITE : UISA_LOG_READ, addr, nb, v);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random moves", op, ext, 0);
    if (uisa_shadow_kind_total(1) != pla + 1 && bad++ < 5) ref_fail("random moves: expected a PLA flow", op, ext, 0);
  }
  return bad;
}

// 68k condition code cc (0-15) on CCR flags
static bool ref_cc(int cc, uint16_t ccr) {
  bool c = ccr & 1, v = ccr & 2, z = ccr & 4, nf = ccr & 8;
  switch (cc) {
    case 0: return true;   case 1: return false;
    case 2: return !c && !z; case 3: return c || z;
    case 4: return !c;     case 5: return c;
    case 6: return !z;     case 7: return z;
    case 8: return !v;     case 9: return v;
    case 10: return !nf;   case 11: return nf;
    case 12: return nf == v; case 13: return nf != v;
    case 14: return !z && nf == v; default: return z || nf != v;
  }
}

// DBcc Dn,disp for every condition, flag state and count (0 and 0xffff
// included): cc true falls through untouched; otherwise Dn.W -= 1 and the
// branch is taken unless the old count was 0.
static int random_dbcc(int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    int cc = rnd() % 16, r = rnd() % 8;
    uint16_t op = (uint16_t)(0x50c8 | cc << 8 | r);
    int16_t disp = (int16_t)(((int)(rnd() % 256) - 128) & ~1);
    uisa_m68k_state pre = st(0x3000, (uint16_t)(0x2700 | (rnd() & 0x1f)));
    for (int k = 0; k < 16; k++) pre.r[k] = rnd_val();
    int pick = rnd() % 4;
    if (pick == 0) pre.r[r] &= 0xffff0000;
    if (pick == 1) pre.r[r] |= 0xffff;
    uisa_m68k_state post = pre;
    post.pc = 0x3004;
    if (!ref_cc(cc, pre.sr & 0x1f)) {
      uint16_t cnt = (uint16_t)pre.r[r];
      post.r[r] = (pre.r[r] & 0xffff0000) | (uint16_t)(cnt - 1);
      if (cnt != 0) post.pc = 0x3002 + disp;
    }
    uint64_t before = uisa_shadow_count(op, 0), pla = uisa_shadow_kind_total(1);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_log(UISA_LOG_IFETCH, 0x3002, 2, (uint16_t)disp);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random dbcc", op, (uint16_t)disp, 0);
    if (uisa_shadow_kind_total(1) != pla + 1 && bad++ < 5) ref_fail("random dbcc: expected a PLA flow", op, 0, 0);
  }
  return bad;
}

// CMPM (Ay)+,(Ax)+ in B/W/L, including Ax = Ay (the second read is from the
// incremented address) and A7 bytes (steps of 2). Flags of Ax-value - Ay-value.
static int random_cmpm(int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    int sz = rnd() % 3, nb = 1 << sz, ay = rnd() % 8, ax = rnd() % 4 == 0 ? ay : rnd() % 8;
    uint16_t op = (uint16_t)(0xb108 | ax << 9 | sz << 6 | ay);
    uisa_m68k_state pre = st(0x3200, (uint16_t)(0x2700 | (rnd() & 0x1f)));
    for (int k = 0; k < 8; k++) pre.r[k] = rnd_val();
    for (int k = 8; k < 16; k++) pre.r[k] = 0x40000 + (uint32_t)(k - 8) * 0x1000 + (rnd() % 16) * 4;
    int incy = (nb == 1 && ay == 7) ? 2 : nb, incx = (nb == 1 && ax == 7) ? 2 : nb;
    uint32_t m = nb == 4 ? 0xffffffffu : (1u << (8 * nb)) - 1;
    uint32_t sa = pre.r[8 + ay], da = (ax == ay ? sa + incy : pre.r[8 + ax]);
    uint32_t sv = rnd_val() & m, dv = rnd() % 4 == 0 ? sv : rnd_val() & m;
    uisa_m68k_state post = pre;
    post.r[8 + ay] += incy;
    post.r[8 + ax] = da + incx;
    uint32_t res = (dv - sv) & m, sb = 1u << (8 * nb - 1);
    uint16_t ccr = pre.sr & 0x10;
    if (res & sb) ccr |= 8;
    if (res == 0) ccr |= 4;
    if (((dv ^ sv) & (dv ^ res)) & sb) ccr |= 2;
    if (sv > dv) ccr |= 1;
    post.sr = (pre.sr & 0xff00) | ccr;
    post.pc = 0x3202;
    uint64_t before = uisa_shadow_count(op, 0), pla = uisa_shadow_kind_total(1);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_log(UISA_LOG_READ, sa, nb, sv);
    uisa_shadow_log(UISA_LOG_READ, da, nb, dv);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random cmpm", op, 0, 0);
    if (uisa_shadow_kind_total(1) != pla + 1 && bad++ < 5) ref_fail("random cmpm: expected a PLA flow", op, 0, 0);
  }
  return bad;
}

// CAS Dc,Du,<ea> for each size and memory-alterable mode, with equal and
// unequal operands (millicode): an equal compare stores Du, an unequal one
// loads the operand into Dc's low bits and stores nothing.
static int random_cas(int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    int sz = rnd() % 3, nb = 1 << sz, form = rnd() % 6, an = rnd() % 8;
    int dc = rnd() % 8, du = rnd() % 4 == 0 ? dc : rnd() % 8;
    static const int modes[6][2] = {{2, -1}, {3, -1}, {4, -1}, {5, -1}, {7, 0}, {7, 1}};
    int mode = modes[form][0], reg = modes[form][1] < 0 ? an : modes[form][1];
    uint16_t op = (uint16_t)(0x08c0 | (sz + 1) << 9 | mode << 3 | reg);
    uint16_t ext = (uint16_t)(du << 6 | dc);
    uisa_m68k_state pre = st(0x3400, (uint16_t)(0x2700 | (rnd() & 0x1f)));
    for (int k = 0; k < 8; k++) pre.r[k] = rnd_val();
    for (int k = 8; k < 16; k++) pre.r[k] = 0x50000 + (uint32_t)(k - 8) * 0x1000 + (rnd() % 16) * 4;
    uint32_t m = nb == 4 ? 0xffffffffu : (1u << (8 * nb)) - 1;
    int inc = (nb == 1 && an == 7) ? 2 : nb;
    uisa_m68k_state post = pre;
    uint32_t ea = 0;
    int len = 4;
    int16_t d16 = (int16_t)(rnd() % 512 - 256);
    switch (form) {
    case 0: ea = pre.r[8 + an]; break;
    case 1: ea = pre.r[8 + an]; post.r[8 + an] += inc; break;
    case 2: ea = pre.r[8 + an] - inc; post.r[8 + an] = ea; break;
    case 3: ea = pre.r[8 + an] + (uint32_t)(int32_t)d16; len = 6; break;
    case 4: ea = 0x6000 + (rnd() % 64) * 2; len = 6; break;
    case 5: ea = 0x00070000 + (rnd() % 64) * 2; len = 8; break;
    }
    uint32_t v = rnd() % 3 == 0 ? pre.r[dc] & m : rnd_val() & m, c = pre.r[dc] & m;
    bool eq = v == c;
    uint32_t res = (v - c) & m, sb = 1u << (8 * nb - 1);
    uint16_t ccr = pre.sr & 0x10;
    if (res & sb) ccr |= 8;
    if (res == 0) ccr |= 4;
    if (((v ^ c) & (v ^ res)) & sb) ccr |= 2;
    if (c > v) ccr |= 1;
    post.sr = (pre.sr & 0xff00) | ccr;
    if (!eq) post.r[dc] = (pre.r[dc] & ~m) | v;
    post.pc = 0x3400 + (uint32_t)len;
    uint64_t before = uisa_shadow_count(op, 0);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_log(UISA_LOG_IFETCH, 0x3402, 2, ext);
    if (form == 3 || form == 4) uisa_shadow_log(UISA_LOG_IFETCH, 0x3404, 2, form == 3 ? (uint16_t)d16 : ea);
    if (form == 5) uisa_shadow_log(UISA_LOG_IFETCH, 0x3404, 4, ea);
    uisa_shadow_log(UISA_LOG_READ, ea, nb, v);
    if (eq) uisa_shadow_log(UISA_LOG_WRITE, ea, nb, pre.r[du] & m);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) {
      printf("  cas: size %d form %d dc %d du %d eq %d\n", nb, form, dc, du, eq);
      ref_fail("random cas", op, ext, 0);
    }
  }
  return bad;
}

static int random_bfpath(int n) {
  int bad = 0;
  const int kinds[4] = {0, 1, 3, 5};   // tst extu exts ffo
  for (int i = 0; i < n; i++) {
    int kind = kinds[rnd() % 4];
    int mode = rnd() % 3;              // 0 Dn, 1 (A0), 2 (d16,A1)
    int dreg = rnd() % 8, xreg = rnd() % 8, oreg = rnd() % 8, wreg = rnd() % 8;
    uint16_t op = (uint16_t)(0xe8c0 | (kind << 8) | (mode == 0 ? dreg : mode == 1 ? 0x10 : 0x29));
    bool dyn_o = rnd() % 4 == 0, dyn_w = rnd() % 4 == 0;
    uint16_t ext = (uint16_t)((xreg << 12) | (dyn_o ? 0x800 | (oreg << 6) : (rnd() % 32) << 6) |
                              (dyn_w ? 0x20 | wreg : rnd() % 32));
    if (rnd() % 3 == 0) {   // a byte-aligned whole long
      dyn_o = dyn_w = false;
      ext = (uint16_t)((xreg << 12) | ((rnd() % 4) * 8) << 6);
    }
    int16_t d16 = (int16_t)((int)(rnd() % 64) - 32);
    int ilen = mode == 2 ? 6 : 4;
    uisa_m68k_state pre = st(0x2400, 0x2700 | (rnd() & 0x1f));
    for (int r = 0; r < 8; r++) pre.r[r] = rnd_val();
    if (dyn_o) pre.r[oreg] = mode ? (uint32_t)((int32_t)(rnd() % 200) - 100) : rnd();
    pre.r[8] = 0x10000;
    pre.r[9] = 0x20000;
    pre.r[15] = 0x8000;
    uisa_m68k_state post = pre;
    int32_t offset = dyn_o ? (int32_t)pre.r[oreg] : (ext >> 6) & 31;
    int width = (((dyn_w ? pre.r[wreg] : ext) - 1) & 31) + 1;
    uint32_t old;
    uint8_t bytes[5];
    uint32_t addr = 0;
    int nb = 0;
    if (mode == 0) {
      uint32_t v = pre.r[dreg];
      int o = offset & 31;
      uint32_t t = o ? (v << o) | (v >> (32 - o)) : v;
      old = t >> (32 - width);
    } else {
      addr = (mode == 1 ? pre.r[8] : pre.r[9] + d16) + (offset >> 3);
      int bo = offset & 7;
      nb = (bo + width + 7) >> 3;
      uint64_t v = 0;
      for (int b = 0; b < 5; b++) { bytes[b] = (uint8_t)rnd(); v = (v << 8) | bytes[b]; }
      old = (uint32_t)((v >> (40 - bo - width)) & ((1ull << width) - 1));
    }
    uint16_t ccr = pre.sr & 0x1f;
    ref_bf_insert_flags(old, width, ccr);
    if (kind == 1) post.r[xreg] = old;
    if (kind == 3) post.r[xreg] = width == 32 ? old : (uint32_t)((int32_t)(old << (32 - width)) >> (32 - width));
    if (kind == 5) post.r[xreg] = (uint32_t)offset + (old ? (uint32_t)(__builtin_clz(old) - (32 - width)) : (uint32_t)width);
    post.sr = (post.sr & 0xff00) | ccr;
    post.pc = 0x2400 + ilen;
    bool fast = !dyn_o && !dyn_w && (mode == 0 || (kind != 5 && nb <= 4));
    uint64_t before = uisa_shadow_count(op, 0), pla = uisa_shadow_kind_total(1);
    uisa_shadow_pre(op, &pre);
    uisa_shadow_log(UISA_LOG_IFETCH, 0x2402, 2, ext);
    if (mode == 2) uisa_shadow_log(UISA_LOG_IFETCH, 0x2404, 2, (uint16_t)d16);
    for (int b = 0; b < nb; b++) uisa_shadow_log(UISA_LOG_READ, addr + b, 1, bytes[b]);
    uisa_shadow_post(&post);
    if (uisa_shadow_count(op, 0) != before + 1 && bad++ < 5) ref_fail("random bf read", op, ext, 0);
    if ((uisa_shadow_kind_total(1) != pla + 1) != !fast && bad++ < 5)
      ref_fail(fast ? "random bf read: expected a PLA flow" : "random bf read: expected millicode", op, ext, 0);
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
  {  // TAS D2: byte 0x00 -> Z set, then 0x80
    Insn t{"tas d2", 0x4ac2, st(0x1c00, 0x271f), st(0x1c02, 0x2714), {}, 0};
    t.pre.r[2] = 0x12345600; t.post.r[2] = 0x12345680;
    run(t);
  }
  {  // TAS (A0)+: locked read-modify-write, N set
    Insn t{"tas (a0)+", 0x4ad8, st(0x1d00, 0x2700), st(0x1d02, 0x2708),
           {{UISA_LOG_READ, 1, 0x3000, 0x81}, {UISA_LOG_WRITE, 1, 0x3000, 0x81}}, 0};
    t.pre.r[8] = 0x3000; t.post.r[8] = 0x3001;
    run(t);
  }
  {  // CPUSHA both caches: no visible effect
    Insn t{"cpusha bc", 0xf4f8, st(0x1e00, 0x2700), st(0x1e02, 0x2700), {}, 0};
    run(t);
  }
  {  // PFLUSH (A1)
    Insn t{"pflush (a1)", 0xf509, st(0x1e80, 0x2700), st(0x1e82, 0x2700), {}, 0};
    t.pre.r[9] = 0x4000; t.post.r[9] = 0x4000;
    run(t);
  }
  {  // PTESTR (A2): MMUSR from the table walker (the host)
    Insn t{"ptestr (a2)", 0xf56a, st(0x1f00, 0x2700), st(0x1f02, 0x2700), {}, 0};
    t.pre.r[10] = 0x5000; t.post.r[10] = 0x5000; t.post.ctl[UISA_CTL_MMUSR] = 0x00123419;
    run(t);
  }
  {  // CPUSHA in user mode: privilege violation
    Insn t{"cpusha (user)", 0xf4f9, st(0x1f80, 0x0000), st(0x5000, 0x2000),
           {{UISA_LOG_READ, 4, 0x1020, 0x5000}, {UISA_LOG_WRITE, 2, 0x7ff8, 0x0000}, {UISA_LOG_WRITE, 4, 0x7ffa, 0x1f80},
            {UISA_LOG_WRITE, 2, 0x7ffe, 0x0020}}, 0};
    t.pre.r[15] = 0x4000; t.pre.ctl[UISA_CTL_ISP] = 0x8000; t.pre.ctl[UISA_CTL_VBR] = 0x1000;
    t.post.r[15] = 0x7ff8; t.post.ctl[UISA_CTL_USP] = 0x4000; t.post.ctl[UISA_CTL_VBR] = 0x1000;
    run(t);
  }
  {  // MOVE.L ([$10,A0,D1.L*4],$4),D2: full format, pre-indexed, word bd and od; 8 bytes
    Insn t{"move.l ([bd,a0,d1.l*4],od),d2", 0x2430, st(0x2300, 0x2700), st(0x2308, 0x2708),
           {{UISA_LOG_IFETCH, 2, 0x2302, 0x1d22}, {UISA_LOG_IFETCH, 2, 0x2304, 0x0010}, {UISA_LOG_IFETCH, 2, 0x2306, 0x0004},
            {UISA_LOG_READ, 4, 0x101c, 0x2000}, {UISA_LOG_READ, 4, 0x2004, 0xdeadbeef}}, 0};
    t.pre.r[8] = 0x1000; t.pre.r[1] = 3;
    t.post.r[8] = 0x1000; t.post.r[1] = 3; t.post.r[2] = 0xdeadbeef;
    run(t);
  }
  {  // LEA ($12345),A1 via (bd,ZA0,ZD0): base and index suppressed, long bd; 8 bytes
    Insn t{"lea (bd.l,za0,zd0),a1", 0x43f0, st(0x2400, 0x2700), st(0x2408, 0x2700),
           {{UISA_LOG_IFETCH, 2, 0x2402, 0x01f0}, {UISA_LOG_IFETCH, 4, 0x2404, 0x00012345}}, 0};
    t.post.r[9] = 0x12345;
    run(t);
  }
  {  // JSR ([A0],D1.W): post-indexed, null bd/od; return address = pc + 4
    Insn t{"jsr ([a0],d1.w)", 0x4eb0, st(0x2500, 0x2700), st(0x4002, 0x2700),
           {{UISA_LOG_IFETCH, 2, 0x2502, 0x1115}, {UISA_LOG_READ, 4, 0x3000, 0x4000}, {UISA_LOG_WRITE, 4, 0x7ffc, 0x2504}}, 0};
    t.pre.r[8] = 0x3000; t.pre.r[1] = 0xffff0002; t.pre.r[15] = 0x8000;
    t.post.r[8] = 0x3000; t.post.r[1] = 0xffff0002; t.post.r[15] = 0x7ffc;
    run(t);
  }
  {  // MULU.L ([8,A0],2),D1 in millicode: memory indirect, index suppressed; 10 bytes
    Insn t{"mulu.l ([bd,a0],od),d1", 0x4c30, st(0x2600, 0x2700), st(0x260a, 0x2700),
           {{UISA_LOG_IFETCH, 2, 0x2602, 0x1000}, {UISA_LOG_IFETCH, 2, 0x2604, 0x0162}, {UISA_LOG_IFETCH, 2, 0x2606, 0x0008},
            {UISA_LOG_IFETCH, 2, 0x2608, 0x0002}, {UISA_LOG_READ, 4, 0x5008, 0x6000}, {UISA_LOG_READ, 4, 0x6002, 7}}, 0};
    t.pre.r[8] = 0x5000; t.pre.r[1] = 6;
    t.post.r[8] = 0x5000; t.post.r[1] = 42;
    run(t);
  }
  fails += random_divl(20000);
  fails += random_divw(20000);
  fails += random_bfwrite(20000);
  fails += random_bfread(40000, BF_FULL_OK);
  fails += random_bfpath(40000);
  fails += random_movec(20000);
  fails += random_sr(20000);
  fails += random_mull(40000);
  fails += random_divl_pla(40000);
  fails += random_moves(40000);
  fails += random_dbcc(40000);
  fails += random_cmpm(40000);
  fails += random_cas(40000);
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
