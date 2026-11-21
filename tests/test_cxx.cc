// Checks the generated C++ header: decode against the generated vectors, and
// the guest-op semantics against hand-checked 68k results.
#include <cstdio>
#include <fstream>
#include <string>
#include "uisa.h"

using namespace uisa;

static int fails = 0;

static void check(bool ok, const char *what) {
  if (!ok) { printf("FAIL: %s\n", what); fails++; }
}

struct gv {
  const char *name; unsigned fn, n; uint32_t a, b; bool xin, zin;
  uint32_t r; int N, Z, V, C, X;   // -1 = don't care
};

// fn numbers follow spec/uisa.yaml guest_ops
static const gv GVEC[] = {
  {"add.8 7f+01",      0,  8, 0x7f, 0x01, 0, 0, 0x80, 1, 0, 1, 0, 0},
  {"add.8 ff+01",      0,  8, 0xff, 0x01, 0, 0, 0x00, 0, 1, 0, 1, 1},
  {"add.32 upper junk",0,  8, 0x12345601, 0xabcdef01, 0, 0, 0x02, 0, 0, 0, 0, 0},
  {"addx.8 sticky Z",  1,  8, 0x00, 0xff, 1, 1, 0x00, 0, 1, 0, 1, 1},
  {"addx.8 Z cleared", 1,  8, 0x00, 0xff, 1, 0, 0x00, 0, 0, 0, 1, 1},
  {"sub.16 0-1",       2, 16, 0x0000, 0x0001, 0, 0, 0xffff, 1, 0, 0, 1, 1},
  {"sub.8 80-01",      2,  8, 0x80, 0x01, 0, 0, 0x7f, 0, 0, 1, 0, 0},
  {"neg.8 80",         4,  8, 0, 0x80, 0, 0, 0x80, 1, 0, 1, 1, 1},
  {"neg.8 0",          4,  8, 0, 0x00, 0, 0, 0x00, 0, 1, 0, 0, 0},
  {"cmp.32 5,5",       6, 32, 5, 5, 1, 0, 0, 0, 1, 0, 0, 1},   // X untouched (stays 1)
  {"asl.8 40<<1",     14,  8, 0x40, 1, 0, 0, 0x80, 1, 0, 1, 0, 0},
  {"asl.8 40<<2",     14,  8, 0x40, 2, 0, 0, 0x00, 0, 1, 1, 1, 1},
  {"lsr.16 1>>1",     13, 16, 0x0001, 1, 0, 0, 0x0000, 0, 1, 0, 1, 1},
  {"lsl.8 count 0",   12,  8, 0x81, 0, 1, 0, 0x81, 1, 0, 0, 0, 1},
  {"asr.8 80>>1",     15,  8, 0x80, 1, 0, 0, 0xc0, 1, 0, 0, 0, 0},
  {"asr.8 81>>1",     15,  8, 0x81, 1, 0, 0, 0xc0, 1, 0, 0, 1, 1},
  {"rol.8 81",        16,  8, 0x81, 1, 1, 0, 0x03, 0, 0, 0, 1, 1},  // X untouched
  {"ror.8 01",        17,  8, 0x01, 1, 0, 0, 0x80, 1, 0, 0, 1, 0},
  {"roxl.8 80 x0",    18,  8, 0x80, 1, 0, 0, 0x00, 0, 1, 0, 1, 1},
  {"roxl.8 00 x1",    18,  8, 0x00, 1, 1, 0, 0x01, 0, 0, 0, 0, 0},
  {"roxr.16 count0",  19, 16, 0x1234, 0, 1, 0, 0x1234, 0, 0, 0, 1, 1},
  {"roxl.32 msb out", 18, 32, 0x80000001, 1, 0, 0, 0x00000002, 0, 0, 0, 1, 1},
  {"roxr.32 x in",    19, 32, 0x00000001, 1, 1, 0, 0x80000000, 1, 0, 0, 1, 1},
  {"abcd 45+38",      20,  8, 0x45, 0x38, 0, 1, 0x83, -1, 0, -1, 0, 0},
  {"abcd 99+01",      20,  8, 0x99, 0x01, 0, 1, 0x00, -1, 1, -1, 1, 1},
  {"sbcd 10-01",      21,  8, 0x10, 0x01, 0, 1, 0x09, -1, 0, -1, 0, 0},
  {"sbcd 00-01",      21,  8, 0x00, 0x01, 0, 1, 0x99, -1, 0, -1, 1, 1},
  {"nbcd 01",         22,  8, 0, 0x01, 0, 1, 0x99, -1, 0, -1, 1, 1},
  {"nbcd 00",         22,  8, 0, 0x00, 0, 1, 0x00, -1, 1, -1, 0, 0},
  {"mulu.16 ffff^2",  23, 16, 0xffff, 0xffff, 0, 0, 0xfffe0001, 1, 0, 0, 0, -1},
  {"muls.16 -1*2",    24, 16, 0xffff, 0x0002, 0, 0, 0xfffffffe, 1, 0, 0, 0, -1},
  {"muls.32 ovf",     24, 32, 0x10000, 0x10000, 0, 0, 0x00000000, 0, 1, 1, 0, -1},
  {"divu.16 100/7",   25, 16, 100, 7, 0, 0, 0x0002000e, 0, 0, 0, 0, -1},
  {"divu.16 ovf",     25, 16, 0x10000, 1, 0, 0, 0x10000, -1, -1, 1, 0, -1},
  {"divs.16 -7/2",    26, 16, 0xfffffff9, 2, 0, 0, 0xfffffffd, 1, 0, 0, 0, -1},
  {"divs.32 min/-1",  26, 32, 0x80000000, 0xffffffff, 0, 0, 0x80000000, -1, -1, 1, 0, -1},
  {"ext.16 80",       27, 16, 0, 0x80, 0, 0, 0xff80, 1, 0, 0, 0, -1},
  {"extb.32 7f",      28, 32, 0, 0x7f, 0, 0, 0x7f, 0, 0, 0, 0, -1},
  {"btst.32 bit4 set", 29, 32, 0x10, 4, 0, 0, 0, -1, 0, -1, -1, -1},
  {"btst.32 bit3 clr", 29, 32, 0x10, 3, 0, 0, 0, -1, 1, -1, -1, -1},
  {"bset.8 bit 9->1", 32,  8, 0x00, 9, 0, 0, 0x02, -1, 1, -1, -1, -1},
  {"bclr.32 bit31",   31, 32, 0x80000001, 31, 0, 0, 0x00000001, -1, 0, -1, -1, -1},
};

