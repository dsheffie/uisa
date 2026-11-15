// BFTST / BFEXTU / BFEXTS / BFFFO <ea>{offset:width}[,Dn]
//
// Compiled by ucc (compiler/). The memory address comes from the shared
// hand-written ea_addr (millicode/ea.s), which handles brief and
// full-format index words and corrects the next-instruction address for
// the latter. The field is read with at most two loads sized to the bytes
// it touches (1-5, as the 68040 reads them) and extracted with the uisa
// bitfield instructions.

struct ea_res {
  u32 addr;
  u32 next_pc;
};

// ea.s: x20 = EA field, x21 = operand size, x22 = offset of the EA's first
// extension word -> x23 = address; x4 = the next instruction's address
// (corrected for full-format index words). Reads the extension words in
// x2/x3; nests three calls deep.
extern ea_res ea_addr(u32 ea, u32 size, u32 ext_off)
    asm("x20 x21 x22 -> x23 x4; reads x2 x3 x4; clobbers x16-x19 x24-x26; depth 3");

entry void bitfield(void) {
  u32 op = __opword();
  u32 ext = __ext01() >> 16;
  u32 offset = (ext & 0x800) ? __rdg((ext >> 6) & 7) : (ext >> 6) & 31;   // Dn offsets are signed for memory
  u32 wfield = ((ext & 0x20) ? __rdg(ext & 7) : ext) & 31;                // 0 means 32
  u32 width = ((wfield - 1) & 31) + 1;

  u32 src;                                   // value the field is taken from
  u32 desc;                                  // its bitfield descriptor: offset in 4:0, width in 9:5
  u32 npc;
  if (((op >> 3) & 7) == 0) {
    src = __rdg(op & 7);                     // register fields wrap
    desc = (offset & 31) | (wfield << 5);
    npc = __next_pc();
  } else {
    ea_res r = ea_addr(op & 0x3f, 1, 4);
    npc = r.next_pc;
    u32 a = r.addr + ((s32)offset >> 3);
    u32 bit = offset & 7;
    desc = bit | (wfield << 5);
    switch (((bit + width + 7) >> 3) & 7) {  // bytes touched (1-5)
      case 1: src = __ld_8u_gd(a) << 24; break;
      case 2: src = __ld_16u_gd(a) << 16; break;
      case 3: src = (__ld_16u_gd(a) << 16) | (__ld_8u_gd(a + 2) << 8); break;
      case 4: src = __ld_32_gd(a); break;
      default:                               // 5: the field starts at bit >= 1 of the first byte
        src = (__ld_32_gd(a) << bit) | (__ld_8u_gd(a + 4) >> (8 - bit));
        desc = wfield << 5;
    }
  }

  // every access that can fault is done; from here on, commit
  u32 val = __bfextu(src, desc);
  u32 ccr = __rdccr() & CCR_X;               // keep X; V = C = 0
  if (val == 0) ccr |= CCR_Z;
  if (__bfextu(src, (desc & 31) | 32)) ccr |= CCR_N;   // the field's top bit
  __wrccr(ccr);

  u32 dn = (ext >> 12) & 7;
  switch ((op >> 8) & 7) {                   // e8 bftst, e9 bfextu, eb bfexts, ed bfffo
    case 0: break;
    case 1: __wrg(dn, val); break;
    case 3: __wrg(dn, __bfexts(src, desc)); break;
    case 5: __wrg(dn, offset + __bfffo(src, desc) - (desc & 31)); break;   // no 1 bit: offset + width
    default: __brk(7);
  }
  __wrg(GPC, npc);
  __gexit();
}
