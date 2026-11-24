// Frames beyond the 12-bit displacement and the addi range, stack-passed
// arguments, recursion through them, and structs by value.
struct pair { u32 a; s16 b; };
struct pair mk(u32 a, s16 b) { struct pair p; p.a = a; p.b = b; return p; }
u32 use(struct pair p, struct pair q) { return p.a * 3 + (u32)p.b + q.a * 5 - (u32)q.b; }

u32 big(u32 n) {
  u32 a[3000];              // 12000 bytes: offsets past 2047 and a frame past 8191
  u32 b[16];
  for (u32 i = 0; i < 3000; i++) a[i] = i * n + (i >> 3);
  for (u32 i = 0; i < 16; i++) b[i] = a[i * 187] ^ a[2999 - i];
  u32 s = 0;
  for (u32 i = 0; i < 16; i++) s = s * 31 + b[i];
  if (n > 1) s += big(n - 1);   // frames stack up
  return s;
}

u32 stackargs(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h, u32 i, u64 j, u32 k, u32 depth) {
  if (depth == 0) return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + (u32)j * 10 +
                         (u32)(j >> 32) * 11 + k * 12;
  return stackargs(k, a, b, c, d, e, f, g, h, j * 3 + i, i, depth - 1) + depth;
}

u32 addr(void) {             // address-taken locals and parameters
  u32 x = 5;
  u32 *p = &x;
  *p += 7;
  struct pair s = mk(9, -3);
  struct pair *ps = &s;
  ps->a *= x;
  ps->b -= 2;
  return x * 1000 + use(s, mk(1, 2));
}
u32 param_addr(u32 v, u64 w) {
  u32 *p = &v;
  u64 *q = &w;
  *p = *p * 3;
  *q = *q + v;
  return v + (u32)w + (u32)(w >> 32);
}

int main(void) {
  u32 r = big(5);
  r = r * 7 + stackargs(1, 2, 3, 4, 5, 6, 7, 8, 9, 0x100000001ull, 11, 20);
  r = r * 7 + addr();
  r = r * 7 + param_addr(41, 0xffffffff00000005ull);
  return r;
}