static void test_gops() {
  for (const gv &t : GVEC) {
    gop_out o = gop_eval(t.fn, t.n, t.a, t.b, t.xin, 0, t.zin, 0, 0);
    bool ok = o.r == t.r &&
      (t.N < 0 || o.N == (bool)t.N) && (t.Z < 0 || o.Z == (bool)t.Z) &&
      (t.V < 0 || o.V == (bool)t.V) && (t.C < 0 || o.C == (bool)t.C) &&
      (t.X < 0 || o.X == (bool)t.X);
    if (!ok)
      printf("  %s: r=%08x N%d Z%d V%d C%d X%d\n", t.name, o.r, o.N, o.Z, o.V, o.C, o.X);
    check(ok, t.name);
  }
  check(gop_writeback(GF_MERGE, 8, 0xdeadbe00, 0x1234) == 0xdeadbe34, "merge.8");
  check(gop_writeback(GF_MERGE, 16, 0xdead0000, 0x12345678) == 0xdead5678, "merge.16");
  check(gop_writeback(GF_ZFILL, 8, 0xdeadbeef, 0x1234) == 0x34, "zfill.8");
}

static void test_bitfields() {
  check(BF_EXTU(0xf0000000u, BF_PACK(0, 4)) == 0xf, "bfextu top nibble");
  check(BF_EXTU(0x80000001u, BF_PACK(31, 2)) == 0x3, "bfextu wraps");
  check(BF_EXTS(0x80000000u, BF_PACK(0, 4)) == 0xfffffff8u, "bfexts");
  check(BF_INS(0, 3, BF_PACK(31, 2)) == 0x80000001u, "bfins wraps");
  check(BF_INS(0xffffffffu, 0, BF_PACK(4, 8)) == 0xf00fffffu, "bfins clears");
  check(BF_FFO(0x00010000u, BF_PACK(0, 32)) == 15, "bfffo");
  check(BF_FFO(0, BF_PACK(4, 8)) == 12, "bfffo none");
  check(BF_MASK(BF_PACK(4, 8)) == 0x0ff00000u, "bfmask");
  check(BF_EXTU(0x12345678u, BF_PACK(0, 0)) == 0x12345678u, "bfextu width 32");
}

