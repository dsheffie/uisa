// Switch lowering: bx tables (field form and range-checked form) and the
// compare chains used otherwise.
const u32 SH = 4;

// range-checked table in a called function; cases return
u32 classify(u32 v) {
  switch (v) {
    case 0: return 7;
    case 1: case 2: return v * 3;
    case 4: return 100;
    case 9: return v + 1;
    default: return 1;
  }
}

entry void t_switch(void) {
  u32 a = __rdg(D0), b = __rdg(D1), c = __rdg(D2);

  // field form: (x >> k) & mask, k a named constant; fallthrough
  u32 r = 0;
  switch ((a >> SH) & 7) {
    case 0: r = 1; break;
    case 3: r = b; break;
    case 4: r = b ^ c;
    case 5: r += 2; break;
    case 7: r = 9; break;
  }
  __wrg(D3, r);

  // field form without a shift or default; a case outside the field
  u32 q = 5;
  switch (b & 3) {
    case 0: q = 10; break;
    case 1: q = 11; break;
    case 2: q = 12; break;
    case 9: q = 13; break;
  }
  __wrg(D4, q);

  // range-checked table on a computed value, in a loop (phis through the
  // table), with continue and default
  u32 acc = 0;
  for (u32 i = 0; i < (c & 31); i++) {
    switch ((a + i) % 13) {
      case 1: acc += i; break;
      case 2: acc ^= b; continue;
      case 3: case 6: acc = acc * 3 + 1; break;
      case 12: acc -= 7; break;
      default: acc += 2;
    }
    acc += 1;
  }
  __wrg(D5, acc);
  __wrg(D6, classify(a & 15) + classify(b));

  // a negative case: compare chain
  s32 sv = (s32)c >> 28;
  u32 t = 0;
  switch (sv) {
    case -1: t = 1; break;
    case 0: t = 2; break;
    case 3: t = 3; break;
    case -8: t = 4; break;
  }
  // two cases (chain) around a range-checked table with every slot used
  switch (a & 1) {
    case 0:
      switch (c >> 30) {
        case 0: t += 10; break;
        case 1: t += 20; break;
        case 2: t += 30; break;
        case 3: t += 40; break;
      }
      break;
    case 1: t += 5; break;
  }
  __wrg(D7, t);

  // a 64-bit value: compare chain
  u64 w = ((u64)a << 32) | b;
  u32 z = 0;
  switch (w & 3) {
    case 0: z = 1; break;
    case 1: z = 2; break;
    case 2: z = 3; break;
  }
  __wrg(A0, z);

  // field at the top of the word (k + w = 32)
  u32 y = 0;
  switch ((c >> 28) & 15) {
    case 0: y = 1; break;
    case 8: y = 2; break;
    case 15: y = 3; break;
    default: y = c;
  }
  __wrg(A1, y);
  __wrg(GPC, __next_pc());
  __gexit();
}
