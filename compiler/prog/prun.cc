// prun: run a `ucc --program` image (compiler/prog/uimage.py) on the uisa ISS.
//
//   prun prog.hex [max-steps]
//
// Memory is flat and identity-mapped: 16 MB of the hidden space, big-endian.
// Execution starts at 0 (`_start`) and ends at `halt`; the program's result
// (main's return value, the csmith checksum) is in x10. Prints
//   result <x10, hex> steps <n>
// and exits 0, or prints the error and exits 1. Misaligned or out-of-range
// accesses, guest-space accesses and millicode-only control flow are errors.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static inline uint8_t membyte(uint32_t) { return 0; }   // no guest memory
#include "iss.h"

static const uint32_t MEM_SIZE = 16u << 20;

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: prun prog.hex [max-steps]\n");
    return 2;
  }
  uint64_t maxSteps = argc > 2 ? strtoull(argv[2], nullptr, 0) : 2000000000ull;
  std::vector<uint8_t> mem(MEM_SIZE, 0);
  FILE *f = fopen(argv[1], "r");
  if (!f) {
    perror(argv[1]);
    return 2;
  }
  char line[256];
  uint32_t addr = 0;
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    if (line[0] == '@') {
      addr = (uint32_t)strtoul(line + 1, nullptr, 16);
      continue;
    }
    uint32_t w = (uint32_t)strtoul(line, nullptr, 16);
    if (addr + 4 > MEM_SIZE) {
      fprintf(stderr, "prun: image outside memory at 0x%08x\n", addr);
      return 2;
    }
    for (int k = 0; k < 4; k++) mem[addr + k] = (uint8_t)(w >> (24 - 8 * k));
    addr += 4;
  }
  fclose(f);

  Machine m;
  uint32_t pc = 0;
  uint64_t steps = 0;
  std::string err;
  auto check = [&](uint32_t a, int n) {
    if (a % n) err = "misaligned access";
    else if (a >= MEM_SIZE || a + n > MEM_SIZE) err = "access outside memory";
    if (!err.empty()) {
      char b[64];
      snprintf(b, sizeof b, " at 0x%08x", a);
      err += b;
    }
    return err.empty();
  };
  for (;;) {
    if (++steps > maxSteps) { err = "step limit reached"; break; }
    if (pc % 4 || pc >= MEM_SIZE) { err = "bad pc"; break; }
    uint32_t w = ((uint32_t)mem[pc] << 24) | (mem[pc + 1] << 16) | (mem[pc + 2] << 8) | mem[pc + 3];
    dec_t d = decode(w);
    const opinfo &o = OPS[d.op];
    if (d.op == UISA_HALT) break;
    if (d.op == UISA_ILLEGAL) { err = "illegal instruction"; break; }
    uint32_t a = m.R[d.rs1];
    uint32_t b = o.reads_rs2 ? m.R[d.rs2] : d.imm;
    uint32_t npc = pc + 4;
    if (o.is_load || o.is_store) {
      if (d.sp != 0 || o.lock_begin || o.lock_end || o.wb != 0) { err = std::string("unsupported ") + o.mnem; break; }
      uint32_t ea = a + d.imm;
      int n = o.mem_bytes;
      if (!check(ea, n)) break;
      if (o.is_load) {
        uint32_t v = 0;
        for (int k = 0; k < n; k++) v = (v << 8) | mem[ea + k];
        if (o.load_signed) v = (uint32_t)SX(v, 8 * n);
        m.R[d.rd] = v;
      } else {
        uint32_t v = m.R[d.rd];
        for (int k = 0; k < n; k++) mem[ea + k] = (uint8_t)(v >> (8 * (n - 1 - k)));
      }
    } else if (o.gform != GF_NONE || o.is_probe || o.cti == CTI_CALL || o.cti == CTI_RET || o.cti >= CTI_GEXIT) {
      err = std::string("not a program-mode instruction: ") + o.mnem;
      break;
    } else {
      m.cur_pc = pc;
      m.next_pc = npc;
      m.base(d, a, b, d.imm);
      if (!m.err.empty()) { err = m.err; break; }
      npc = m.next_pc;
    }
    m.R[0] = 0;
    pc = npc;
  }
  if (!err.empty()) {
    printf("error %s (pc 0x%08x, step %llu)\n", err.c_str(), pc, (unsigned long long)steps);
    return 1;
  }
  printf("result %08x steps %llu\n", m.R[10], (unsigned long long)steps);
  return 0;
}