static void test_decode() {
  std::ifstream w("build/vectors.hex"), o("build/vector_ops.hex");
  std::string ws, os;
  unsigned n = 0;
  while (std::getline(w, ws) && std::getline(o, os)) {
    uint32_t word = std::stoul(ws, nullptr, 16);
    unsigned op = std::stoul(os, nullptr, 16);
    dec_t d = decode(word);
    if (d.op != op) {
      printf("  decode %08x: got %s want %s\n", word, OPS[d.op].mnem, OPS[op].mnem);
      fails++;
    }
    n++;
  }
  check(n == UISA_NUM_OPS - 1, "vector count");
  check(decode(0xc4000000u).op == UISA_ILLEGAL, "reserved major (FP, 49) is illegal");
}

// the millicode predicate u: encodings, reserved values, the op table's u bits
static void test_u() {
  for (uint32_t sp = 0; sp < 4; sp++) {
    check(decode((36u << 26) | (sp << 12) | (3u << 10)).op == UISA_ILLEGAL, "probe.r um=3 is illegal");
    check(decode((37u << 26) | (sp << 12) | (3u << 10)).op == UISA_ILLEGAL, "probe.w um=3 is illegal");
    for (uint32_t lk = 5; lk < 8; lk++)
      check(decode((54u << 26) | (sp << 12) | (lk << 9)).op == UISA_ILLEGAL, "ld.c lk 5-7 is illegal");
    for (uint32_t ul = 0; ul < 2; ul++)
      check(decode((53u << 26) | (sp << 12) | (3u << 10) | (ul << 9)).op == UISA_ILLEGAL, "st.c sz=3 is illegal");
  }
  for (uint32_t m = 49; m <= 52; m++) check(decode(m << 26).op == UISA_ILLEGAL, "majors 49-52 reserved (FP)");
  // major 48: FP ops; their FP register fields (opinfo fpregs: bit 0 rd, 1 rs1, 2 rs2)
  check(decode(48u << 26).op == UISA_FADD_D && OPS[UISA_FADD_D].fpregs == 7 && OPS[UISA_FADD_D].unit == UNIT_FPU,
        "fadd.d: major 48 fn 0, FP rd rs1 rs2");
  check(OPS[UISA_FSETHI].fpregs == 1 && OPS[UISA_FSETHI].reads_rd && OPS[UISA_FGETHI].fpregs == 2 &&
        OPS[UISA_FGETHI].writes_rd, "fsethi / fgethi register files");
  dec_t d = decode((36u << 26) | (5u << 20) | (10u << 14) | (1u << 12) | (1u << 10) | 0x3f0u);
  check(d.op == UISA_PROBE_R_US_GD && d.rd == 5 && d.rs1 == 10 && d.sp == 1 && d.imm == 0xfffffff0u, "probe.r.us.gd decode");
  d = decode((54u << 26) | (11u << 20) | (10u << 14) | (1u << 12) | (1u << 9) | 0x100u);
  check(d.op == UISA_LD_C_8S_GD && d.rd == 11 && d.imm == 0xffffff00u, "ld.c.8s.gd decode");
  d = decode((53u << 26) | (13u << 20) | (2u << 14) | (3u << 12) | (2u << 10) | (1u << 9) | 0xffu);
  check(d.op == UISA_STU_C_32_GA && d.rd == 13 && d.rs1 == 2 && d.imm == 0xff, "stu.c.32.ga decode");
  d = decode((16u << 26) | (6u << 11) | 0x7ffu);
  check(d.op == UISA_B_U && d.imm == 0xfffffffcu, "b.u decode");
  d = decode((16u << 26) | (9u << 20) | (7u << 11) | 2u);
  check(d.op == UISA_B_NU && d.imm == 8, "b.nu decode (rs1 ignored)");
  check(OPS[UISA_B_U].reads_u && !OPS[UISA_B_U].reads_rs1 && !OPS[UISA_B_U].reads_rs2, "b.u reads u only");
  check(!OPS[UISA_B_EQ].reads_u && OPS[UISA_B_EQ].reads_rs1 && OPS[UISA_B_EQ].reads_rs2, "b.eq unchanged");
  check(OPS[UISA_PROBE_R_US_GD].writes_u && !OPS[UISA_PROBE_R_US_GD].reads_u, "probe .us writes u");
  check(OPS[UISA_PROBE_W_UA_GD].writes_u && OPS[UISA_PROBE_W_UA_GD].reads_u, "probe .ua reads and writes u");
  check(!OPS[UISA_PROBE_R_GD].writes_u && !OPS[UISA_PROBE_R_GD].reads_u, "plain probe leaves u");
  check(OPS[UISA_LD_C_32_GD].reads_u && OPS[UISA_LD_C_32_GD].is_load && OPS[UISA_LD_C_32_GD].mem_bytes == 4, "ld.c.32");
  check(OPS[UISA_LD_C_16S_GD].load_signed && !OPS[UISA_LD_C_16U_GD].load_signed, "ld.c signedness");
  check(OPS[UISA_STU_C_16_GD].reads_u && OPS[UISA_STU_C_16_GD].is_store && OPS[UISA_STU_C_16_GD].lock_end &&
        OPS[UISA_STU_C_16_GD].reads_rd, "stu.c.16");
  check(!OPS[UISA_ST_C_16_GD].lock_end && !OPS[UISA_ST_C_16_GD].serialize, "st.c is an ordinary store");
  check(!OPS[UISA_RDCCR].reads_u && !OPS[UISA_WRCCR].writes_u, "rdccr/wrccr do not touch u");
  // PROBE_OK: T or R, not B; probe.w needs W clear; a user access needs S clear
  check(!PROBE_OK(0, 0, 0, 1) && PROBE_OK(1, 0, 0, 1) && PROBE_OK(2, 0, 0, 1), "probe ok: T or R");
  check(!PROBE_OK(0x801, 0, 0, 1), "probe ok: B");
  check(PROBE_OK(5, 0, 0, 1) && !PROBE_OK(5, 1, 0, 1), "probe ok: W");
  check(PROBE_OK(0x81, 0, 0, 1) && !PROBE_OK(0x81, 0, 1, 1), "probe ok: S");
  // translation off (TC.E = 0): MMUSR 0 = no TTR matched, untranslated, ok;
  // a TTR match (T, R, W from the TTR) still follows the MMUSR rule
  check(PROBE_OK(0, 0, 0, 0) && PROBE_OK(0, 1, 1, 0), "probe ok: TC.E = 0, no TTR match");
  check(PROBE_OK(0x3, 1, 0, 0) && !PROBE_OK(0x7, 1, 0, 0) && PROBE_OK(0x7, 0, 1, 0), "probe ok: TC.E = 0, TTR match");
}

int main() {
  test_gops();
  test_bitfields();
  test_decode();
  test_u();
  printf("test_cxx: %d failures\n", fails);
  return fails ? 1 : 0;
}
