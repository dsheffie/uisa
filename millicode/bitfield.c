// BFTST / BFEXTU / BFEXTS / BFFFO <ea>{offset:width}[,Dn]
//
// Compiled by ucc (compiler/). Replaces the hand-written `bitfield` routine
// of integer.s. The EA helpers are a C port of ea.s for the control
// addressing modes, which are the only memory modes these instructions take.

// The extension word at byte offset `off` (2, 4, 6 or 8) of the instruction.
u32 ext_word(u32 e01, u32 e23, u32 off) {
  if (off == 2) return e01 >> 16;
  if (off == 4) return e01 & 0xffff;
  if (off == 6) return e23 >> 16;
  if (off != 8) __brk(3);
  return e23 & 0xffff;
}

// Index register * scale + d8, from the brief extension word at `off`.
u32 ea_brief(u32 e01, u32 e23, u32 off) {
  u32 ext = ext_word(e01, e23, off);
  if (ext & 0x100) __brk(5);                 // full-format extension word: not handled yet
  u32 x = __rdg(ext >> 12);
  if (!(ext & 0x800)) x = (s16)x;            // Xn.W
  return (x << ((ext >> 9) & 3)) + (s8)ext;
}

// Address of a control-mode EA whose first extension word is at `off`.
u32 ea_control(u32 ea, u32 e01, u32 e23, u32 off) {
  u32 reg = ea & 7;
  switch ((ea >> 3) & 7) {
    case 2: return __rdg(A0 + reg);                                     // (An)
    case 5: return __rdg(A0 + reg) + (s16)ext_word(e01, e23, off);      // (d16,An)
    case 6: return __rdg(A0 + reg) + ea_brief(e01, e23, off);          // (d8,An,Xn)
    case 7:
      switch (reg) {
        case 0: return (s16)ext_word(e01, e23, off);                     // abs.W
        case 1: return (ext_word(e01, e23, off) << 16) | ext_word(e01, e23, off + 2);   // abs.L
        case 2: return __rdg(GPC) + off + (s16)ext_word(e01, e23, off);  // (d16,PC)
        case 3: return __rdg(GPC) + off + ea_brief(e01, e23, off);      // (d8,PC,Xn)
      }
      __brk(6);                                                         // #imm
  }
  __brk(4);                                                             // not a control mode
}

entry void bitfield(void) {
  u32 op = __opword();
  u32 e01 = __ext01();
  u32 ext = e01 >> 16;
  u32 offset = (ext & 0x800) ? __rdg((ext >> 6) & 7) : (ext >> 6) & 31;   // Dn offsets are signed for memory
  u32 width = (ext & 0x20) ? __rdg(ext & 7) & 31 : ext & 31;
  if (width == 0) width = 32;

  u32 field;                                 // the field, left-justified at bit 31
  if (((op >> 3) & 7) == 0) {
    field = __rol(__rdg(op & 7), offset & 31);   // register fields wrap
  } else {
    u32 a = ea_control(op & 0x3f, e01, __ext23(), 4) + ((s32)offset >> 3);
    u32 bit = offset & 7;
    u32 bytes = (bit + width + 7) >> 3;      // bytes touched: 1-5
    field = 0;
    for (u32 i = 0; i < bytes && i < 4; i++) field = (field << 8) | __ld_8u_gd(a + i);
    if (bytes < 4) field <<= (4 - bytes) * 8;
    field <<= bit;
    if (bytes == 5) field |= __ld_8u_gd(a + 4) >> (8 - bit);
  }

  // every access that can fault is done; from here on, commit
  u32 val = field >> (32 - width);           // zero-extended field
  u32 ccr = __rdccr() & CCR_X;               // keep X; V = C = 0
  if (val == 0) ccr |= CCR_Z;
  if ((s32)field < 0) ccr |= CCR_N;
  __wrccr(ccr);

  u32 dn = (ext >> 12) & 7;
  switch ((op >> 8) & 7) {                   // e8 bftst, e9 bfextu, eb bfexts, ed bfffo
    case 0: break;
    case 1: __wrg(dn, val); break;
    case 3: __wrg(dn, (s32)field >> (32 - width)); break;
    case 5: __wrg(dn, __minu(__clz(field), width) + offset); break;   // no 1 bit: offset + width
    default: __brk(7);
  }
  __wrg(GPC, __next_pc());
  __gexit();
}
