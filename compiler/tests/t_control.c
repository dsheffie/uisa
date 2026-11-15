// Loops, switch, short-circuit logic, phi cycles (swaps and rotations).
u32 collatz(u32 n) {
  u32 steps = 0;
  while (n != 1 && steps < 200) {
    if (n & 1) n = 3 * n + 1;
    else n >>= 1;
    steps++;
  }
  return steps;
}

// a three-value rotation with no register to spare: every register the
// function writes is a copy source or destination on the back edge, so the
// cycle is broken with xor swaps (ends once a zero reaches p)
u32 rot3(u32 p, u32 q, u32 r) {
  while (p != 0) {
    u32 t = p;
    p = q;
    q = r;
    r = t;
  }
  return q;
}

entry void t_control(void) {
  u32 a = __rdg(D0), b = __rdg(D1);
  u32 acc = 0;
  for (u32 i = 0; i < (a & 15); i++) {
    if (i == 3) continue;
    if (i * b > 1000 && (b & 1)) break;
    acc += i * i;
  }
  __wrg(D2, acc);
  u32 k = 0;
  do {
    k += 3;
  } while (k < (b & 63));
  __wrg(D3, k);
  u32 s = 0;
  switch (a & 7) {
    case 0: s = 10;
    case 1: s += 1; break;
    case 2:
    case 3: s = b; break;
    case 5: {
      u32 t = a + b;
      s = t * 2;
    } break;
    default: s = 99;
  }
  __wrg(D4, s);
  __wrg(D5, (a > b || b == 0) && !(a & 4) ? a - b : b - a);
  __wrg(D6, collatz((a & 255) + 1));
  s32 sa = a;
  __wrg(D7, sa < 0 ? -sa : sa);
  // phi cycles: a swap and a rotation carried around loops
  u32 p = a, q = b, r = a ^ b;
  for (u32 i = 0; i < (b & 7); i++) {
    u32 t = p;
    p = q;
    q = t;
  }
  for (u32 i = 0; i < (a & 7); i++) {
    u32 t = p;
    p = q;
    q = r;
    r = t;
  }
  __wrg(A0, p);
  __wrg(A1, q);
  __wrg(A2, r);
  // a four-value rotation (a scratch register: five moves, not nine)
  u32 w0 = a, w1 = b, w2 = a + b, w3 = a ^ 0x5a5a;
  for (u32 i = 0; i < (b & 15); i++) {
    u32 t = w0;
    w0 = w1;
    w1 = w2;
    w2 = w3;
    w3 = t;
  }
  __wrg(A4, w0 + 3 * w1 + 5 * w2 + 7 * w3);
  __wrg(A5, rot3(a | 1, b, 0) + rot3(b, 0, a));
  u32 n = 0;
  while (1) {
    n++;
    if (n > (a & 31)) break;
  }
  __wrg(A3, n);
  __wrg(GPC, __next_pc());
  __gexit();
}
